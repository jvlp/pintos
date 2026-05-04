#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/pagedir.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/malloc.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"

static thread_func start_process NO_RETURN;
static bool load (const char *cmdline, void (**eip) (void), void **esp);
static struct child_status *child_status_create (void);
static void child_status_release (struct child_status *cs);

struct child_status
  {
    // tid real do filho depois de thread_create
    tid_t tid;
    // status que o pai recebe em wait
    int exit_status;
    // marca que o filho ja terminou
    bool exited;
    // impede wait duplicado no mesmo filho
    bool waited;
    // resultado do load para handshake de exec
    bool load_success;
    // contador de referencias pai + filho
    int ref_cnt;
    // protege campos desse registro compartilhado
    struct lock lock;
    // pai espera aqui o resultado de load
    struct semaphore load_sema;
    // pai espera aqui a saida do filho
    struct semaphore exit_sema;
    // encadeia esse filho na lista de filhos do pai
    struct list_elem elem;
  };

struct start_process_args
  {
    // copia completa da linha de comando
    char *cmdline;
    // ponteiro para o registro compartilhado pai filho
    struct child_status *wait_status;
  };

/* Starts a new thread running a user program loaded from
   FILENAME.  The new thread may be scheduled (and may even exit)
   before process_execute() returns.  Returns the new process's
   thread id, or TID_ERROR if the thread cannot be created. */
tid_t
process_execute (const char *file_name) 
{
  char *cmdline;
  char *name_copy;
  char *prog_name;
  char *save_ptr;
  struct start_process_args *args;
  struct child_status *child;
  tid_t tid;

  cmdline = NULL;
  name_copy = NULL;
  args = NULL;
  child = NULL;

  // cmdline segue para load e setup_stack na thread filha
  // name_copy existe so para extrair o nome do executavel
  cmdline = palloc_get_page (0);
  name_copy = palloc_get_page (0);
  if (cmdline == NULL || name_copy == NULL)
    {
      // se faltar memoria aborta de forma centralizada
      goto fail;
    }
  strlcpy (name_copy, file_name, PGSIZE);
  // o primeiro token vira o nome da thread no kernel
  prog_name = strtok_r (name_copy, " ", &save_ptr);
  if (prog_name == NULL)
    goto fail;
  // preserva a linha inteira para montar argc argv no filho
  strlcpy (cmdline, file_name, PGSIZE);

  // cria registro compartilhado para load e wait
  child = child_status_create ();
  if (child == NULL)
    goto fail;

  // empacota argumentos para start_process
  args = malloc (sizeof *args);
  if (args == NULL)
    goto fail;
  args->cmdline = cmdline;
  args->wait_status = child;

  tid = thread_create (prog_name, PRI_DEFAULT, start_process, args);
  if (tid == TID_ERROR)
    goto fail;

  // publica tid e registra o filho na lista do pai
  child->tid = tid;
  list_push_back (&thread_current ()->children, &child->elem);
  palloc_free_page (name_copy);
  // handshake de exec pai so retorna apos saber se load funcionou
  sema_down (&child->load_sema);
  if (!child->load_success)
    {
      // se load falhou remove da lista e retorna erro de exec
      list_remove (&child->elem);
      child_status_release (child);
      return TID_ERROR;
    }
  return tid;

 fail:
  // bloco unico de limpeza para qualquer falha parcial
  if (cmdline != NULL)
    palloc_free_page (cmdline);
  if (name_copy != NULL)
    palloc_free_page (name_copy);
  if (args != NULL)
    free (args);
  if (child != NULL)
    {
      child_status_release (child);
      child_status_release (child);
    }
  return TID_ERROR;
}

static void
child_status_release (struct child_status *cs)
{
  bool free_now = false;

  if (cs == NULL)
    return;

  // decrementa contador com lock para evitar corrida
  lock_acquire (&cs->lock);
  cs->ref_cnt--;
  if (cs->ref_cnt == 0)
    free_now = true;
  lock_release (&cs->lock);

  // libera memoria so quando pai e filho ja soltaram referencia
  if (free_now)
    free (cs);
}

static struct child_status *
child_status_create (void)
{
  struct child_status *cs = malloc (sizeof *cs);

  if (cs != NULL)
    {
      // estado inicial ate thread_create e load preencherem dados reais
      cs->tid = TID_ERROR;
      cs->exit_status = -1;
      cs->exited = false;
      cs->waited = false;
      cs->load_success = false;
      // duas referencias iniciais uma do pai e uma do filho
      cs->ref_cnt = 2;
      lock_init (&cs->lock);
      // semaforos iniciam em zero para sincronizacao bloqueante
      sema_init (&cs->load_sema, 0);
      sema_init (&cs->exit_sema, 0);
    }
  return cs;
}

/* A thread function that loads a user process and starts it
   running. */
static void
start_process (void *file_name_)
{
  struct start_process_args *args = file_name_;
  char *cmdline = args->cmdline;
  struct child_status *wait_status = args->wait_status;
  struct intr_frame if_;
  bool success;

  // liga a thread filha ao registro compartilhado criado pelo pai
  thread_current ()->wait_status = wait_status;
  // args so e usado no bootstrap e pode ser liberado cedo
  free (args);

  /* Initialize interrupt frame and load executable. */
  memset (&if_, 0, sizeof if_);
  if_.gs = if_.fs = if_.es = if_.ds = if_.ss = SEL_UDSEG;
  if_.cs = SEL_UCSEG;
  if_.eflags = FLAG_IF | FLAG_MBS;
  success = load (cmdline, &if_.eip, &if_.esp);

  // a copia da linha de comando pertence so ao filho
  palloc_free_page (cmdline);

  // publica resultado do load para process_execute
  lock_acquire (&wait_status->lock);
  wait_status->load_success = success;
  lock_release (&wait_status->lock);
  sema_up (&wait_status->load_sema);
  if (!success) 
    // se load falhar encerra sem entrar em modo usuario
    thread_exit ();

  /* Start the user process by simulating a return from an
     interrupt, implemented by intr_exit (in
     threads/intr-stubs.S).  Because intr_exit takes all of its
     arguments on the stack in the form of a `struct intr_frame',
     we just point the stack pointer (%esp) to our stack frame
     and jump to it. */
  asm volatile ("movl %0, %%esp; jmp intr_exit" : : "g" (&if_) : "memory");
  NOT_REACHED ();
}

/* Waits for thread TID to die and returns its exit status.  If
   it was terminated by the kernel (i.e. killed due to an
   exception), returns -1.  If TID is invalid or if it was not a
   child of the calling process, or if process_wait() has already
   been successfully called for the given TID, returns -1
   immediately, without waiting.

   This function will be implemented in problem 2-2.  For now, it
   does nothing. */
int
process_wait (tid_t child_tid) 
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  struct child_status *child = NULL;
  int status = -1;

  // procura o tid dentro da lista de filhos do processo atual
  for (e = list_begin (&cur->children); e != list_end (&cur->children);
       e = list_next (e))
    {
      struct child_status *candidate = list_entry (e, struct child_status, elem);
      if (candidate->tid == child_tid)
        {
          child = candidate;
          break;
        }
    }

  if (child == NULL)
    // contrato do pintos tid invalido ou nao filho retorna -1
    return -1;

  // garante que o mesmo filho nao seja esperado duas vezes
  lock_acquire (&child->lock);
  if (child->waited)
    {
      lock_release (&child->lock);
      return -1;
    }
  child->waited = true;
  lock_release (&child->lock);

  // bloqueia ate process_exit do filho sinalizar saida
  sema_down (&child->exit_sema);

  // le status final publicado pelo filho
  lock_acquire (&child->lock);
  status = child->exit_status;
  lock_release (&child->lock);

  // retira da lista do pai e libera referencia do lado pai
  list_remove (&child->elem);
  child_status_release (child);
  return status;
}

/* Free the current process's resources. */
void
process_exit (void)
{
  struct thread *cur = thread_current ();
  uint32_t *pd;
  struct list_elem *e;
  int fd;

  // mensagem exigida pelos testes de userprog
  if (cur->pagedir != NULL)
    printf ("%s: exit(%d)\n", cur->name, cur->exit_status);

  if (cur->wait_status != NULL)
    {
      struct child_status *self_status = cur->wait_status;

      // publica status final para o pai que estiver em wait
      lock_acquire (&self_status->lock);
      self_status->exit_status = cur->exit_status;
      self_status->exited = true;
      lock_release (&self_status->lock);
      sema_up (&self_status->exit_sema);
      // solta referencia do lado filho nesse registro
      child_status_release (self_status);
      cur->wait_status = NULL;
    }

  // limpa filhos que o pai nao esperou para evitar vazamento
  while (!list_empty (&cur->children))
    {
      e = list_pop_front (&cur->children);
      child_status_release (list_entry (e, struct child_status, elem));
    }

  // fecha todos os arquivos de dados abertos por esse processo
  for (fd = 2; fd < 128; fd++)
    if (cur->fd_table[fd] != NULL)
      {
        file_close (cur->fd_table[fd]);
        cur->fd_table[fd] = NULL;
      }

  // libera lock de escrita do binario e fecha executavel
  if (cur->executable != NULL)
    {
      file_allow_write (cur->executable);
      file_close (cur->executable);
      cur->executable = NULL;
    }

  /* Destroy the current process's page directory and switch back
     to the kernel-only page directory. */
  pd = cur->pagedir;
  if (pd != NULL) 
    {
      /* Correct ordering here is crucial.  We must set
         cur->pagedir to NULL before switching page directories,
         so that a timer interrupt can't switch back to the
         process page directory.  We must activate the base page
         directory before destroying the process's page
         directory, or our active page directory will be one
         that's been freed (and cleared). */
      cur->pagedir = NULL;
      pagedir_activate (NULL);
      pagedir_destroy (pd);
    }
}

/* Sets up the CPU for running user code in the current
   thread.
   This function is called on every context switch. */
void
process_activate (void)
{
  struct thread *t = thread_current ();

  /* Activate thread's page tables. */
  pagedir_activate (t->pagedir);

  /* Set thread's kernel stack for use in processing
     interrupts. */
  tss_update ();
}

/* We load ELF binaries.  The following definitions are taken
   from the ELF specification, [ELF1], more-or-less verbatim.  */

/* ELF types.  See [ELF1] 1-2. */
typedef uint32_t Elf32_Word, Elf32_Addr, Elf32_Off;
typedef uint16_t Elf32_Half;

/* For use with ELF types in printf(). */
#define PE32Wx PRIx32   /* Print Elf32_Word in hexadecimal. */
#define PE32Ax PRIx32   /* Print Elf32_Addr in hexadecimal. */
#define PE32Ox PRIx32   /* Print Elf32_Off in hexadecimal. */
#define PE32Hx PRIx16   /* Print Elf32_Half in hexadecimal. */

/* Executable header.  See [ELF1] 1-4 to 1-8.
   This appears at the very beginning of an ELF binary. */
struct Elf32_Ehdr
  {
    unsigned char e_ident[16];
    Elf32_Half    e_type;
    Elf32_Half    e_machine;
    Elf32_Word    e_version;
    Elf32_Addr    e_entry;
    Elf32_Off     e_phoff;
    Elf32_Off     e_shoff;
    Elf32_Word    e_flags;
    Elf32_Half    e_ehsize;
    Elf32_Half    e_phentsize;
    Elf32_Half    e_phnum;
    Elf32_Half    e_shentsize;
    Elf32_Half    e_shnum;
    Elf32_Half    e_shstrndx;
  };

/* Program header.  See [ELF1] 2-2 to 2-4.
   There are e_phnum of these, starting at file offset e_phoff
   (see [ELF1] 1-6). */
struct Elf32_Phdr
  {
    Elf32_Word p_type;
    Elf32_Off  p_offset;
    Elf32_Addr p_vaddr;
    Elf32_Addr p_paddr;
    Elf32_Word p_filesz;
    Elf32_Word p_memsz;
    Elf32_Word p_flags;
    Elf32_Word p_align;
  };

/* Values for p_type.  See [ELF1] 2-3. */
#define PT_NULL    0            /* Ignore. */
#define PT_LOAD    1            /* Loadable segment. */
#define PT_DYNAMIC 2            /* Dynamic linking info. */
#define PT_INTERP  3            /* Name of dynamic loader. */
#define PT_NOTE    4            /* Auxiliary info. */
#define PT_SHLIB   5            /* Reserved. */
#define PT_PHDR    6            /* Program header table. */
#define PT_STACK   0x6474e551   /* Stack segment. */

/* Flags for p_flags.  See [ELF3] 2-3 and 2-4. */
#define PF_X 1          /* Executable. */
#define PF_W 2          /* Writable. */
#define PF_R 4          /* Readable. */

static bool setup_stack (void **esp, char *cmdline);
static bool validate_segment (const struct Elf32_Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
                          uint32_t read_bytes, uint32_t zero_bytes,
                          bool writable);

/* Loads an ELF executable from FILE_NAME into the current thread.
   Stores the executable's entry point into *EIP
   and its initial stack pointer into *ESP.
   Returns true if successful, false otherwise. */
bool
load (const char *cmdline, void (**eip) (void), void **esp) 
{
  struct thread *t = thread_current ();
  struct Elf32_Ehdr ehdr;
  struct file *file = NULL;
  off_t file_ofs;
  bool success = false;
  int i;
  char *cmdline_copy = NULL;
  char *stack_cmdline = NULL;
  char *file_name;
  char *save_ptr;

  /* Allocate and activate page directory. */
  t->pagedir = pagedir_create ();
  if (t->pagedir == NULL) 
    goto done;
  process_activate ();

  cmdline_copy = palloc_get_page (0);
  stack_cmdline = palloc_get_page (0);
  if (cmdline_copy == NULL || stack_cmdline == NULL)
    goto done;
  /* strtok_r altera a string:
     - cmdline_copy: para descobrir nome do binario (filesys_open)
     - stack_cmdline: preservada para construir argc/argv em setup_stack */
  strlcpy (cmdline_copy, cmdline, PGSIZE);
  strlcpy (stack_cmdline, cmdline, PGSIZE);
  file_name = strtok_r (cmdline_copy, " ", &save_ptr);
  if (file_name == NULL)
    goto done;

  /* Open executable file. */
  file = filesys_open (file_name);
  if (file == NULL) 
    {
      printf ("load: %s: open failed\n", file_name);
      goto done; 
    }

    file_deny_write (file);    // impede modificacoes no arquivo durante execucao
    t->executable = file;
  //t->executable = NULL;

  /* Read and verify executable header. */
  if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
      || memcmp (ehdr.e_ident, "\177ELF\1\1\1", 7)
      || ehdr.e_type != 2
      || ehdr.e_machine != 3
      || ehdr.e_version != 1
      || ehdr.e_phentsize != sizeof (struct Elf32_Phdr)
      || ehdr.e_phnum > 1024) 
    {
      printf ("load: %s: error loading executable\n", file_name);
      goto done; 
    }

  /* Read program headers. */
  file_ofs = ehdr.e_phoff;
  for (i = 0; i < ehdr.e_phnum; i++) 
    {
      struct Elf32_Phdr phdr;

      if (file_ofs < 0 || file_ofs > file_length (file))
        goto done;
      file_seek (file, file_ofs);

      if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
        goto done;
      file_ofs += sizeof phdr;
      switch (phdr.p_type) 
        {
        case PT_NULL:
        case PT_NOTE:
        case PT_PHDR:
        case PT_STACK:
        default:
          /* Ignore this segment. */
          break;
        case PT_DYNAMIC:
        case PT_INTERP:
        case PT_SHLIB:
          goto done;
        case PT_LOAD:
          if (validate_segment (&phdr, file)) 
            {
              bool writable = (phdr.p_flags & PF_W) != 0;
              uint32_t file_page = phdr.p_offset & ~PGMASK;
              uint32_t mem_page = phdr.p_vaddr & ~PGMASK;
              uint32_t page_offset = phdr.p_vaddr & PGMASK;
              uint32_t read_bytes, zero_bytes;
              if (phdr.p_filesz > 0)
                {
                  /* Normal segment.
                     Read initial part from disk and zero the rest. */
                  read_bytes = page_offset + phdr.p_filesz;
                  zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
                                - read_bytes);
                }
              else 
                {
                  /* Entirely zero.
                     Don't read anything from disk. */
                  read_bytes = 0;
                  zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
                }
              if (!load_segment (file, file_page, (void *) mem_page,
                                 read_bytes, zero_bytes, writable))
                goto done;
            }
          else
            goto done;
          break;
        }
    }

  /* Set up stack. */
  if (!setup_stack (esp, stack_cmdline))
    goto done;

  /* Start address. */
  *eip = (void (*) (void)) ehdr.e_entry;

  success = true;

 done:
  /* We arrive here whether the load is successful or not. */
  if (cmdline_copy != NULL)
    palloc_free_page (cmdline_copy);
  if (stack_cmdline != NULL)
    palloc_free_page (stack_cmdline);
  return success;
}

/* load() helpers. */

static bool install_page (void *upage, void *kpage, bool writable);

/* Checks whether PHDR describes a valid, loadable segment in
   FILE and returns true if so, false otherwise. */
static bool
validate_segment (const struct Elf32_Phdr *phdr, struct file *file) 
{
  /* p_offset and p_vaddr must have the same page offset. */
  if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK)) 
    return false; 

  /* p_offset must point within FILE. */
  if (phdr->p_offset > (Elf32_Off) file_length (file)) 
    return false;

  /* p_memsz must be at least as big as p_filesz. */
  if (phdr->p_memsz < phdr->p_filesz) 
    return false; 

  /* The segment must not be empty. */
  if (phdr->p_memsz == 0)
    return false;
  
  /* The virtual memory region must both start and end within the
     user address space range. */
  if (!is_user_vaddr ((void *) phdr->p_vaddr))
    return false;
  if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
    return false;

  /* The region cannot "wrap around" across the kernel virtual
     address space. */
  if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
    return false;

  /* Disallow mapping page 0.
     Not only is it a bad idea to map page 0, but if we allowed
     it then user code that passed a null pointer to system calls
     could quite likely panic the kernel by way of null pointer
     assertions in memcpy(), etc. */
  if (phdr->p_vaddr < PGSIZE)
    return false;

  /* It's okay. */
  return true;
}

/* Loads a segment starting at offset OFS in FILE at address
   UPAGE.  In total, READ_BYTES + ZERO_BYTES bytes of virtual
   memory are initialized, as follows:

        - READ_BYTES bytes at UPAGE must be read from FILE
          starting at offset OFS.

        - ZERO_BYTES bytes at UPAGE + READ_BYTES must be zeroed.

   The pages initialized by this function must be writable by the
   user process if WRITABLE is true, read-only otherwise.

   Return true if successful, false if a memory allocation error
   or disk read error occurs. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
              uint32_t read_bytes, uint32_t zero_bytes, bool writable) 
{
  ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
  ASSERT (pg_ofs (upage) == 0);
  ASSERT (ofs % PGSIZE == 0);

  file_seek (file, ofs);
  while (read_bytes > 0 || zero_bytes > 0) 
    {
      /* Calculate how to fill this page.
         We will read PAGE_READ_BYTES bytes from FILE
         and zero the final PAGE_ZERO_BYTES bytes. */
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      /* Get a page of memory. */
      uint8_t *kpage = palloc_get_page (PAL_USER);
      if (kpage == NULL)
        return false;

      /* Load this page. */
      if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes)
        {
          palloc_free_page (kpage);
          return false; 
        }
      memset (kpage + page_read_bytes, 0, page_zero_bytes);

      /* Add the page to the process's address space. */
      if (!install_page (upage, kpage, writable)) 
        {
          palloc_free_page (kpage);
          return false; 
        }

      /* Advance. */
      read_bytes -= page_read_bytes;
      zero_bytes -= page_zero_bytes;
      upage += PGSIZE;
    }
  return true;
}

/* Create a minimal stack by mapping a zeroed page at the top of
   user virtual memory. */
static bool
setup_stack (void **esp, char *cmdline) 
{
  uint8_t *kpage;
  bool success = false;
  uint8_t *stack_bottom = ((uint8_t *) PHYS_BASE) - PGSIZE;
  char *argv[64];
  char *arg_addrs[64];
  char *token;
  char *save_ptr;
  int argc = 0;
  int i;

  // aloca pagina zerada da pilha de usuario e mapeia no topo
  // do espaco virtual de usuario (PHYS_BASE - PGSIZE ... PHYS_BASE).
  kpage = palloc_get_page (PAL_USER | PAL_ZERO);
  if (kpage != NULL) 
    {
      success = install_page (((uint8_t *) PHYS_BASE) - PGSIZE, kpage, true);
      if (success)
        *esp = PHYS_BASE;
      else
        palloc_free_page (kpage);
    }
  if (!success)
    return false;

  // tokeniza cmdline para preencher argv[] e contar argc
  for (token = strtok_r (cmdline, " ", &save_ptr); token != NULL;
       token = strtok_r (NULL, " ", &save_ptr))
    {
      if (argc >= (int) (sizeof argv / sizeof *argv))
        return false;
      argv[argc++] = token;
    }

  // copia cada string de argumento para dentro da stack
  // ordem reversa para facilitar depois a montagem de ponteiros argv[]
  for (i = argc - 1; i >= 0; i--)
    {
      size_t len = strlen (argv[i]) + 1;
      // stack cresce para baixo: decrementa esp antes de escrever.
      *esp = (uint8_t *) *esp - len;
      // protecao: nao ultrapassar a pagina unica de stack deste projeto.
      if ((uint8_t *) *esp < stack_bottom)
        return false;
      memcpy (*esp, argv[i], len);
      // salva endereco final da string para empilhar ponteiros depois.
      arg_addrs[i] = *esp;
    }

  // alinhamento em 4 bytes.
  while ((uintptr_t) *esp % sizeof (uint32_t) != 0)
    {
      *esp = (uint8_t *) *esp - 1;
      if ((uint8_t *) *esp < stack_bottom)
        return false;
      // padding com zero para limpeza/debug.
      *(uint8_t *) *esp = 0;
    }

  // sentinel argv[argc] = NULL.
  *esp = (uint8_t *) *esp - sizeof (char *);
  if ((uint8_t *) *esp < stack_bottom)
    return false;
  *(char **) *esp = NULL;

  // empilha ponteiros argv[i] em ordem reversa para que
  // argv[0] fique no menor endereco do vetor, como esperado em C.
  for (i = argc - 1; i >= 0; i--)
    {
      *esp = (uint8_t *) *esp - sizeof (char *);
      if ((uint8_t *) *esp < stack_bottom)
        return false;
      memcpy (*esp, &arg_addrs[i], sizeof (char *));
    }

  // empilha argv (char **), apontando para argv[0] na stack.
  {
    char **argv_start = (char **) *esp;
    *esp = (uint8_t *) *esp - sizeof (char **);
    if ((uint8_t *) *esp < stack_bottom)
      return false;
    memcpy (*esp, &argv_start, sizeof (char **));
  }

  // empilha argc (int).
  *esp = (uint8_t *) *esp - sizeof (int);
  if ((uint8_t *) *esp < stack_bottom)
    return false;
  memcpy (*esp, &argc, sizeof (int));

  // empilha return address fake
  // _start() nao retorna, mas o layout da pilha 
  // deve parecer uma chamada de funcao padrao
  *esp = (uint8_t *) *esp - sizeof (void *);
  if ((uint8_t *) *esp < stack_bottom)
    return false;
  *(void **) *esp = NULL;

  return true;
}

/* Adds a mapping from user virtual address UPAGE to kernel
   virtual address KPAGE to the page table.
   If WRITABLE is true, the user process may modify the page;
   otherwise, it is read-only.
   UPAGE must not already be mapped.
   KPAGE should probably be a page obtained from the user pool
   with palloc_get_page().
   Returns true on success, false if UPAGE is already mapped or
   if memory allocation fails. */
static bool
install_page (void *upage, void *kpage, bool writable)
{
  struct thread *t = thread_current ();

  /* Verify that there's not already a page at that virtual
     address, then map our page there. */
  return (pagedir_get_page (t->pagedir, upage) == NULL
          && pagedir_set_page (t->pagedir, upage, kpage, writable));
}

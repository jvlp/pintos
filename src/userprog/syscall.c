#include "userprog/syscall.h"
#include <stddef.h>
#include <stdint.h>
#include <syscall-nr.h>
#include "devices/input.h"
#include "devices/shutdown.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "lib/kernel/stdio.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"
#include "vm/page.h"
#include "vm/frame.h"
#include "threads/malloc.h"

#define FD_MIN 2
#define FD_MAX 128

// lock unico para serializar acesso ao filesys que nao e thread safe
struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);
static void syscall_exit (int status) NO_RETURN;
static void validate_user_address (const void *uaddr);
static void validate_user_buffer (const void *buffer, size_t size);
static void validate_user_string (const char *str);
static uint32_t fetch_u32 (const void *uaddr);
static struct file *fd_lookup (int fd);
static int fd_allocate (struct file *file);
static void fd_close (int fd);
static mapid_t sys_mmap (int fd, void *addr);
static void sys_munmap (mapid_t mapping);

struct mmap_desc {
    mapid_t id;
    struct file *file;
    void *addr;
    size_t size;
    struct list_elem elem;
};

void
syscall_init (void) 
{
  // inicializa lock global das syscalls de arquivo
  lock_init (&filesys_lock);
  // registra int 0x30 como entrada de syscalls de user mode
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{

  thread_current()->user_esp = f->esp;

  // numero da syscall fica em *esp da stack de usuario
  uint32_t syscall_nr = fetch_u32 (f->esp);

  switch (syscall_nr)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      syscall_exit ((int) fetch_u32 ((uint32_t *) f->esp + 1));
      break;

    case SYS_EXEC:
      {
        // valida string inteira antes de process_execute
        const char *cmdline =
          (const char *) fetch_u32 ((uint32_t *) f->esp + 1);
        validate_user_string (cmdline);
        f->eax = process_execute (cmdline);
      }
      break;

    case SYS_WAIT:
      {
        // delega semantica de wait para process_wait
        tid_t tid = (tid_t) fetch_u32 ((uint32_t *) f->esp + 1);
        f->eax = process_wait (tid);
      }
      break;

    case SYS_CREATE:
      {
        // create recebe nome e tamanho inicial
        const char *file =
          (const char *) fetch_u32 ((uint32_t *) f->esp + 1);
        unsigned initial_size =
          (unsigned) fetch_u32 ((uint32_t *) f->esp + 2);

        // nome vem da memoria de usuario e precisa ser validado byte a byte
        validate_user_string (file);
        lock_acquire (&filesys_lock);
        f->eax = filesys_create (file, initial_size);
        lock_release (&filesys_lock);
      }
      break;

    case SYS_REMOVE:
      {
        // remove apaga entrada de arquivo pelo nome
        const char *file =
          (const char *) fetch_u32 ((uint32_t *) f->esp + 1);

        validate_user_string (file);
        lock_acquire (&filesys_lock);
        f->eax = filesys_remove (file);
        lock_release (&filesys_lock);
      }
      break;

    case SYS_OPEN:
      {
        // open cria um handle kernel e mapeia para fd local do processo
        const char *file =
          (const char *) fetch_u32 ((uint32_t *) f->esp + 1);
        struct file *opened;
        int fd;

        validate_user_string (file);
        lock_acquire (&filesys_lock);
        opened = filesys_open (file);
        if (opened == NULL)
          f->eax = -1;
        else
          {
            // fd e alocado so no espaco 2..127
            fd = fd_allocate (opened);
            if (fd == -1)
              // se tabela lotou fecha o handle para nao vazar
              file_close (opened);
            f->eax = fd;
          }
        lock_release (&filesys_lock);
      }
      break;

    case SYS_FILESIZE:
      {
        // filesize opera apenas em fd de arquivo aberto
        int fd = (int) fetch_u32 ((uint32_t *) f->esp + 1);
        struct file *file = fd_lookup (fd);

        if (file == NULL)
          f->eax = -1;
        else
          {
            lock_acquire (&filesys_lock);
            f->eax = file_length (file);
            lock_release (&filesys_lock);
          }
      }
      break;

    case SYS_READ:
      {
        // read usa fd buffer size no padrao da abi
        int fd = (int) fetch_u32 ((uint32_t *) f->esp + 1);
        void *buffer = (void *) fetch_u32 ((uint32_t *) f->esp + 2);
        unsigned size = (unsigned) fetch_u32 ((uint32_t *) f->esp + 3);
        unsigned i;

        // buffer de escrita do kernel para usuario precisa ser valido
        if (size > 0)
          validate_user_buffer (buffer, size);

        if (fd == 0)
          {
            // stdin e alimentado caractere a caractere pelo dispositivo de input
            uint8_t *bytes = buffer;
            for (i = 0; i < size; i++)
              bytes[i] = input_getc ();
            f->eax = (int) size;
          }
        else if (fd == 1)
          // stdout nao pode ser lido
          f->eax = -1;
        else
          {
            struct file *file = fd_lookup (fd);
            if (file == NULL)
              f->eax = -1;
            else
              {
                lock_acquire (&filesys_lock);
                f->eax = file_read (file, buffer, size);
                lock_release (&filesys_lock);
              }
          }
      }
      break;

    case SYS_WRITE:
      {
        // write usa fd buffer size no padrao da abi
        int fd = (int) fetch_u32 ((uint32_t *) f->esp + 1);
        const void *buffer = (const void *) fetch_u32 ((uint32_t *) f->esp + 2);
        unsigned size = (unsigned) fetch_u32 ((uint32_t *) f->esp + 3);
        struct file *file;

        // buffer de leitura vindo do usuario precisa estar mapeado
        if (size > 0)
          validate_user_buffer (buffer, size);

        if (fd == 1)
          {
            // stdout escreve direto no console
            putbuf (buffer, size);
            f->eax = (int) size;
          }
        else if (fd == 0)
          // stdin nao pode ser usado como destino de write
          f->eax = -1;
        else
          {
            file = fd_lookup (fd);
            if (file == NULL)
              f->eax = -1;
            else
              {
                lock_acquire (&filesys_lock);
                f->eax = file_write (file, buffer, size);
                lock_release (&filesys_lock);
              }
          }
      }
      break;

    case SYS_SEEK:
      {
        // seek ajusta cursor interno do arquivo aberto
        int fd = (int) fetch_u32 ((uint32_t *) f->esp + 1);
        unsigned position = (unsigned) fetch_u32 ((uint32_t *) f->esp + 2);
        struct file *file = fd_lookup (fd);

        if (file != NULL)
          {
            lock_acquire (&filesys_lock);
            file_seek (file, position);
            lock_release (&filesys_lock);
          }
      }
      break;

    case SYS_TELL:
      {
        // tell retorna cursor atual de leitura escrita
        int fd = (int) fetch_u32 ((uint32_t *) f->esp + 1);
        struct file *file = fd_lookup (fd);

        if (file == NULL)
          f->eax = -1;
        else
          {
            lock_acquire (&filesys_lock);
            f->eax = file_tell (file);
            lock_release (&filesys_lock);
          }
      }
      break;

    case SYS_CLOSE:
      {
        // close sempre e idempotente para fd invalido neste baseline
        int fd = (int) fetch_u32 ((uint32_t *) f->esp + 1);

        lock_acquire (&filesys_lock);
        fd_close (fd);
        lock_release (&filesys_lock);
      }
      break;

    case SYS_MMAP:
      { // File e onde escrever
        f->eax = sys_mmap ((int) fetch_u32 ((uint32_t *) f->esp + 1),
                          (void *) fetch_u32 ((uint32_t *) f->esp + 2));
        break;
      }

    case SYS_MUNMAP:
      {
        sys_munmap ((mapid_t) fetch_u32 ((uint32_t *) f->esp + 1));
        break;
      }

    default:
      // qualquer syscall fora do escopo mata o processo com erro
      syscall_exit (-1);
      break;
    }
}

static void
syscall_exit (int status)
{
  // publica status para process_exit e encerra thread
  thread_current ()->exit_status = status;
  thread_exit ();
}

static void
validate_user_address (const void *uaddr)
{
  struct thread *cur = thread_current ();

  // ponteiro valido precisa ser de user space e estar mapeado na pagedir atual
  if (uaddr == NULL || !is_user_vaddr (uaddr))
    syscall_exit (-1);

  /* As páginas não são mais mapeadas todas, então pode apenas não ter sido
     puxada e estar na tabela de paginas (spt) */

  // Se ela já estiver mapeada é sucesso
  if (pagedir_get_page (cur->pagedir, uaddr) != NULL)
    return;  

  // Se não, e preciso verificar se a página está na tabela
  void *upage = pg_round_down (uaddr);
  struct spt_entry *entry = spt_find (&cur->spt, upage);

  // Se não existe ficha, pode ser a pilha a pedindo mais espaço
  if (entry == NULL){
    void *stack_limit = ((uint8_t *) PHYS_BASE) - (8 * 1024 * 1024);
    if (uaddr >= stack_limit && uaddr < PHYS_BASE && 
      (uint8_t *) uaddr >= (uint8_t *) cur->user_esp - 32) 
    {
        if (!spt_grow_stack(upage)) syscall_exit(-1);
        return;
    }
    syscall_exit (-1); 
  }

  // Se tem ficha na SPT mas não está carregada, forçamos a puxar do disco AGORA
  if (!entry->is_loaded)
  {
    if (!spt_load_page (entry))
      syscall_exit (-1); // Falta de memória ou erro de disco
  }
  
}

static void
validate_user_buffer (const void *buffer, size_t size)
{
  const uint8_t *uaddr = buffer;
  size_t i;

  // buffer vazio e valido por definicao da syscall
  if (size == 0)
    return;

  // valida byte a byte para cobrir fronteira de pagina
  for (i = 0; i < size; i++)
    validate_user_address (uaddr + i);
}

static void
validate_user_string (const char *str)
{
  const char *uaddr = str;

  // avanca ate encontrar terminador garantindo que cada byte e valido
  while (true)
    {
      validate_user_address (uaddr);
      if (*uaddr == '\0')
        return;
      uaddr++;
    }
}

static uint32_t
fetch_u32 (const void *uaddr)
{
  // primeiro valida 4 bytes depois le valor bruto
  validate_user_buffer (uaddr, sizeof (uint32_t));
  return *(const uint32_t *) uaddr;
}

static struct file *
fd_lookup (int fd)
{
  struct thread *cur = thread_current ();

  // fd 0 e 1 sao reservados para stdin stdout
  if (fd < FD_MIN || fd >= FD_MAX)
    return NULL;
  return cur->fd_table[fd];
}

static int
fd_allocate (struct file *file)
{
  struct thread *cur = thread_current ();
  int fd;

  // tenta alocar a partir de next_fd para reduzir busca media
  for (fd = cur->next_fd; fd < FD_MAX; fd++)
    if (cur->fd_table[fd] == NULL)
      {
        cur->fd_table[fd] = file;
        cur->next_fd = fd + 1;
        if (cur->next_fd >= FD_MAX)
          cur->next_fd = FD_MIN;
        return fd;
      }

  // faz wrap e procura no intervalo inicial
  for (fd = FD_MIN; fd < cur->next_fd; fd++)
    if (cur->fd_table[fd] == NULL)
      {
        cur->fd_table[fd] = file;
        cur->next_fd = fd + 1;
        if (cur->next_fd >= FD_MAX)
          cur->next_fd = FD_MIN;
        return fd;
      }

  return -1;
}

static void
fd_close (int fd)
{
  struct thread *cur = thread_current ();

  // close de fd fora da faixa nao faz nada
  if (fd < FD_MIN || fd >= FD_MAX)
    return;

  if (cur->fd_table[fd] != NULL)
    {
      // fecha handle kernel e libera slot na tabela do processo
      file_close (cur->fd_table[fd]);
      cur->fd_table[fd] = NULL;
      if (fd < cur->next_fd)
        cur->next_fd = fd;
    }
}

static mapid_t
sys_mmap (int fd, void *addr)
{
  struct thread *cur = thread_current ();
  struct file *f = fd_lookup (fd);

  // Validações o file, se é realmente um file
  if (f == NULL || addr == NULL || pg_ofs (addr) != 0 || fd == 0 || fd == 1) 
    return -1;

  lock_acquire (&filesys_lock);
  size_t length = file_length (f); //Vê o tamanho
  lock_release (&filesys_lock);

  if (length == 0) return -1;

  // Verificar se algum pedaço não já esta sendo usado
  size_t offset;
  for (offset = 0; offset < length; offset += PGSIZE) {
      if (spt_find (&cur->spt, (uint8_t *) addr + offset) != NULL) 
          return -1; 
  }

  // Reabrir o ficheiro para ter um ponteiro independente
  lock_acquire (&filesys_lock);
  struct file *mmap_file = file_reopen (f);
  lock_release (&filesys_lock);
  if (mmap_file == NULL) return -1;

  // Inserir as páginas na SPT
  size_t read_bytes = length;
  
  for (offset = 0; offset < length; offset += PGSIZE) {
      size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
      size_t page_zero_bytes = PGSIZE - page_read_bytes;

      // Cria a entrada e insere na spt da thread
      struct spt_entry *entry = malloc (sizeof (struct spt_entry));
      entry->upage = (uint8_t *) addr + offset;
      entry->type = PAGE_FILE;
      entry->is_loaded = false;
      entry->writable = true; // mmap pode ser escrito
      entry->file = mmap_file;
      entry->offset = offset;
      entry->read_bytes = page_read_bytes;
      entry->zero_bytes = page_zero_bytes;

      spt_insert (&cur->spt, entry);
      read_bytes -= page_read_bytes;
  }

  // Regista na lista do processo e devolver o ID
  struct mmap_desc *md = malloc (sizeof (struct mmap_desc));
  md->id = cur->next_mapid++;
  md->file = mmap_file;
  md->addr = addr;
  md->size = length;
  list_push_back (&cur->mmap_list, &md->elem);

  return md->id;
}

static void
sys_munmap (mapid_t mapping)
{
  struct thread *cur = thread_current ();
  struct list_elem *e;
  struct mmap_desc *md = NULL;

  // Procurar o mapeamento pelo ID
  for (e = list_begin (&cur->mmap_list); e != list_end (&cur->mmap_list); e = list_next (e)) {
      struct mmap_desc *temp = list_entry (e, struct mmap_desc, elem);
      if (temp->id == mapping) {
          md = temp;
          break;
      }
  }
  if (md == NULL) return;

  // Desmapear, gravar os dados sujos e destruir a SPT local
  size_t offset;
  for (offset = 0; offset < md->size; offset += PGSIZE) {
      void *upage = (uint8_t *) md->addr + offset;
      struct spt_entry *entry = spt_find (&cur->spt, upage);

      if (entry != NULL) {
          if (entry->is_loaded) {
              // Salvar se houve alguma escrita
              if (pagedir_is_dirty (cur->pagedir, upage)) {
                  lock_acquire (&filesys_lock);
                  file_write_at (entry->file, upage, entry->read_bytes, entry->offset);
                  lock_release (&filesys_lock);
              }
              // Limpar da RAM
              void *kpage = pagedir_get_page (cur->pagedir, upage);
              if (kpage) frame_free (kpage);
              pagedir_clear_page (cur->pagedir, upage);
          }
          // Remover da Tabela de Páginas Suplementar
          hash_delete (&cur->spt, &entry->elem);
          free (entry);
      }
  }

  // Fechar o file e apagar o registo
  lock_acquire (&filesys_lock);
  file_close (md->file);
  lock_release (&filesys_lock);
  
  list_remove (&md->elem);
  free (md);
}
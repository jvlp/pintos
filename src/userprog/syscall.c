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
#include "threads/synch.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/pagedir.h"
#include "userprog/process.h"

#define FD_MIN 2
#define FD_MAX 128

// lock unico para serializar acesso ao filesys que nao e thread safe
static struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);
static void syscall_exit (int status) NO_RETURN;
static void validate_user_address (const void *uaddr);
static void validate_user_buffer (const void *buffer, size_t size);
static void validate_user_string (const char *str);
static uint32_t fetch_u32 (const void *uaddr);
static struct file *fd_lookup (int fd);
static int fd_allocate (struct file *file);
static void fd_close (int fd);

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
  if (uaddr == NULL
      || !is_user_vaddr (uaddr)
      || pagedir_get_page (cur->pagedir, uaddr) == NULL)
    syscall_exit (-1);
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

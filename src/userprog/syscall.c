#include "userprog/syscall.h"
#include <stddef.h>
#include <stdint.h>
#include <syscall-nr.h>
#include "devices/shutdown.h"
#include "lib/kernel/stdio.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"
#include "userprog/process.h"
#include "userprog/pagedir.h"
#include "threads/synch.h"
#include <stdio.h>

static struct lock filesys_lock;

static void syscall_handler (struct intr_frame *);
static void syscall_exit (int status) NO_RETURN;
static void validate_user_address (const void *uaddr);
static void validate_user_buffer (const void *buffer, size_t size);
static uint32_t fetch_u32 (const void *uaddr);

void
syscall_init (void) 
{
  /* int 0x30 eh a "porta" usada pelo programa de usuario para
     entrar no kernel e pedir um servico (syscall). */
  lock_init(&filesys_lock); //Inicializa o lock para as escritas.
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
syscall_handler (struct intr_frame *f) 
{
  /* Convencao: numero da syscall fica em *esp do processo usuario. */
  uint32_t syscall_nr = fetch_u32 (f->esp);

  //printf("Syscall detectada: %d\n", syscall_nr);

  switch (syscall_nr)
    {
    case SYS_HALT:
      shutdown_power_off ();
      break;

    case SYS_EXIT:
      syscall_exit ((int) fetch_u32 ((uint32_t *) f->esp + 1));
      break;

    case SYS_WRITE:
      {
        /* Argumentos ficam logo apos o numero da syscall na pilha:
           [esp+4]=fd, [esp+8]=buffer, [esp+12]=size. */
        int fd = (int) fetch_u32 ((uint32_t *) f->esp + 1);
        const void *buffer = (const void *) fetch_u32 ((uint32_t *) f->esp + 2);
        unsigned size = (unsigned) fetch_u32 ((uint32_t *) f->esp + 3);

        /* Neste baseline inicial, so suportamos escrita no stdout (fd=1). */
        if (fd != 1)
          {
            f->eax = -1;
            break;
          }
        /* Nunca acessamos memoria de usuario sem validar antes. */
        validate_user_buffer (buffer, size);

        lock_acquire(&filesys_lock);
        /**/putbuf (buffer, size);
        lock_release(&filesys_lock);

        /* Valor de retorno de syscall vai em eax. */
        f->eax = (int) size;
      }
      break;

    case SYS_WAIT:
      {
        /* wait(pid):
           - pid vem como primeiro argumento apos o numero da syscall.
           - a logica completa fica em process_wait().
           - neste baseline, process_wait usa sincronizacao global simples
             (um slot de filho por vez), entao este caso cobre cenarios
             iniciais mas nao todos os casos concorrentes de projeto 2. */
        tid_t tid = (tid_t) fetch_u32 ((uint32_t *) f->esp + 1);
        f->eax = process_wait (tid);
      }
      break;

    case SYS_EXEC:
      {
        /* Pega o ponteiro da string de comando na pilha */
        const char *comand = (const char *) fetch_u32 ((uint32_t *) f->esp + 1);
        
        /* Valida a string (não apenas o ponteiro, mas a string inteira) */
        int i = 0;
        while (comand[i] != '\0'){
          validate_user_address (comand + i); //Valida do primerio até o último endereço
          i++;   
        }
        
        /* Executa e retorna o TID (ou -1 se falhar) */
        lock_acquire(&filesys_lock);
        /**/f->eax = process_execute (comand);   
        lock_release(&filesys_lock);
      }
      break;

    default:
      /* Syscall nao implementada neste baseline:
         encerramos o processo para manter comportamento seguro. */
      syscall_exit (-1);
      break;
    }
}

static void
syscall_exit (int status)
{
  /* Guardamos o codigo para a mensagem final em process_exit(). */
  thread_current ()->exit_status = status;
  thread_exit ();
}

static void
validate_user_address (const void *uaddr)
{
  struct thread *cur = thread_current ();

  /* Regras basicas de seguranca:
     - nao pode ser NULL
     - precisa estar no espaco de usuario (abaixo de PHYS_BASE)
     - precisa estar mapeado na pagedir atual */
  if (
      uaddr == NULL || !is_user_vaddr (uaddr) ||
      uaddr < (void *) 0x08048000 ||  // Verifica se está no espaço virtual destinado corretamente
      pagedir_get_page (cur->pagedir, uaddr) == NULL
    )
    syscall_exit (-1);
}

static void
validate_user_buffer (const void *buffer, size_t size)
{
  const uint8_t *uaddr = buffer;
  size_t i;

  /* Validacao byte a byte: simples de entender e cobre buffers
     que cruzam limite de pagina. */
  for (i = 0; i < size; i++)
    validate_user_address (uaddr + i);
}

static uint32_t
fetch_u32 (const void *uaddr)
{
  /* Primeiro valida, depois le os 4 bytes. */
  validate_user_buffer (uaddr, sizeof (uint32_t));
  return *(const uint32_t *) uaddr;
}

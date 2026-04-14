#include "userprog/syscall.h"
#include <stdio.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/vaddr.h"    /* Resolve o is_user_vaddr */
#include "userprog/pagedir.h" /* Resolve o pagedir_get_page */
#include "devices/shutdown.h" /* Resolve o shutdown_power_off */

static void syscall_handler (struct intr_frame *);
static void check_address (const void *vaddr);

void
syscall_init (void) 
{
  intr_register_int (0x30, 3, INTR_ON, syscall_handler, "syscall");
}

static void
check_address (const void *vaddr) 
{
  if (
      // Returns true if VADDR is a user virtual address.
      !is_user_vaddr (vaddr) ||  
      // Verifica se está no espaço virtual destinado corretamente e
      // se não é nulo, que seria 0.
      vaddr < (void *) 0x08048000 || 
      // Verifica se o endereço virtual foi reservado (paginação).
      pagedir_get_page (thread_current ()->pagedir, vaddr) == NULL
    ) 
  {
    printf("%s: exit(-1)\n", thread_current ()->name);
    thread_exit (); // Fecha a thread.
  }
}

static void
syscall_handler (struct intr_frame *f) 
{
  check_address (f->esp); // Valida o endereço da pilha.

  // Guarda o primeiro elemento, que identifica qual a syscall.
  int num_syscall = *(int *) f->esp; 

  switch (num_syscall) // Chama a função solicitada.
  {
    case SYS_HALT:
      shutdown_power_off (); // Deve desligar o pint os.
      break;

    case SYS_EXIT:
      // Valida o argumento do status, esse casos servem pra validar se,
      // primeiro, não deu overflow e, segundo, se o espaço físico foi reservado
      // pela máquina. Ou seja, tem que validar tbm.
      check_address (f->esp + 4); 
      thread_exit (); // Implementação básica, só para não travar.
      break;

    case SYS_WRITE:
      
      check_address (f->esp + 4); // fd
      check_address (f->esp + 8); // buffer ptr
      check_address (f->esp + 12); // size
      
      // Valida o conteúdo do buffer que o usuário quer escrever,
      // tem que ver se o buffer tá dentro da área permitida.
      void *buffer = *(void **)(f->esp + 8);
      unsigned size = *(unsigned *)(f->esp + 12);
      check_address (buffer);
      check_address (buffer + size - 1); //Vê se o final está dentro
      
      // File descriptor, descritor de arquivo, indica
      // onde deve escrever.
      int fd = *(int *)(f->esp + 4);

      // No caso 1 pra imprimir no console mesmo
      if (fd == 1) 
        {
          putbuf (buffer, size); //Função do Kernel para imprimir direto na saida padrão
          f->eax = size; 
          // Esse eax é o registrados onde ficam os retornos, nesse caso a gente retorna
          // o tamanho da string escrita.
        }
      else 
        {
          // Agora só estamos focando em imprimir no buffer então
          // retorna -1, para indicar como erro.
          f->eax = -1; 
        }
        
      // Still in process!
      break;

    default:
      // Caso o código não remeta a uma syscall demonstra um erro.
      printf ("%s: exit(-1)\n", thread_current ()->name);
      thread_exit ();
  }
}
#ifndef USERPROG_SYSCALL_H
#define USERPROG_SYSCALL_H

// Lock unico para serializar acesso ao filesys, que não é thread safe.
extern struct lock filesys_lock;
/* Movido para cá para evitar erros de leitura gerais,
   principalmente ao puxar páginas para a ram. */

void syscall_init (void);

#endif /* userprog/syscall.h */

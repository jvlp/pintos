#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include <stdint.h>
#include "threads/palloc.h"

struct frame_table_entry {
    void *kpage;               // Endereço físico real (o frame na RAM, alocado com palloc)
    void *upage;               // Endereço virtual (o endereço de memória falso que o programa acha que está usando)
    struct thread *owner;      // Ponteiro para o processo dono deste frame
    bool pinned;               // Flag crítica: se 'true', impede que o frame seja despejado durante I/O
    struct list_elem elem;     // Para encadear na lista ou fila de controle
};

void frame_init (void);
void *frame_allocate (enum palloc_flags flags, void *upage);
void frame_free (void *kpage);
void frame_unpin (void *kpage);

#endif /* vm/frame.h */
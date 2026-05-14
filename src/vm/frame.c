#include "vm/frame.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/thread.h"

/* Garantir que não haja competição para mecher na tabela. */
static struct lock frame_lock;

static struct list frame_table;

struct list_elem *frame_find(void *kpage);

struct frame_table_entry {
    void *kpage;               // Endereço físico real (o frame na RAM, alocado com palloc)
    struct thread *owner;      // Ponteiro para o processo dono deste frame
    bool pinned;               // Flag crítica: se 'true', impede que o frame seja despejado durante I/O
    struct list_elem elem;     // Para encadear na lista ou fila de controle
};

void frame_init (void)
{
    lock_init (&frame_lock);
    list_init (&frame_table);
}

void *frame_allocate (enum palloc_flags flags)
{
    void* kpage =  palloc_get_page (flags); // Aloca o frame

    if (kpage == NULL)
    {
        /*  Aqui ele deveria tratar falta de página,
            remove alguma página livre da tabela jogando
            na swap e pra ter espaço
            
        */
        return NULL; 
    }

    // Aloca o elemento que vai entrar na tabela
    struct frame_table_entry *fte = malloc (sizeof (struct frame_table_entry));
    
    if (fte == NULL)
    {
        // Se até a memória do Kernel acabou.
        palloc_free_page (kpage);
        return NULL;
    }

    fte->kpage = kpage;
    fte->owner = thread_current (); // O processo atual é o dono
    fte->pinned = true;
    /*  Esse pinned serve para manter o frame livre de possíveis
        substituições, pelo menos enquanto o kernel está trabalhando 
        nele diretamente. */

    lock_acquire(&frame_lock);
    list_push_back (&frame_table, &fte->elem);
    lock_release(&frame_lock);

    return kpage;
}

void frame_free (void *kpage){

    lock_acquire (&frame_lock);

    //  A procura por enquanto é sequencial
    struct list_elem *elm = frame_find(kpage);

    if(elm != NULL)
    {
        struct frame_table_entry *fte = list_entry (elm, struct frame_table_entry, elem);
        palloc_free_page (kpage);
        list_remove(elm);
        free(fte);
    }

    lock_release (&frame_lock);

}

struct list_elem *frame_find(void *kpage)
{
    // Busca sequencial
    for (struct list_elem *e = list_begin (&frame_table); e != list_end (&frame_table); e = list_next (e))
    {
        struct frame_table_entry *fte = list_entry (e, struct frame_table_entry, elem);
        if (fte->kpage == kpage)
        {
            return e;
        }
    }

    return NULL;
}

void frame_unpin (void *kpage) 
{
    lock_acquire (&frame_lock);
    struct list_elem *elm = frame_find(kpage);
    if(elm != NULL) {
        struct frame_table_entry *fte = list_entry (elm, struct frame_table_entry, elem);
        fte->pinned = false; // Aqui sim o pinned=false faz sentido!
    }
    lock_release (&frame_lock);
}
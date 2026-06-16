#include "vm/frame.h"
#include "threads/synch.h"
#include "threads/malloc.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "vm/swap.h"
#include "vm/page.h"

/* Garantir que não haja competição para mecher na tabela. */
static struct lock frame_lock;

static struct list frame_table;

// Algoritmo de susbtitução de clock,
// aponta para a próxima a ser retirada
static struct list_elem *clock_ptr; 

struct list_elem *frame_find(void *kpage);

void frame_init (void)
{
    lock_init (&frame_lock);
    list_init (&frame_table);
}

// Funcção para tratar faltas
static void *frame_evict (enum palloc_flags flags) 
{
    lock_acquire(&frame_lock);

    // Caso o "ponteiro" aponte para a última (que não vale) ou 
    // para nada
    if (clock_ptr == NULL || clock_ptr == list_end(&frame_table))
        clock_ptr = list_begin(&frame_table);

    // Flag pra verificar se todas estão pinadas,
    // o pin serve para não retirar páginas em andamento.
    int flag = 0;

    struct frame_table_entry *victim = NULL;

    // Roda o relógio procurando uma vítima
    while(true) 
    {
        if (clock_ptr == list_end(&frame_table)) {
            clock_ptr = list_begin(&frame_table);
            flag++;
            if(flag > 2) break; // Evita loop infinito se todas estiverem pinned
        }

        struct frame_table_entry *fte = list_entry(clock_ptr, struct frame_table_entry, elem);

        if (!fte->pinned) {
            // Verifica o Bit de Acesso no hardware, também dá uma 2ª chance
            if (pagedir_is_accessed(fte->owner->pagedir, fte->upage)) {
                // Segunda chance
                // Função do pintos já
                pagedir_set_accessed(fte->owner->pagedir, fte->upage, false); 
            } else {
                victim = fte; // Achou a vítima
                break;
            }
        }
        clock_ptr = list_next(clock_ptr);
    }

    if (victim == NULL) {
        lock_release(&frame_lock);
        return NULL;
    }

    // Procura o elemento na tebela de página do processo
    struct spt_entry *spte = spt_find(&victim->owner->spt, victim->upage);

    // Manda pra swap
    size_t swap_idx = swap_out(victim->kpage);
    
    // Atualiza as infos
    spte->is_loaded = false;
    spte->type = PAGE_SWAP;
    spte->swap_index = swap_idx;

    // Tira da tabela de páginas
    pagedir_clear_page(victim->owner->pagedir, victim->upage);

    void *kpage = victim->kpage;
    clock_ptr = list_remove(&victim->elem); // Devolve o próximo
    free(victim);
    lock_release(&frame_lock);

    //Desaloca a antiga e retona uma nova
    palloc_free_page(kpage); 
    return palloc_get_page(flags);
}

void *frame_allocate (enum palloc_flags flags, void *upage)
{
    void* kpage =  palloc_get_page (flags); // Aloca o frame

    
    if (kpage == NULL)
    {
        // Não conseguiu alocar, joga uma na swap e tenta alocar de novo
        kpage = frame_evict(flags);
        if (kpage == NULL) return NULL; // Caso dê errado
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
    fte->upage = upage;
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
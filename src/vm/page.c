#include "page.h"
#include "threads/thread.h"
#include "userprog/pagedir.h"
#include "frame.h"
#include "threads/malloc.h"
#include "threads/vaddr.h"
#include <string.h>
#include "userprog/syscall.h"
#include "threads/synch.h"

// Função hash, chaveamento
static unsigned int 
spt_hash_func (const struct hash_elem *e, void *aux UNUSED) 
{
    // Pega o elemento e localiza o spt
    const struct spt_entry *entry = hash_entry (e, struct spt_entry, elem);
    // Função pronta do pintos, pega o endereço e o tamanho e tranforma num número
    return  hash_bytes (&entry->upage, sizeof entry->upage);
}

// Função que compara as entradas para caso de colisão
static bool
spt_less_func (const struct hash_elem *a, const struct hash_elem *b, void *aux UNUSED)
{
    const struct spt_entry *entry_a = hash_entry (a, struct spt_entry, elem);
    const struct spt_entry *entry_b = hash_entry (b, struct spt_entry, elem);
    return entry_a->upage < entry_b->upage;
}

// Inicia a hash
void spt_init (struct hash *spt)
{
    hash_init (spt, spt_hash_func, spt_less_func, NULL);
}

struct spt_entry *spt_find (struct hash *spt, void *upage)
{
    struct spt_entry entry;
    struct hash_elem *e;

    // Arredonda o endereço para garantir que a chave bata com a base da página 
    entry.upage = pg_round_down (upage);

    // Procuara na hash, função do pintos
    e = hash_find (spt, &entry.elem);

    if (e != NULL) {
        // Retorna o elemento
        return hash_entry (e, struct spt_entry, elem);
    }
    
    // Caso dê erro, não ache
    return NULL;
}

bool spt_insert (struct hash *spt, struct spt_entry *entry)
{
    // A função hash_insert retorna NULL se inseriu com sucesso, ou o elemento se ele já estava na tabela
    struct hash_elem *e = hash_insert (spt, &entry->elem);
    
    // NULL == Sucesso
    return (e == NULL);
}

// Destrói um nó individual da spt
static void 
spt_destroy_func (struct hash_elem *e, void *aux UNUSED)
{
    struct spt_entry *entry = hash_entry (e, struct spt_entry, elem);

    // Se a página estiver na RAM, é necessáro devolver o frame físico (kpage) */
    if (entry->is_loaded)
    {
        // Consultamos o hardware para descobrir em qual frame físico essa página está
        void *kpage = pagedir_get_page (thread_current ()->pagedir, entry->upage);
        
        if (kpage != NULL)
        {
            /* Chama a função que você criou em frame.c para destruir e liberar a RAM */
            frame_free (kpage);
            /* Limpa o mapeamento no hardware para evitar o double free depois */
            pagedir_clear_page (thread_current ()->pagedir, entry->upage);
        }
    }

    if (entry->type == PAGE_FILE && entry->file != NULL) {
        file_close (entry->file);
    }

    // Libera a memória alocada pro registro 
    free (entry);
}

// Função para destruir a spt toda, age sobre cada elemento
void 
spt_destroy (struct hash *spt)
{
    hash_destroy (spt, spt_destroy_func);
}

bool 
spt_load_page (struct spt_entry *entry) 
{
    //Pede um frame físico para a Frame Table.
    void *kpage = frame_allocate (PAL_USER, entry->upage);
    if (kpage == NULL) {
        return false;
    }

    //Lê os dados do arquivo 
    if (entry->read_bytes > 0) {
        // Lê do disco no offset
        lock_acquire (&filesys_lock); /* Pega o cadeado */
        int bytes_read = file_read_at (entry->file, kpage, entry->read_bytes, entry->offset);
        lock_release (&filesys_lock); /* Solta o cadeado */

        if (bytes_read != (int) entry->read_bytes) {
            frame_free (kpage);
            return false;
        }
    }

    // Preenche o restante do frame com zeros
    if (entry->zero_bytes > 0) {
        memset ((uint8_t *) kpage + entry->read_bytes, 0, entry->zero_bytes);
    }

    // Conecta o endereço virtual ao físico
    if (!pagedir_set_page (thread_current ()->pagedir, entry->upage, kpage, entry->writable)) {
        frame_free (kpage);
        return false; // Caso dê erro no processo
    }

    entry->is_loaded = true; // Marca como carregada
    frame_unpin (kpage); // Solta o pin

    return true;
}

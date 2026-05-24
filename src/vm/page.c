#include "page.h"
#include "threads.h"
#include "userprog/pagedir.h"
#include "frame.h"
#include "malloc.h"

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
        }
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
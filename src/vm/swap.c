
#include "vm/swap.h"
#include "threads/malloc.h"
#include <stdbool.h>
#include <stdint.h>

static struct block *swap_block; //ponteiro para o disco físico do swap//
static struct list swap_list; //lista para controle dos espaços do swap//
static struct lock swap_lock; 
static bool swap_initialization = false;

void swap_init (void)
{

	if (swap_initialization) { //garantia que o swap seja inicializado apenas uma vez//
		return;
	}
	swap_initialization = true;

	swap_block = block_get_role (BLOCK_SWAP);
	if (!swap_block)
	{
		return;
	}

	list_init(&swap_list);
	lock_init(&swap_lock); //inicializa a lock que evita colisões//

	size_t i = 0;
    size_t total_slots = block_size(swap_block) / SECTORS_PER_PAGE; //calcula quantas páginas cabem no disco, 4KB/ 512B = 8//

	for(i = 0; i < total_slots; i++)
	{
		struct swap_item * item  = malloc(sizeof(struct swap_item)); //aloca um item para cada slot da swap//

        if(item == NULL){
            return;
        }
		item->available = true; //começa com os espaços da swap disponiveis //
		item->index = i; //identificação do slot por meio do índice na lista //
		if(item == NULL) return;
		list_push_back(&swap_list, &item->elem); //inserção do item no fim da lista//
	}
	return;
}


size_t swap_out (void * upage) //Colocando página que está ocupando espaço na memória RAM no disco//
{
	swap_init ();
	
	if (swap_block == NULL)
	{
		return (size_t)-1;
	}

	lock_acquire(&swap_lock); 
	struct list_elem * e;
	
	size_t index = (size_t)-1;
	bool found = false;

	for(e = list_begin(&swap_list); e != list_end(&swap_list); e = list_next(e)) //pecorre a lista procurando um slot disponível//
	{
		struct swap_item * item = list_entry(e, struct swap_item, elem);
		if(item->available == true)
		{
			item->available = false; //marca o espaço como ocupado//
			index = item->index;
            found = true;
			break;
		}
	}

	if(!found) 
	{
		lock_release(&swap_lock); //se o disco estiver cheio, desoculpa o espaço e retorna erro//
		return (size_t)-1;
	}

    int i;
	for (i = 0; i < SECTORS_PER_PAGE; i++) //escreve a página no disco, dividindo nos 8 setores//
	{
		block_write (swap_block, index * SECTORS_PER_PAGE + i, (uint8_t *) upage + i * BLOCK_SECTOR_SIZE);
	}
	lock_release(&swap_lock);
	return index; //retorna o bloco utilizado para a Supplemental Page Table marcar//
}


struct swap_item * get_swap_item_at_index(size_t index) //encontro do item por meio do seu indice, função auxiliar das demais//
{

	if(list_empty(&swap_list)) return NULL;
    
    struct list_elem * e;
    for(e = list_begin(&swap_list); e != list_end(&swap_list); e = list_next(e))
    {
        struct swap_item * item = list_entry(e, struct swap_item, elem);
        if(item->index == index) {
            return item;
        }
    }
    return NULL;
}

// swap a page from swap slot into memory
void swap_in (size_t used_index, void * upage) //dados do disco para a RAM, ou seja, acesso da memória enviada ao swap//
{
	int i;
	lock_acquire(&swap_lock);
	struct swap_item * item = get_swap_item_at_index(used_index); //localização da estrutura//
	
	if (item == NULL || item->available == true)
	{
		lock_release(&swap_lock);
		return;
	}


	for (i = 0; i < SECTORS_PER_PAGE; i++) //leitura do disco para a RAM, que é recontruida por meio dos 8 setores da swap table//
	{
		block_read (swap_block, used_index * SECTORS_PER_PAGE + i, (uint8_t *) upage + i * BLOCK_SECTOR_SIZE);
	}
    item->available = true; // marca o espaço do swap como disponivel novamente

	lock_release(&swap_lock);
}


void swap_free (size_t used_index) //busca pela lista, para marcar o espaço como livre//
{
    lock_acquire(&swap_lock);
    struct swap_item * item = get_swap_item_at_index(used_index);
    if (item != NULL)
    {
        item->available = true;
    }
    lock_release(&swap_lock);
}

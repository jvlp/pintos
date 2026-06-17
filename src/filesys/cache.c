#include "filesys/cache.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include <string.h>

/* Estrutura de uma entrada individual no Cache */
struct cache_entry {
    bool valid;                 // Se tiver dados válidos do disco
    bool dirty;                 // Se foi modificado e precisa de ir para o disco 
    bool accessed;              // Usado para o algoritmo de relógio
    
    block_sector_t sector;      // Qual é o setor do disco que está aqui guardado? 
    uint8_t data[BLOCK_SECTOR_SIZE]; // Os bytes reais de dados 
    
    struct lock entry_lock;     // Bloqueio para que duas threads não acedam ao mesmo bloco em simultâneo
};

static struct cache_entry cache[CACHE_SIZE];

// Um bloqueio global para mexer na tabela
static struct lock cache_lock;

// Ponteiro para o Algoritmo do Relógio 
static int clock_hand;

void 
cache_init (void) 
{
    lock_init (&cache_lock);
    clock_hand = 0;
    
    int i;
    for (i = 0; i < CACHE_SIZE; i++) {
        cache[i].valid = false;
        cache[i].dirty = false;
        cache[i].accessed = false;
        lock_init (&cache[i].entry_lock);
    }
}

// Devolve uma linha do cache com os dados do setor pedido 
static struct cache_entry*
cache_get_block (struct block *fs_device, block_sector_t sector)
{
  lock_acquire (&cache_lock);

  //Já está na RAM? 
  int i;
  for (i = 0; i < CACHE_SIZE; i++) {
      if (cache[i].valid && cache[i].sector == sector) {
          lock_acquire (&cache[i].entry_lock);
          cache[i].accessed = true;
          lock_release (&cache_lock);
          return &cache[i];
      }
  }

  // Puxar da memória (disco), caso não esteja na cache
  struct cache_entry *victim = NULL; 
  
  while (victim == NULL) {
      struct cache_entry *ce = &cache[clock_hand]; //Segue a fila
      
      if (lock_try_acquire (&ce->entry_lock)) {
          if (!ce->valid) {
              victim = ce; // Espaço vazio perfeito!
          } else if (ce->accessed) {
              ce->accessed = false; // Segunda chance
              lock_release (&ce->entry_lock);
          } else {
              victim = ce;
          }
      }
      // O próximo bloco será o primeiro a ser escolhido para sair
      clock_hand = (clock_hand + 1) % CACHE_SIZE;
  }

  lock_release (&cache_lock);

  // Se tiver sido modificada tem que salvar no disco 
  if (victim->valid && victim->dirty) {
      block_write (fs_device, victim->sector, victim->data);
  }

  victim->valid = true;
  victim->sector = sector;
  victim->dirty = false;
  victim->accessed = true;
  
  block_read (fs_device, sector, victim->data);
  
  return victim;
}

// Puxa do cache para o buffer
void 
cache_read (struct block *fs_device, block_sector_t sector, void *buffer) 
{
    struct cache_entry *ce = cache_get_block (fs_device, sector);
    
    memcpy (buffer, ce->data, BLOCK_SECTOR_SIZE);
    lock_release (&ce->entry_lock);
}

// Do buffer para o cache
void 
cache_write (struct block *fs_device, block_sector_t sector, const void *buffer) 
{
    struct cache_entry *ce = cache_get_block (fs_device, sector);
    
    memcpy (ce->data, buffer, BLOCK_SECTOR_SIZE);
    ce->dirty = true; //Foi escrito
    lock_release (&ce->entry_lock);
}

// Força a gravação de todos os blocos sujos no disco
void 
cache_flush (struct block *fs_device) 
{
    lock_acquire (&cache_lock);

    int i;
    for (i = 0; i < CACHE_SIZE; i++) {
        if (cache[i].valid) {
            lock_acquire (&cache[i].entry_lock);
            if (cache[i].dirty) {
                block_write (fs_device, cache[i].sector, cache[i].data);
                cache[i].dirty = false;
            }
            lock_release (&cache[i].entry_lock);
        }
    }
    
    lock_release (&cache_lock);
}
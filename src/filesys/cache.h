#ifndef FILESYS_CACHE_H
#define FILESYS_CACHE_H

#include "devices/block.h"
#include <stdbool.h>

// Limite de 64 blocos para a cache
#define CACHE_SIZE 64

void cache_init (void);
void cache_read (struct block *fs_device, block_sector_t sector, void *buffer);
void cache_write (struct block *fs_device, block_sector_t sector, const void *buffer);
void cache_flush (struct block *fs_device);

#endif /* filesys/cache.h */
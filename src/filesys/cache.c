#include "filesys/cache.h"
#include <stdbool.h>
#include <string.h>
#include "devices/timer.h"
#include "filesys/filesys.h"
#include "threads/synch.h"
#include "threads/thread.h"

#define CACHE_SIZE 64
#define READ_AHEAD_QUEUE_SIZE 64
#define WRITE_BEHIND_INTERVAL (TIMER_FREQ * 5)

struct cache_entry
  {
    bool valid;
    bool dirty;
    bool accessed;
    block_sector_t sector;
    uint8_t data[BLOCK_SECTOR_SIZE];
  };

static struct cache_entry cache[CACHE_SIZE];
static struct lock cache_lock;
static size_t clock_hand;

static block_sector_t read_ahead_queue[READ_AHEAD_QUEUE_SIZE];
static size_t read_ahead_head;
static size_t read_ahead_tail;
static size_t read_ahead_count;
static struct lock read_ahead_lock;
static struct semaphore read_ahead_sema;

static bool cache_shutdown;

static struct cache_entry *cache_lookup (block_sector_t sector);
static struct cache_entry *cache_get_entry (block_sector_t sector);
static void cache_flush_entry (struct cache_entry *entry);
static void cache_prefetch (block_sector_t sector);
static void cache_schedule_read_ahead (block_sector_t sector);
static void read_ahead_worker (void *aux UNUSED);
static void write_behind_worker (void *aux UNUSED);

void
cache_init (void)
{
  size_t i;

  lock_init (&cache_lock);
  lock_init (&read_ahead_lock);
  sema_init (&read_ahead_sema, 0);
  clock_hand = 0;
  read_ahead_head = 0;
  read_ahead_tail = 0;
  read_ahead_count = 0;
  cache_shutdown = false;
  for (i = 0; i < CACHE_SIZE; i++)
    {
      cache[i].valid = false;
      cache[i].dirty = false;
      cache[i].accessed = false;
      cache[i].sector = 0;
    }

  thread_create ("fs-read-ahead", PRI_DEFAULT, read_ahead_worker, NULL);
  thread_create ("fs-write-behind", PRI_DEFAULT, write_behind_worker, NULL);
}

void
cache_read (block_sector_t sector, void *buffer)
{
  struct cache_entry *entry;

  lock_acquire (&cache_lock);
  entry = cache_get_entry (sector);
  memcpy (buffer, entry->data, BLOCK_SECTOR_SIZE);
  lock_release (&cache_lock);

  if (sector + 1 < block_size (fs_device))
    cache_schedule_read_ahead (sector + 1);
}

void
cache_write (block_sector_t sector, const void *buffer)
{
  struct cache_entry *entry;

  lock_acquire (&cache_lock);
  entry = cache_get_entry (sector);
  memcpy (entry->data, buffer, BLOCK_SECTOR_SIZE);
  entry->dirty = true;
  entry->accessed = true;
  lock_release (&cache_lock);
}

void
cache_flush (void)
{
  size_t i;

  lock_acquire (&cache_lock);
  for (i = 0; i < CACHE_SIZE; i++)
    cache_flush_entry (&cache[i]);
  lock_release (&cache_lock);
}

void
cache_shutdown_threads (void)
{
  lock_acquire (&read_ahead_lock);
  cache_shutdown = true;
  lock_release (&read_ahead_lock);
  sema_up (&read_ahead_sema);
}

void
cache_invalidate (block_sector_t sector)
{
  struct cache_entry *entry;

  lock_acquire (&cache_lock);
  entry = cache_lookup (sector);
  if (entry != NULL)
    {
      entry->valid = false;
      entry->dirty = false;
      entry->accessed = false;
    }
  lock_release (&cache_lock);
}

static void
cache_prefetch (block_sector_t sector)
{
  lock_acquire (&cache_lock);
  cache_get_entry (sector);
  lock_release (&cache_lock);
}

static void
cache_schedule_read_ahead (block_sector_t sector)
{
  if (sector >= block_size (fs_device))
    return;

  lock_acquire (&read_ahead_lock);
  if (!cache_shutdown && read_ahead_count < READ_AHEAD_QUEUE_SIZE)
    {
      read_ahead_queue[read_ahead_tail] = sector;
      read_ahead_tail = (read_ahead_tail + 1) % READ_AHEAD_QUEUE_SIZE;
      read_ahead_count++;
      sema_up (&read_ahead_sema);
    }
  lock_release (&read_ahead_lock);
}

static void
read_ahead_worker (void *aux UNUSED)
{
  for (;;)
    {
      block_sector_t sector;
      bool have_sector = false;

      sema_down (&read_ahead_sema);

      lock_acquire (&read_ahead_lock);
      if (cache_shutdown && read_ahead_count == 0)
        {
          lock_release (&read_ahead_lock);
          break;
        }
      if (read_ahead_count > 0)
        {
          sector = read_ahead_queue[read_ahead_head];
          read_ahead_head = (read_ahead_head + 1) % READ_AHEAD_QUEUE_SIZE;
          read_ahead_count--;
          have_sector = true;
        }
      lock_release (&read_ahead_lock);

      if (have_sector)
        cache_prefetch (sector);
    }
}

static void
write_behind_worker (void *aux UNUSED)
{
  while (!cache_shutdown)
    {
      timer_sleep (WRITE_BEHIND_INTERVAL);
      cache_flush ();
    }
}

static struct cache_entry *
cache_lookup (block_sector_t sector)
{
  size_t i;

  for (i = 0; i < CACHE_SIZE; i++)
    if (cache[i].valid && cache[i].sector == sector)
      {
        cache[i].accessed = true;
        return &cache[i];
      }
  return NULL;
}

static struct cache_entry *
cache_get_entry (block_sector_t sector)
{
  struct cache_entry *entry;
  size_t scans;

  entry = cache_lookup (sector);
  if (entry != NULL)
    return entry;

  for (scans = 0; scans < CACHE_SIZE; scans++)
    {
      entry = &cache[scans];
      if (!entry->valid)
        {
          block_read (fs_device, sector, entry->data);
          entry->valid = true;
          entry->dirty = false;
          entry->accessed = true;
          entry->sector = sector;
          return entry;
        }
    }

  for (;;)
    {
      entry = &cache[clock_hand];
      clock_hand = (clock_hand + 1) % CACHE_SIZE;

      if (entry->accessed)
        {
          entry->accessed = false;
          continue;
        }

      cache_flush_entry (entry);
      block_read (fs_device, sector, entry->data);
      entry->valid = true;
      entry->dirty = false;
      entry->accessed = true;
      entry->sector = sector;
      return entry;
    }
}

static void
cache_flush_entry (struct cache_entry *entry)
{
  if (entry->valid && entry->dirty)
    {
      block_write (fs_device, entry->sector, entry->data);
      entry->dirty = false;
    }
}

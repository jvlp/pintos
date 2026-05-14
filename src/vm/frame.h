#ifndef VM_FRAME_H
#define VM_FRAME_H

#include <list.h>
#include <stdint.h>
#include "threads/palloc.h"

void frame_init (void);
void *frame_allocate (enum palloc_flags flags);
void frame_free (void *kpage);
void frame_unpin (void *kpage);

#endif /* vm/frame.h */
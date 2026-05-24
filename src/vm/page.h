#ifndef VM_PAGE_H
#define VM_PAGE_H

#include "lib/kernel/hash.h"
#include "filesys/file.h"

// Os três estados possíveis de uma página virtual
enum page_type {
    PAGE_FILE,      // A página se refere à um arquivo
    PAGE_SWAP,      // A página está na swap
    PAGE_ZERO       // A página nova
};

struct spt_entry {
    void *upage;                 // Endereço virtual base da página
    enum page_type type;     
    bool writable;             
    bool is_loaded;              // Se a página já estiver na ram

    /* Dados para carregar do arquivo (PAGE_FILE) */
    struct file *file;
    off_t offset;
    uint32_t read_bytes;
    uint32_t zero_bytes;

    struct hash_elem elem;
};

void spt_init (struct hash *spt);
struct spt_entry *spt_find (struct hash *spt, void *upage);
bool spt_insert (struct hash *spt, struct spt_entry *entry);
void spt_destroy (struct hash *spt);
bool spt_load_page (struct spt_entry *entry);

#endif /* vm/page.h */
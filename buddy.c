#include "buddy.h"
#define NULL ((void *)0)

#define PAGE_SIZE 4096
#define MAX_RANK 16
#define MAX_PAGES (128 * 1024 * 1024 / PAGE_SIZE)  /* 128MB max */

static void *mem_start = NULL;
static int total_pages = 0;

/* Metadata arrays */
static int block_order[MAX_PAGES];      /* order (rank) of the block starting at this page */
static char is_block_head[MAX_PAGES];   /* 1 if this page is the head of a block (not internal) */
static char is_allocated[MAX_PAGES];    /* 1 if this specific page is part of an allocated block */

/* Free lists: doubly linked list for each order */
static int free_head[MAX_RANK + 2];     /* free_head[order] = first free block of that order */
static int free_next[MAX_PAGES];
static int free_prev[MAX_PAGES];
static int free_count_arr[MAX_RANK + 2];

static inline int addr_to_idx(void *p) {
    return ((char *)p - (char *)mem_start) / PAGE_SIZE;
}

static inline void *idx_to_addr(int idx) {
    return (void *)((char *)mem_start + idx * PAGE_SIZE);
}

static inline int get_buddy_idx(int idx, int order) {
    return idx ^ (1 << (order - 1));
}

/* Check if address is valid and page-aligned */
static int is_valid_addr(void *p) {
    if (p == NULL) return 0;
    char *addr = (char *)p;
    char *end = (char *)mem_start + total_pages * PAGE_SIZE;
    if (addr < (char *)mem_start || addr >= end) return 0;
    if ((addr - (char *)mem_start) % PAGE_SIZE != 0) return 0;
    return 1;
}

/* Remove block from free list */
static void remove_from_freelist(int order, int idx) {
    if (idx < 0) return;
    int prev = free_prev[idx];
    int next_s = free_next[idx];
    
    if (prev >= 0) {
        free_next[prev] = next_s;
    } else {
        free_head[order] = next_s;
    }
    
    if (next_s >= 0) {
        free_prev[next_s] = prev;
    }
    
    free_prev[idx] = -1;
    free_next[idx] = -1;
    free_count_arr[order]--;
}

/* Add block to free list (at head) */
static void add_to_freelist(int order, int idx) {
    free_next[idx] = free_head[order];
    free_prev[idx] = -1;
    
    if (free_head[order] >= 0) {
        free_prev[free_head[order]] = idx;
    }
    
    free_head[order] = idx;
    free_count_arr[order]++;
    block_order[idx] = order;
}

int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0) {
        return -EINVAL;
    }
    
    mem_start = p;
    total_pages = pgcount;
    
    /* Initialize arrays */
    int i, ord;
    for (ord = 0; ord <= MAX_RANK + 1; ord++) {
        free_head[ord] = -1;
        free_count_arr[ord] = 0;
    }
    
    for (i = 0; i < pgcount; i++) {
        block_order[i] = 0;
        is_block_head[i] = 0;
        is_allocated[i] = 0;
        free_next[i] = -1;
        free_prev[i] = -1;
    }
    
    /* Build initial free lists */
    int page_idx = 0;
    while (page_idx < pgcount) {
        /* Find the largest block starting at this position */
        int order = 1;
        int size = 1;
        
        while (order < MAX_RANK) {
            int next_size = size * 2;
            if (page_idx % next_size == 0 && page_idx + next_size <= pgcount) {
                size = next_size;
                order++;
            } else {
                break;
            }
        }
        
        /* Set up this free block */
        is_block_head[page_idx] = 1;
        block_order[page_idx] = order;
        add_to_freelist(order, page_idx);
        
        /* Mark pages as belonging to this block (not strictly necessary for free blocks) */
        for (i = 1; i < size && page_idx + i < pgcount; i++) {
            is_block_head[page_idx + i] = 0;
        }
        
        page_idx += size;
    }
    
    return OK;
}

void *alloc_pages(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return ERR_PTR(-EINVAL);
    }
    
    /* Find a free block of order >= rank */
    int found_order = rank;
    while (found_order <= MAX_RANK && free_count_arr[found_order] == 0) {
        found_order++;
    }
    
    if (found_order > MAX_RANK) {
        return ERR_PTR(-ENOSPC);
    }
    
    /* Get the first free block of found_order */
    int idx = free_head[found_order];
    if (idx < 0) {
        return ERR_PTR(-ENOSPC);
    }
    
    /* Mark it as no longer free (remove from free list) */
    remove_from_freelist(found_order, idx);
    
    /* Split down to desired order */
    while (found_order > rank) {
        found_order--;
        
        /* Split block at idx into two blocks of found_order */
        int size = 1 << (found_order - 1);
        int buddy_idx = idx + size;
        
        /* Right half goes to free list */
        is_block_head[buddy_idx] = 1;
        block_order[buddy_idx] = found_order;
        add_to_freelist(found_order, buddy_idx);
        
        /* Left half (idx) continues to be split or allocated */
        block_order[idx] = found_order;
    }
    
    /* Finalize allocation */
    int size = 1 << (rank - 1);
    block_order[idx] = rank;
    is_block_head[idx] = 1;
    
    /* Mark all pages in this block as allocated */
    int i;
    for (i = 0; i < size; i++) {
        is_allocated[idx + i] = 1;
    }
    
    return idx_to_addr(idx);
}

int return_pages(void *p) {
    if (!is_valid_addr(p)) {
        return -EINVAL;
    }
    
    int idx = addr_to_idx(p);
    
    /* Check if this is a valid allocated block head */
    if (!is_block_head[idx] || !is_allocated[idx]) {
        return -EINVAL;
    }
    
    int order = block_order[idx];
    int size = 1 << (order - 1);
    
    /* Mark as free */
    int i;
    for (i = 0; i < size; i++) {
        is_allocated[idx + i] = 0;
    }
    
    /* Try to merge with buddy */
    while (order < MAX_RANK) {
        int buddy_idx = get_buddy_idx(idx, order);
        
        /* Check if buddy is valid */
        if (buddy_idx < 0 || buddy_idx >= total_pages) {
            break;
        }
        
        /* Check if buddy is a free block of the same order */
        if (!is_block_head[buddy_idx] || is_allocated[buddy_idx]) {
            break;
        }
        if (block_order[buddy_idx] != order) {
            break;
        }
        
        /* Need to check if buddy is in a free list */
        /* A free block should have is_block_head=1, is_allocated=0, and match order */
        
        /* Remove buddy from free list */
        remove_from_freelist(order, buddy_idx);
        
        /* Merge: combined block uses lower index */
        int combined_idx = (idx < buddy_idx) ? idx : buddy_idx;
        int other_idx = (idx < buddy_idx) ? buddy_idx : idx;
        
        is_block_head[other_idx] = 0;
        idx = combined_idx;
        order++;
        block_order[idx] = order;
    }
    
    /* Add the final block to free list */
    add_to_freelist(order, idx);
    
    return OK;
}

int query_ranks(void *p) {
    if (!is_valid_addr(p)) {
        return -EINVAL;
    }
    
    int idx = addr_to_idx(p);
    
    /* Find the head of the block containing this page */
    int head_idx = idx;
    while (head_idx > 0 && !is_block_head[head_idx]) {
        head_idx--;
    }
    
    /* head_idx should be a block head */
    if (!is_block_head[head_idx]) {
        return -EINVAL;
    }
    
    int order = block_order[head_idx];
    int size = 1 << (order - 1);
    
    /* Check if idx is within this block */
    if (idx >= head_idx + size) {
        return -EINVAL;
    }
    
    return order;
}

int query_page_counts(int rank) {
    if (rank < 1 || rank > MAX_RANK) {
        return -EINVAL;
    }
    
    return free_count_arr[rank];
}

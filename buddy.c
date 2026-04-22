#include "buddy.h"
#include <string.h>
#include <stdlib.h>

#define NULL ((void *)0)
#define MAXRANK 16

// Free list for each rank - each entry points to a free block
static void *free_list[MAXRANK + 1];

// Allocation status array - 0 = free, 1 = allocated
// Each page (4K) has a status entry
static char *alloc_status = NULL;

// Base address of the memory pool
static void *base_addr = NULL;

// Total number of pages
static int total_pages = 0;

// Maximum rank for this memory pool
static int max_rank = 0;

// Page size is 4K
#define PAGE_SIZE (4096)

// Calculate the buddy address for a block
static void *get_buddy(void *addr, int rank) {
    size_t offset = (size_t)addr - (size_t)base_addr;
    size_t block_size = PAGE_SIZE << (rank - 1);
    size_t buddy_offset = offset ^ block_size;
    return (void *)((size_t)base_addr + buddy_offset);
}

// Get the page index for an address
static int get_page_index(void *addr) {
    size_t offset = (size_t)addr - (size_t)base_addr;
    return offset / PAGE_SIZE;
}

// Check if an address is within the memory pool
static int is_valid_addr(void *addr) {
    if (addr == NULL) return 0;
    size_t offset = (size_t)addr - (size_t)base_addr;
    return offset < (size_t)total_pages * PAGE_SIZE;
}

// Check if an address is aligned to a page boundary
static int is_aligned(void *addr) {
    size_t offset = (size_t)addr - (size_t)base_addr;
    return (offset & (PAGE_SIZE - 1)) == 0;
}

int init_page(void *p, int pgcount) {
    if (p == NULL || pgcount <= 0) return -EINVAL;

    base_addr = p;
    total_pages = pgcount;

    // Calculate max rank: find largest power of 2 that fits
    max_rank = 1;
    while ((PAGE_SIZE << (max_rank - 1)) * 2 <= (size_t)pgcount * PAGE_SIZE && max_rank < MAXRANK) {
        max_rank++;
    }

    // Initialize free lists
    for (int i = 1; i <= MAXRANK; i++) {
        free_list[i] = NULL;
    }

    // Allocate status array
    alloc_status = (char *)malloc(pgcount);
    if (alloc_status == NULL) return -EINVAL;
    memset(alloc_status, 0, pgcount);

    // Add the largest possible block to free list
    free_list[max_rank] = base_addr;

    return OK;
}

void *alloc_pages(int rank) {
    // Validate rank
    if (rank < 1 || rank > MAXRANK) return ERR_PTR(-EINVAL);

    // Find a free block of at least the requested rank
    int found_rank = rank;
    while (found_rank <= MAXRANK && free_list[found_rank] == NULL) {
        found_rank++;
    }

    // No space available
    if (found_rank > MAXRANK) return ERR_PTR(-ENOSPC);

    // Remove block from free list
    void *block = free_list[found_rank];
    free_list[found_rank] = *(void **)block;

    // Split blocks if necessary
    // Continue splitting the lower half, add upper half to free list
    while (found_rank > rank) {
        found_rank--;
        void *buddy = (void *)((size_t)block + (PAGE_SIZE << (found_rank - 1)));
        // Add buddy (upper half) to free list, continue with block (lower half)
        *(void **)buddy = free_list[found_rank];
        free_list[found_rank] = buddy;
    }

    // Mark pages as allocated with their rank
    int pages = 1 << (rank - 1);
    int start_idx = get_page_index(block);
    for (int i = 0; i < pages; i++) {
        alloc_status[start_idx + i] = rank;
    }

    return block;
}

int return_pages(void *p) {
    // Validate address
    if (!is_valid_addr(p) || !is_aligned(p)) return -EINVAL;

    int page_idx = get_page_index(p);
    if (alloc_status[page_idx] == 0) return -EINVAL;  // Not allocated

    // Get the rank of this block (stored in alloc_status)
    int rank = alloc_status[page_idx];

    // Verify all pages in this block have the same rank
    int pages = 1 << (rank - 1);
    for (int i = 0; i < pages; i++) {
        if (page_idx + i >= total_pages || alloc_status[page_idx + i] != rank) {
            return -EINVAL;  // Not a valid block start or inconsistent ranks
        }
    }

    // Mark pages as free
    for (int i = 0; i < pages; i++) {
        alloc_status[page_idx + i] = 0;
    }

    // Try to coalesce with buddy
    void *current = p;
    int current_rank = rank;

    while (current_rank <= max_rank) {
        void *buddy = get_buddy(current, current_rank);
        int buddy_idx = get_page_index(buddy);

        // Check if buddy is free
        int buddy_free = 1;
        for (int i = 0; i < (1 << (current_rank - 1)); i++) {
            if (buddy_idx + i >= total_pages || alloc_status[buddy_idx + i] != 0) {
                buddy_free = 0;
                break;
            }
        }

        if (!buddy_free) break;

        // Remove buddy from free list
        void **prev = &free_list[current_rank];
        while (*prev != NULL && *prev != buddy) {
            prev = (void **)*prev;
        }
        if (*prev == buddy) {
            *prev = *(void **)buddy;
        }

        // Merge: use the lower address as the new block
        if ((size_t)buddy < (size_t)current) {
            current = buddy;
        }
        current_rank++;
    }

    // Add the (possibly coalesced) block to free list
    *(void **)current = free_list[current_rank];
    free_list[current_rank] = current;

    return OK;
}

int query_ranks(void *p) {
    // Validate address
    if (!is_valid_addr(p) || !is_aligned(p)) return -EINVAL;

    int page_idx = get_page_index(p);

    // If allocated, find the rank of this allocated block
    if (alloc_status[page_idx] == 1) {
        int rank = 1;
        while (rank < MAXRANK) {
            int next_page_idx = page_idx + (1 << (rank - 1));
            int is_contiguous = 1;
            for (int i = 0; i < (1 << (rank - 1)); i++) {
                if (next_page_idx + i >= total_pages || alloc_status[next_page_idx + i] != 1) {
                    is_contiguous = 0;
                    break;
                }
            }
            if (!is_contiguous) break;

            if (rank > 1) {
                int prev_page_idx = page_idx - (1 << (rank - 2));
                int prev_contiguous = 1;
                for (int i = 0; i < (1 << (rank - 2)); i++) {
                    if (prev_page_idx + i < 0 || alloc_status[prev_page_idx + i] != 1) {
                        prev_contiguous = 0;
                        break;
                    }
                }
                if (prev_contiguous) break;
            }
            rank++;
        }
        return rank;
    }

    // If not allocated, return the maximum possible rank
    int rank = 1;
    while (rank <= max_rank) {
        size_t block_size = PAGE_SIZE << (rank - 1);
        size_t offset = (size_t)p - (size_t)base_addr;

        // Check if this block is aligned to its size
        if ((offset & (block_size - 1)) != 0) break;

        // Check if all pages in this block are free
        int pages = 1 << (rank - 1);
        int all_free = 1;
        for (int i = 0; i < pages; i++) {
            if (page_idx + i >= total_pages || alloc_status[page_idx + i] != 0) {
                all_free = 0;
                break;
            }
        }
        if (!all_free) break;
        rank++;
    }
    return rank - 1;
}

int query_page_counts(int rank) {
    // Validate rank
    if (rank < 1 || rank > MAXRANK) return -EINVAL;

    int count = 0;
    void *p = free_list[rank];
    while (p != NULL) {
        count++;
        p = *(void **)p;
    }
    return count;
}

#include <stdio.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <string.h>
#include <stddef.h>
#include <assert.h>
#include <signal.h>
#include <execinfo.h>
#include <unistd.h>
#include <time.h>
#ifdef __APPLE__
#include <sys/errno.h>
#else
#include <errno.h>
#endif
#include "lab.h"
#define handle_error_and_die(msg) \
    do                            \
    {                             \
        perror(msg);              \
        raise(SIGKILL);           \
    } while (0)
    
/**
 * @brief Convert bytes to the correct K value
 *
 * @param bytes the number of bytes
 * @return size_t the K value that will fit bytes
 */
size_t btok(size_t bytes) {
    // Total size includes the header
    size_t total_size = bytes + sizeof(struct avail);

    if (total_size == 0) return 0; // Edge case: 0 bytes requested

    size_t k = 0;
    uint64_t power = UINT64_C(1); // Start with 2^0 = 1, using C99 macro

    // Increase k until power >= total_size
    while (power < total_size) {
        power <<= 1; // Double the power (2^k)
        k++;
    }

    return k;
}

struct avail *buddy_calc(struct buddy_pool *pool, struct avail *buddy)
{
    //Get offset from base address
    uintptr_t base_addr = (uintptr_t)pool->base;
    uintptr_t buddy_addr = (uintptr_t)buddy;
    uintptr_t offset = buddy_addr - base_addr;

    size_t block_size = (UINT64_C(1) << buddy->kval);
    uintptr_t buddy_offset = offset ^ block_size;
    //get address
    struct avail *buddy_block = (struct avail *)(base_addr + buddy_offset);
     
    return buddy_block;
}

//Finally able to finish this part
void *buddy_malloc(struct buddy_pool *pool, size_t size)
{
    // Check for invalid inputs: zero size or null pool.
    if (size == 0 || pool == NULL) {
        errno = ENOMEM; // Signal memory allocation failure.
        return NULL;    // Exit early since we can’t proceed.
    }

    // Determine the smallest power-of-2 size (kval) that fits the requested bytes plus the block’s header.
    size_t kval = btok(size + sizeof(struct avail));
    // Example: If size = 32 and header = 32 bytes, kval might be 6 for a 64-byte block.

    // Ensure the block size isn’t too small unless the pool itself is tiny.
    if (kval < SMALLEST_K && pool->kval_m >= SMALLEST_K) {
        kval = SMALLEST_K; // Bump up to the minimum allowed block size, like 16 bytes.
    }

    size_t j = kval;
    bool block_found = false;

    // Look for an available block, starting from the desired size up to the pool’s largest size.
    while (j <= pool->kval_m) {
        if (pool->avail[j].next != &pool->avail[j]) { // Check if the free list at size 2^j has blocks.
            block_found = true;                       // Found a usable block!
            break;
        }
        j++; // Try the next larger size.
    }

    // If no block was found, we’re out of memory.
    if (!block_found) {
        errno = ENOMEM; // Mark the error as no memory available.
        return NULL;    // Bail out since we can’t allocate.
    }

    // Pull the chosen block out of its free list.
    struct avail *block = pool->avail[j].next; // Grab the first free block at size 2^j.
    block->prev->next = block->next;           // Link the previous block to the next, skipping this one.
    block->next->prev = block->prev;           // Update the next block’s back-pointer.

    // If the block is too big, split it until it matches the needed size.
    while (j > kval) {
        j--; // Move down one size level (e.g., from 2^7 to 2^6).
        uintptr_t addr = (uintptr_t)block;                     // Get the block’s address as a number for math.
        uintptr_t buddy_addr = addr + (UINT64_C(1) << j);      // Calculate the address of the buddy block (halfway into the current block).
        struct avail *buddy = (struct avail *)buddy_addr;      // Treat that address as a new block header.

        buddy->tag = BLOCK_AVAIL;                              // Mark the buddy as free for future use.
        buddy->kval = j;                                       // Set its size to the new, smaller level.

        // Add the buddy to the free list for its size.
        buddy->next = pool->avail[j].next;                     // Point to the current head of the list.
        buddy->prev = &pool->avail[j];                         // Link back to the list’s anchor.
        pool->avail[j].next->prev = buddy;                     // Update the old head’s back-pointer.
        pool->avail[j].next = buddy;                           // Make the buddy the new head.

        // Shrink the current block to the same size as the buddy.
        block->kval = j;                                       // Update its size to match the new level.
    }

    // Mark the block as allocated so it’s not reused.
    block->tag = BLOCK_RESERVED;

    // Give the user a pointer to the memory just past the header.
    return (void *)(block + 1); // Skip the struct avail to point to usable space.  
}

void buddy_free(struct buddy_pool *pool, void *ptr)
{
    // Ensure the inputs are valid before proceeding.
    if (ptr == NULL || pool == NULL) {
        return; // Exit early if the pointer or pool is invalid.
    }

    // Verify that the pointer falls within the pool’s memory boundaries.
    if ((char*)ptr < (char*)pool->base || (char*)ptr >= (char*)pool->base + pool->numbytes) {
        return; // Ignore pointers that don’t belong to our managed memory region.
    }

    // Locate the block’s header by moving back from the user pointer.
    struct avail *block = (struct avail *)((char *)ptr - sizeof(struct avail)); // Point to the metadata just before the user’s memory.

    // Confirm the block is actually allocated.
    if (block->tag != BLOCK_RESERVED) {
        return; // Skip if the block isn’t marked as in-use or was already freed.
    }

    // Set the block as free for future allocations.
    block->tag = BLOCK_AVAIL; // Change its status to indicate it’s available.

    // Attempt to merge with adjacent buddies to reduce fragmentation.
    while (block->kval < pool->kval_m) { // Keep going until the block reaches the pool’s max size.
        // Get the address of the block’s buddy.
        struct avail *buddy = buddy_calc(pool, block); // Calculate the paired block’s location.

        // Stop merging if the buddy isn’t free or isn’t the same size.
        if (buddy->tag != BLOCK_AVAIL || buddy->kval != block->kval) {
            break; // Can’t combine if the buddy is in use or mismatched.
        }

        // Unlink the buddy from its free list to prepare for merging.
        buddy->next->prev = buddy->prev; // Connect the buddy’s neighbors.
        buddy->prev->next = buddy->next; // Remove it from the list.

        // Use the lower-addressed block as the merged block.
        if (buddy < block) {
            block = buddy; // Switch to the buddy if it comes first in memory.
        }

        // Increase the block’s size to reflect the merger.
        block->kval++; // Double the size (e.g., from 2^k to 2^(k+1)).
    }   

// Place the block back into the appropriate free list.
block->next = pool->avail[block->kval].next; // Link to the head of the list for its size.
block->prev = &pool->avail[block->kval];     // Point back to the list’s anchor.
pool->avail[block->kval].next->prev = block; // Update the old head’s back-pointer.
pool->avail[block->kval].next = block;       // Make this block the new head.
}

void buddy_init(struct buddy_pool *pool, size_t size)
{
    size_t kval = 0;
    if (size == 0)
        kval = DEFAULT_K;
    else
        kval = btok(size);
    if (kval < MIN_K)
        kval = MIN_K;
    if (kval > MAX_K)
        kval = MAX_K - 1;
    // make sure pool struct is cleared out
    memset(pool, 0, sizeof(struct buddy_pool));
    pool->kval_m = kval;
    pool->numbytes = (UINT64_C(1) << pool->kval_m);
    // Memory map a block of raw memory to manage
    pool->base = mmap(
        NULL,                        /*addr to map to*/
        pool->numbytes,              /*length*/
        PROT_READ | PROT_WRITE,      /*prot*/
        MAP_PRIVATE | MAP_ANONYMOUS, /*flags*/
        -1,                          /*fd -1 when using MAP_ANONYMOUS*/
        0                            /* offset 0 when using MAP_ANONYMOUS*/
    );
    if (MAP_FAILED == pool->base)
    {
        handle_error_and_die("buddy_init avail array mmap failed");
    }
    // Set all blocks to empty. We are using circular lists so the first elements just point
    // to an available block. Thus the tag, and kval feild are unused burning a small bit of
    // memory but making the code more readable. We mark these blocks as UNUSED to aid in debugging.
    for (size_t i = 0; i <= kval; i++)
    {
        pool->avail[i].next = pool->avail[i].prev = &pool->avail[i];
        pool->avail[i].kval = i;
        pool->avail[i].tag = BLOCK_UNUSED;
    }
    // Add in the first block
    pool->avail[kval].next = pool->avail[kval].prev = (struct avail *)pool->base;
    struct avail *m = pool->avail[kval].next;
    m->tag = BLOCK_AVAIL;
    m->kval = kval;
    m->next = m->prev = &pool->avail[kval];
}

void buddy_destroy(struct buddy_pool *pool)
{
    int rval = munmap(pool->base, pool->numbytes);
    if (-1 == rval)
    {
        handle_error_and_die("buddy_destroy avail array");
    }
    // Zero out the array so it can be reused it needed
    memset(pool, 0, sizeof(struct buddy_pool));
}

#define UNUSED(x) (void)x
/**
 * This function can be useful to visualize the bits in a block. This can
 * help when figuring out the buddy_calc function!
 */
static void printb(unsigned long int b)
{
    size_t bits = sizeof(b) * 8;
    unsigned long int curr = UINT64_C(1) << (bits - 1);
    for (size_t i = 0; i < bits; i++)
    {
        if (b & curr)
        {
            printf("1");
        }
        else
        {
            printf("0");
        }
        curr >>= 1L;
    }
}

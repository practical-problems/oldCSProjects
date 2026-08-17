#define _DEFAULT_SOURCE   /* ensures sbrk() is declared by <unistd.h> */
#include <string.h>
#include <stdio.h>
#include <unistd.h>

void *malloc(size_t size);
void free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

#define CHUNK_SIZE (1<<12)

extern void *bulk_alloc(size_t size);
extern void bulk_free(void *ptr, size_t size);

static inline __attribute__((unused)) int block_index(size_t x) {
    if (x <= 8) {
        return 5;
    } else {
        return 32 - __builtin_clz((unsigned int)x + 7);
    }
}

/* free_lists[i] is the head of the free list for blocks of size 1 << (i + 5) */
static void *free_lists[8] = {NULL,NULL,NULL,NULL,NULL,NULL,NULL,NULL};

/*
 * Carve a freshly-sbrk'd chunk into fixed-size blocks and thread them
 * onto free_lists[list_index]. Each block layout is:
 *   [ 8-byte header: block size (payload+header) ][ payload ... ]
 * While a block sits on the free list, we reuse the first 8 bytes of
 * its PAYLOAD to store a pointer to the next free block (a classic
 * intrusive free list). That's only safe because a free block's
 * contents are unused until it's handed back out by malloc().
 */
static void add_new_chunk(int list_index) {
    void *chunk = sbrk(CHUNK_SIZE);
    if (chunk == (void *)-1) {
        return; /* out of memory; free_lists[list_index] stays NULL */
    }

    int block_size = 1 << (list_index + 5);       /* header + payload */
    int num_blocks = CHUNK_SIZE / block_size;

    char *cursor = (char *)chunk;  /* the ONLY pointer that walks the chunk */
    for (int i = 0; i < num_blocks; i++) {
        char *block = cursor + (i * block_size);

        /* header: size of this block including its own header */
        *(int *)block = block_size;

        /* payload starts right after the header */
        void *payload = block + 8;

        /* thread this block onto the front of the free list */
        *(void **)payload = free_lists[list_index];
        free_lists[list_index] = payload;
    }
}

void *malloc(size_t size) {
    if (size == 0) {
        return NULL;
    }

    if (size > 4088) {
        /* Bulk allocations also get an 8-byte header, so free()/realloc()
         * can read the block size the same way regardless of which path
         * an allocation came from. */
        size_t total = size + 8;
        void *block = bulk_alloc(total);
        if (block == NULL) {
            return NULL;
        }
        *(int *)block = (int)total;
        return (char *)block + 8;
    }

    int list_index = block_index(size) - 5;

    if (free_lists[list_index] == NULL) {
        add_new_chunk(list_index);
        if (free_lists[list_index] == NULL) {
            return NULL; /* sbrk failed */
        }
    }

    void *payload = free_lists[list_index];
    /* pop: follow the pointer STORED IN the block to the next free block */
    free_lists[list_index] = *(void **)payload;

    return payload;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *ptr = malloc(total);
    if (ptr != NULL) {
        memset(ptr, 0, total);
    }
    return ptr;
}

void *realloc(void *ptr, size_t size) {
    if (ptr == NULL) {
        return malloc(size);
    }
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    char *header_addr = (char *)ptr - 8;
    size_t old_block_size = *(int *)header_addr;   /* header + payload, as stored */
    size_t old_payload_size = old_block_size - 8;

    /* Can we satisfy the new size in the SAME size class we're already in? */
    if (size <= 4088 && old_payload_size <= 4088) {
        int old_index = block_index(old_payload_size) - 5;
        int new_index = block_index(size) - 5;
        if (old_index == new_index) {
            return ptr; /* no move needed */
        }
    }

    void *newptr = malloc(size);
    if (newptr == NULL) {
        return NULL; /* ptr is left valid/untouched, per realloc() contract */
    }

    size_t copy_size = (size < old_payload_size) ? size : old_payload_size;
    memmove(newptr, ptr, copy_size);
    free(ptr);
    return newptr;
}

void free(void *ptr) {
    if (ptr == NULL) {
        return;
    }

    char *header_addr = (char *)ptr - 8;
    int block_size = *(int *)header_addr;

    if (block_size > 4088) {
        bulk_free(header_addr, (size_t)block_size);
        return;
    }

    int list_index = block_index(block_size - 8) - 5;

    /* push: store the OLD head inside the freed block, then it's the new head */
    *(void **)ptr = free_lists[list_index];
    free_lists[list_index] = ptr;
}

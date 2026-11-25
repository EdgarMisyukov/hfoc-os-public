#include "heap.h"

#include "pmm.h"
#include "vmm.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HEAP_PAGE_SIZE   4096U
#define HEAP_PAGE_COUNT  (HFOC_HEAP_SIZE / HEAP_PAGE_SIZE)
#define HEAP_HEADER_SIZE 48U
#define HEAP_MAGIC       UINT64_C(0x48454150424C4F43)
#define HEAP_VIRTUAL_BASE UINT64_C(0xFFFFB00000000000)
#define BLOCK_FREE       0x1ULL

typedef struct heap_block {
    uint64_t magic;
    size_t payload_size;
    uint64_t flags;
    uint64_t reserved;
    struct heap_block *prev;
    struct heap_block *next;
} heap_block;

_Static_assert(sizeof(heap_block) == HEAP_HEADER_SIZE,
               "heap block header must match the boundary-tag layout");

static uint8_t *heap_base;
static size_t heap_size;
static heap_block *head;
static size_t heap_used;
static bool heap_initialized;

#ifdef HFOC_HEAP_HOST_TEST
static unsigned char host_arena[HFOC_HEAP_SIZE]
    __attribute__((aligned(HFOC_HEAP_ALIGNMENT)));
#else
static uint64_t heap_pages[HEAP_PAGE_COUNT];
#endif

static void init_arena(uint8_t *base) {
    heap_base = base;
    heap_size = HFOC_HEAP_SIZE;
    heap_used = 0U;
    head = (heap_block *)(void *)heap_base;
    head->magic = HEAP_MAGIC;
    head->payload_size = heap_size - HEAP_HEADER_SIZE;
    head->flags = BLOCK_FREE;
    head->reserved = 0U;
    head->prev = NULL;
    head->next = NULL;
    heap_initialized = true;
}

bool heap_init(uint64_t hhdm_offset) {
    if (heap_initialized) {
        return true;
    }

#ifdef HFOC_HEAP_HOST_TEST
    (void)hhdm_offset;
    init_arena(host_arena);
    return true;
#else
    const struct address_space *const kernel_as =
        vmm_kernel_address_space();
    if (kernel_as == NULL) {
        return false;
    }

    size_t mapped = 0U;
    for (size_t index = 0U; index < HEAP_PAGE_COUNT; ++index) {
        const uint64_t physical = pmm_alloc_page();
        if (physical == 0U) {
            goto fail;
        }
        heap_pages[index] = physical;
        const enum vmm_status status = vmm_map_page_in(
            kernel_as,
            HEAP_VIRTUAL_BASE + index * HEAP_PAGE_SIZE,
            physical,
            VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE,
            hhdm_offset);
        if (status != VMM_OK) {
            (void)pmm_free_page(physical);
            goto fail;
        }
        ++mapped;
    }

    init_arena((uint8_t *)(uintptr_t)HEAP_VIRTUAL_BASE);
    return true;

fail:
    for (size_t index = 0U; index < mapped; ++index) {
        (void)vmm_unmap_page_in(
            kernel_as,
            HEAP_VIRTUAL_BASE + index * HEAP_PAGE_SIZE,
            hhdm_offset);
        (void)pmm_free_page(heap_pages[index]);
    }
    heap_initialized = false;
    heap_base = NULL;
    head = NULL;
    heap_size = 0U;
    heap_used = 0U;
    return false;
#endif
}

#ifdef HFOC_HEAP_HOST_TEST
void heap_test_reset(void) {
    heap_initialized = false;
    heap_base = NULL;
    head = NULL;
    heap_size = 0U;
    heap_used = 0U;
}
#endif

bool heap_ready(void) {
    return heap_initialized;
}

size_t heap_capacity_bytes(void) {
    return heap_initialized ? heap_size : 0U;
}

size_t heap_used_bytes(void) {
    return heap_initialized ? heap_used : 0U;
}

static size_t aligned_payload(size_t size) {
    if (size == 0U) {
        return 0U;
    }
    if (size > SIZE_MAX - (HFOC_HEAP_ALIGNMENT - 1U)) {
        return 0U;
    }
    size_t payload = (size + (HFOC_HEAP_ALIGNMENT - 1U)) &
                     ~(size_t)(HFOC_HEAP_ALIGNMENT - 1U);
    if (payload < HFOC_HEAP_MIN_BLOCK) {
        payload = HFOC_HEAP_MIN_BLOCK;
    }
    return payload;
}

static void split_block(heap_block *block, size_t payload) {
    const size_t remaining_bytes = block->payload_size - payload;
    if (remaining_bytes < HEAP_HEADER_SIZE + HFOC_HEAP_MIN_BLOCK) {
        return;
    }

    heap_block *const next = (heap_block *)(void *)(
        (uint8_t *)(void *)block + HEAP_HEADER_SIZE + payload);
    next->magic = HEAP_MAGIC;
    next->payload_size = remaining_bytes - HEAP_HEADER_SIZE;
    next->flags = BLOCK_FREE;
    next->reserved = 0U;
    next->prev = block;
    next->next = block->next;
    if (block->next != NULL) {
        block->next->prev = next;
    }
    block->next = next;
    block->payload_size = payload;
}

static void merge_forward(heap_block *block) {
    heap_block *const next = block->next;
    if (next == NULL || (next->flags & BLOCK_FREE) == 0U) {
        return;
    }
    block->payload_size = block->payload_size + HEAP_HEADER_SIZE +
                          next->payload_size;
    block->next = next->next;
    if (next->next != NULL) {
        next->next->prev = block;
    }
}

static void merge_backward(heap_block *block) {
    heap_block *const prev = block->prev;
    if (prev == NULL || (prev->flags & BLOCK_FREE) == 0U) {
        return;
    }
    prev->payload_size = prev->payload_size + HEAP_HEADER_SIZE +
                         block->payload_size;
    prev->next = block->next;
    if (block->next != NULL) {
        block->next->prev = prev;
    }
}

void *kmalloc(size_t size) {
    if (!heap_initialized) {
        return NULL;
    }
    const size_t payload = aligned_payload(size);
    if (payload == 0U || payload > SIZE_MAX - HEAP_HEADER_SIZE) {
        return NULL;
    }

    heap_block *block = head;
    while (block != NULL) {
        if ((block->flags & BLOCK_FREE) != 0U &&
            block->payload_size >= payload) {
            break;
        }
        block = block->next;
    }
    if (block == NULL) {
        return NULL;
    }

    split_block(block, payload);
    block->flags &= ~BLOCK_FREE;
    heap_used += block->payload_size;
    return (void *)(block + 1U);
}

bool kfree(void *ptr) {
    if (!heap_initialized || ptr == NULL) {
        return false;
    }

    const uintptr_t address = (uintptr_t)ptr;
    const uintptr_t base = (uintptr_t)heap_base;
    if (address < base ||
        address > UINTPTR_MAX - HEAP_HEADER_SIZE) {
        return false;
    }
    const uintptr_t header_address = address - HEAP_HEADER_SIZE;
    if (header_address < base ||
        header_address >= base + heap_size ||
        (header_address & (HFOC_HEAP_ALIGNMENT - 1U)) != 0U) {
        return false;
    }

    heap_block *const block = (heap_block *)(uintptr_t)header_address;
    if (block->magic != HEAP_MAGIC ||
        (block->flags & BLOCK_FREE) != 0U ||
        (uintptr_t)(block + 1U) != address) {
        return false;
    }

    heap_used -= block->payload_size;
    block->flags |= BLOCK_FREE;
    merge_forward(block);
    merge_backward(block);
    return true;
}

/* ITEM-626: a bounded bump arena.  No kernel heap, no unbounded growth. */
#include "hfoc.h"

static unsigned char arena[HFOC_ARENA_BYTES] __attribute__((aligned(16)));
static size_t arena_used;

void *hfoc_alloc(size_t bytes)
{
    const size_t aligned = (bytes + 15U) & ~(size_t)15U;

    if (aligned < bytes || arena_used > HFOC_ARENA_BYTES ||
        aligned > HFOC_ARENA_BYTES - arena_used) {
        return NULL;
    }
    {
        void *result = &arena[arena_used];

        arena_used += aligned;
        return result;
    }
}

void hfoc_alloc_reset(void)
{
    arena_used = 0U;
}

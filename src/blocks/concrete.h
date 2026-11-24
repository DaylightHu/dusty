#ifndef VOXILOG_CONCRETE_H
#define VOXILOG_CONCRETE_H

#include "rs/block.h"

enum {
    ORIGINAL_COLORED_CONCRETE_COUNT = BLOCK_YELLOW_CONCRETE - BLOCK_BLUE_CONCRETE + 1,
    COLORED_CONCRETE_COUNT = ORIGINAL_COLORED_CONCRETE_COUNT +
                            BLOCK_RED_CONCRETE - BLOCK_BLACK_CONCRETE + 1
};

/* The first five indices are already assigned in world-save palettes.
 * White concrete retains its own ID and palette entry (BLOCK_IRON). */
static inline int colored_concrete_index(BlockId id)
{
    if (id >= BLOCK_BLUE_CONCRETE && id <= BLOCK_YELLOW_CONCRETE)
        return id - BLOCK_BLUE_CONCRETE;
    if (id >= BLOCK_BLACK_CONCRETE && id <= BLOCK_RED_CONCRETE)
        return ORIGINAL_COLORED_CONCRETE_COUNT + id - BLOCK_BLACK_CONCRETE;
    return -1;
}

/* Callers pass a checked index in the colored concrete range. */
static inline BlockId colored_concrete_id(int index)
{
    if (index < ORIGINAL_COLORED_CONCRETE_COUNT)
        return (BlockId)(BLOCK_BLUE_CONCRETE + index);
    return (BlockId)(BLOCK_BLACK_CONCRETE + index - ORIGINAL_COLORED_CONCRETE_COUNT);
}

static inline bool block_is_concrete(BlockId id)
{
    return id == BLOCK_IRON || colored_concrete_index(id) >= 0;
}

#endif

#pragma once

#include <windows.h>

/*
 * Large memory blocks for document text, kept committed and touched in advance so that a file being
 * opened is written into pages Windows has already mapped. Blocks come in power of two sizes from
 * 1 MB up. The idle host prepares one block of each size up to its budget, and blocks given back
 * stay in the pool for the next file up to the same budget.
 */
#define BLOCK_MINIMUM (1024 * 1024)

/* A block of at least bytes; *capacity receives its size. NULL when memory runs out. */
void *BlockTake(size_t bytes, size_t *capacity);

/* Returns a block from BlockTake; it is kept for reuse or released. */
void BlockReturn(void *block, size_t capacity);

/* Total size of the blocks the pool keeps ready; 0 keeps none. */
void BlockSetBudget(size_t bytes);
size_t BlockBudget(void);

/* Prepares part of one missing block; TRUE when it did some work and should be called again. */
BOOL BlockPrepare(void);

/* Bytes held in the pool right now. */
size_t BlockPooledBytes(void);

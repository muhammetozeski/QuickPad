#include "blocks.h"
#include "quickpad.h"
#include "workers.h"

#include <intrin.h>

#define CLASS_COUNT 24           /* 1 MB .. 8 TB */
#define PAGE 4096
#define PREPARE_STEP (4 * 1024 * 1024)
#define TOUCH_PARALLEL_MINIMUM (8 * 1024 * 1024)

typedef struct FreeBlock {
    struct FreeBlock *next;
} FreeBlock;

static SRWLOCK poolLock = SRWLOCK_INIT;
static FreeBlock *freeLists[CLASS_COUNT];
static size_t pooledBytes;
static size_t budget;

/* A block being prepared in idle time, touched a few megabytes per call. */
static unsigned char *preparing;
static size_t preparingSize;
static size_t preparedBytes;

static int ClassOf(size_t bytes)
{
    size_t size = BLOCK_MINIMUM;
    int index = 0;
    while (size < bytes && index + 1 < CLASS_COUNT) {
        size <<= 1;
        ++index;
    }
    return index;
}

static size_t ClassSize(int index)
{
    return (size_t)BLOCK_MINIMUM << index;
}

typedef struct TouchWork {
    unsigned char *base;
    size_t size;
    size_t parts;
} TouchWork;

static void TouchPart(void *context, size_t index)
{
    TouchWork *work = context;
    size_t start = work->size * index / work->parts;
    size_t end = work->size * (index + 1) / work->parts;
    start = start & ~(size_t)(PAGE - 1);
    for (size_t i = start; i < end; i += PAGE) {
        work->base[i] = 0;
    }
}

/* Maps every page of a fresh block, on several threads when it is large. */
static void Touch(unsigned char *base, size_t size)
{
    TouchWork work = { base, size, 1 };
    if (size >= TOUCH_PARALLEL_MINIMUM) {
        int threads = WorkersInitialize() + 1;
        work.parts = (size_t)(threads > 1 ? threads : 1);
        WorkersRun(TouchPart, &work, work.parts);
    } else {
        TouchPart(&work, 0);
    }
}

void *BlockTake(size_t bytes, size_t *capacity)
{
    int index = ClassOf(bytes);
    if (ClassSize(index) < bytes) {
        *capacity = 0;
        return NULL;
    }

    AcquireSRWLockExclusive(&poolLock);
    FreeBlock *block = NULL;
    int found = index;
    /* The smallest ready block that fits, so a small request does not use up the block for a large file. */
    for (int i = index; i < CLASS_COUNT && block == NULL; ++i) {
        if (freeLists[i] != NULL) {
            block = freeLists[i];
            freeLists[i] = block->next;
            found = i;
        }
    }
    if (block != NULL) {
        pooledBytes -= ClassSize(found);
    }
    ReleaseSRWLockExclusive(&poolLock);

    if (block != NULL) {
        *capacity = ClassSize(found);
        return block;
    }

    size_t size = ClassSize(index);
    unsigned char *fresh = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (fresh == NULL) {
        *capacity = 0;
        return NULL;
    }
    Touch(fresh, size);
    *capacity = size;
    return fresh;
}

void BlockReturn(void *block, size_t capacity)
{
    if (block == NULL) {
        return;
    }
    int index = ClassOf(capacity);
    BOOL kept = FALSE;
    AcquireSRWLockExclusive(&poolLock);
    if (ClassSize(index) == capacity && pooledBytes + capacity <= budget) {
        FreeBlock *entry = block;
        entry->next = freeLists[index];
        freeLists[index] = entry;
        pooledBytes += capacity;
        kept = TRUE;
    }
    ReleaseSRWLockExclusive(&poolLock);
    if (!kept) {
        VirtualFree(block, 0, MEM_RELEASE);
    }
}

void BlockSetBudget(size_t bytes)
{
    AcquireSRWLockExclusive(&poolLock);
    budget = bytes;
    /* Blocks beyond the new budget are released, largest first. */
    for (int i = CLASS_COUNT - 1; i >= 0 && pooledBytes > budget; --i) {
        while (freeLists[i] != NULL && pooledBytes > budget) {
            FreeBlock *block = freeLists[i];
            freeLists[i] = block->next;
            pooledBytes -= ClassSize(i);
            VirtualFree(block, 0, MEM_RELEASE);
        }
    }
    ReleaseSRWLockExclusive(&poolLock);
}

size_t BlockBudget(void)
{
    return budget;
}

size_t BlockPooledBytes(void)
{
    return pooledBytes;
}

/* The smallest class missing from the pool whose block fits the budget, or -1. */
static int MissingClass(void)
{
    int missing = -1;
    AcquireSRWLockExclusive(&poolLock);
    for (int i = 0; i < CLASS_COUNT; ++i) {
        if (pooledBytes + ClassSize(i) > budget) {
            break;
        }
        if (freeLists[i] == NULL) {
            missing = i;
            break;
        }
    }
    ReleaseSRWLockExclusive(&poolLock);
    return missing;
}

BOOL BlockPrepare(void)
{
    if (preparing == NULL) {
        int index = MissingClass();
        if (index < 0) {
            return FALSE;
        }
        preparingSize = ClassSize(index);
        preparing = VirtualAlloc(NULL, preparingSize, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        preparedBytes = 0;
        if (preparing == NULL) {
            return FALSE;
        }
    }

    size_t end = preparedBytes + PREPARE_STEP < preparingSize ? preparedBytes + PREPARE_STEP : preparingSize;
    for (size_t i = preparedBytes; i < end; i += PAGE) {
        preparing[i] = 0;
    }
    preparedBytes = end;
    if (preparedBytes < preparingSize) {
        return TRUE;
    }

    unsigned char *block = preparing;
    size_t size = preparingSize;
    preparing = NULL;
    BlockReturn(block, size);
    return TRUE;
}

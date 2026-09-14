/* Tests for src/workers.c and src/blocks.c. */
#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "blocks.h"
#include "quickpad.h"
#include "workers.h"

static int failures;
static int checks;

#define CHECK(condition, name)                                                  \
    do {                                                                        \
        ++checks;                                                               \
        if (!(condition)) {                                                     \
            ++failures;                                                         \
            wprintf(L"FAIL %hs: %hs (line %d)\n", name, #condition, __LINE__);  \
        }                                                                       \
    } while (0)

typedef struct SumWork {
    volatile LONG *cells;
    volatile LONG completions;
} SumWork;

static void Square(void *context, size_t index)
{
    SumWork *work = context;
    work->cells[index] = (LONG)(index * index);
}

static void Completed(void *context)
{
    SumWork *work = context;
    InterlockedIncrement(&work->completions);
}

static void Slow(void *context, size_t index)
{
    SumWork *work = context;
    Sleep(1);
    InterlockedIncrement(&work->cells[index % 4]);
}

int wmain(void)
{
    int threads = WorkersInitialize();
    CHECK(threads >= 1, "worker threads exist");
    CHECK(WorkersInitialize() == threads, "a second initialization changes nothing");

    enum { COUNT = 5000 };
    static volatile LONG cells[COUNT];
    SumWork work = { cells, 0 };
    WorkersRun(Square, &work, COUNT);
    BOOL allSet = TRUE;
    for (size_t i = 0; i < COUNT; ++i) {
        allSet = allSet && cells[i] == (LONG)(i * i);
    }
    CHECK(allSet, "every index of a synchronous job ran once");

    memset((void *)cells, 0, sizeof cells);
    WorkerJob job;
    CHECK(WorkersStart(&job, Square, Completed, &work, COUNT), "an asynchronous job starts");
    WorkersWait(&job);
    CHECK(work.completions == 1, "the completion ran once");
    CHECK(WorkersIsDone(&job), "the job is done after waiting");
    allSet = TRUE;
    for (size_t i = 0; i < COUNT; ++i) {
        allSet = allSet && cells[i] == (LONG)(i * i);
    }
    CHECK(allSet, "every index of an asynchronous job ran once");

    memset((void *)cells, 0, sizeof cells);
    WorkerJob first;
    WorkerJob second;
    CHECK(WorkersStart(&first, Slow, NULL, &work, 40) && WorkersStart(&second, Slow, NULL, &work, 40), "two jobs queue");
    WorkersWait(&second);
    WorkersWait(&first);
    CHECK(cells[0] + cells[1] + cells[2] + cells[3] == 80, "both queued jobs ran every index");

    WorkerJob empty;
    CHECK(WorkersStart(&empty, Square, NULL, &work, 0), "an empty job starts");
    CHECK(WorkersIsDone(&empty), "an empty job is done at once");
    WorkersWait(&empty);

    /* Blocks: sizes, reuse and the budget. */
    BlockSetBudget(8 * 1024 * 1024);
    size_t capacity = 0;
    void *block = BlockTake(100, &capacity);
    CHECK(block != NULL && capacity == BLOCK_MINIMUM, "a small request gets the smallest block");
    memset(block, 0xAB, capacity);
    BlockReturn(block, capacity);
    CHECK(BlockPooledBytes() == BLOCK_MINIMUM, "a returned block stays in the pool");
    size_t again = 0;
    void *reused = BlockTake(BLOCK_MINIMUM / 2, &again);
    CHECK(reused == block && again == capacity, "the pooled block is handed out again");
    CHECK(BlockPooledBytes() == 0, "the pool is empty after that");
    size_t large = 0;
    void *big = BlockTake(3 * 1024 * 1024, &large);
    CHECK(big != NULL && large == 4 * 1024 * 1024, "sizes round up to a power of two");
    memset(big, 1, large);
    BlockReturn(big, large);
    BlockReturn(reused, again);
    CHECK(BlockPooledBytes() == 5 * 1024 * 1024, "both blocks are pooled within the budget");
    size_t huge = 0;
    void *over = BlockTake(16 * 1024 * 1024, &huge);
    CHECK(over != NULL && huge == 16 * 1024 * 1024, "a block beyond the budget is still given out");
    BlockReturn(over, huge);
    CHECK(BlockPooledBytes() == 5 * 1024 * 1024, "a block that does not fit the budget is released");
    int steps = 0;
    while (BlockPrepare() && steps < 100) {
        ++steps;
    }
    CHECK(steps > 0 && BlockPooledBytes() == 7 * 1024 * 1024, "preparing fills the missing sizes up to the budget");
    BlockSetBudget(1024 * 1024);
    CHECK(BlockPooledBytes() <= 1024 * 1024, "lowering the budget releases blocks");
    BlockSetBudget(0);
    CHECK(BlockPooledBytes() == 0, "a budget of zero keeps nothing");

    wprintf(L"%d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}

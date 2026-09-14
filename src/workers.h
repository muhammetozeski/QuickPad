#pragma once

#include <windows.h>

/*
 * A pool of threads created once and kept waiting, so work can be spread over every processor
 * without starting a thread first. A job is a task run for each index in [0, count); the threads
 * take indices as they become free. The thread that finishes the last index runs the job's
 * completion function, on that thread, before the job is signalled as done.
 */
typedef void WorkerTask(void *context, size_t index);
typedef void WorkerCompletion(void *context);

typedef struct WorkerJob {
    WorkerTask *task;
    WorkerCompletion *completion;
    void *context;
    size_t count;
    volatile LONG next;        /* first index nobody has taken yet */
    volatile LONG remaining;   /* indices not finished yet */
    HANDLE done;               /* manual reset event, set when remaining reaches zero */
    struct WorkerJob *queued;  /* next job in the pool's queue */
} WorkerJob;

/* Creates the threads; safe to call more than once. Returns how many threads the pool has. */
int WorkersInitialize(void);

/* Starts running task for count indices. job must stay valid until WorkersWait returns. */
BOOL WorkersStart(WorkerJob *job, WorkerTask *task, WorkerCompletion *completion, void *context, size_t count);

/* Helps with the job's remaining indices, then waits until every index is finished. */
void WorkersWait(WorkerJob *job);

/* WorkersStart followed by WorkersWait. */
void WorkersRun(WorkerTask *task, void *context, size_t count);

/* TRUE once every index of the job has finished. */
BOOL WorkersIsDone(const WorkerJob *job);

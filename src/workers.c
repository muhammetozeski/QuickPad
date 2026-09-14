#include "workers.h"
#include "quickpad.h"

#include <limits.h>

#define WORKER_STACK_SIZE (256 * 1024)
#define MAX_WORKERS 63

static SRWLOCK queueLock = SRWLOCK_INIT;
static WorkerJob *queueHead;
static HANDLE wakeSemaphore;
static volatile int workerCount;
static volatile LONG initialized;

/* Takes one index of the first queued job that still has indices; FALSE when there is none. */
static BOOL TakeIndex(WorkerJob **job, size_t *index)
{
    AcquireSRWLockExclusive(&queueLock);
    WorkerJob *candidate = queueHead;
    while (candidate != NULL && (size_t)candidate->next >= candidate->count) {
        candidate = candidate->queued;
    }
    if (candidate != NULL) {
        *job = candidate;
        *index = (size_t)candidate->next++;
        if ((size_t)candidate->next >= candidate->count) {
            /* Every index is taken; the job leaves the queue and is finished by whoever runs its last index. */
            WorkerJob **link = &queueHead;
            while (*link != candidate) {
                link = &(*link)->queued;
            }
            *link = candidate->queued;
            candidate->queued = NULL;
        }
    }
    ReleaseSRWLockExclusive(&queueLock);
    return candidate != NULL;
}

static void RunIndex(WorkerJob *job, size_t index)
{
    job->task(job->context, index);
    if (InterlockedDecrement(&job->remaining) == 0) {
        if (job->completion != NULL) {
            job->completion(job->context);
        }
        SetEvent(job->done);
    }
}

static DWORD WINAPI WorkerMain(LPVOID parameter)
{
    UNREFERENCED_PARAMETER(parameter);
    for (;;) {
        WaitForSingleObject(wakeSemaphore, INFINITE);
        WorkerJob *job = NULL;
        size_t index = 0;
        while (TakeIndex(&job, &index)) {
            RunIndex(job, index);
        }
    }
}

int WorkersInitialize(void)
{
    if (InterlockedCompareExchange(&initialized, 1, 0) != 0) {
        while (workerCount == 0 && initialized == 1) {
            SwitchToThread();
        }
        return workerCount;
    }

    SYSTEM_INFO system;
    GetSystemInfo(&system);
    int wanted = (int)system.dwNumberOfProcessors - 1;
    wanted = wanted < 1 ? 1 : wanted > MAX_WORKERS ? MAX_WORKERS : wanted;
    wakeSemaphore = CreateSemaphoreW(NULL, 0, LONG_MAX, NULL);
    int created = 0;
    for (int i = 0; wakeSemaphore != NULL && i < wanted; ++i) {
        HANDLE thread = CreateThread(NULL, WORKER_STACK_SIZE, WorkerMain, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (thread == NULL) {
            break;
        }
        SetThreadPriority(thread, THREAD_PRIORITY_ABOVE_NORMAL);
        CloseHandle(thread);
        ++created;
    }
    workerCount = created;
    if (created == 0) {
        initialized = 2;
    }
    return created;
}

BOOL WorkersStart(WorkerJob *job, WorkerTask *task, WorkerCompletion *completion, void *context, size_t count)
{
    WorkersInitialize();
    job->task = task;
    job->completion = completion;
    job->context = context;
    job->count = count;
    job->next = 0;
    job->remaining = (LONG)count;
    job->queued = NULL;
    job->done = CreateEventW(NULL, TRUE, count == 0, NULL);
    if (job->done == NULL) {
        return FALSE;
    }
    if (count == 0) {
        return TRUE;
    }

    AcquireSRWLockExclusive(&queueLock);
    WorkerJob **link = &queueHead;
    while (*link != NULL) {
        link = &(*link)->queued;
    }
    *link = job;
    ReleaseSRWLockExclusive(&queueLock);

    if (workerCount > 0) {
        LONG wake = (LONG)(count < (size_t)workerCount ? count : (size_t)workerCount);
        ReleaseSemaphore(wakeSemaphore, wake, NULL);
    }
    return TRUE;
}

void WorkersWait(WorkerJob *job)
{
    if (job->done == NULL) {
        return;
    }
    /* Indices nobody has started yet are run here rather than waited for. */
    for (;;) {
        AcquireSRWLockExclusive(&queueLock);
        BOOL available = (size_t)job->next < job->count;
        size_t index = available ? (size_t)job->next++ : 0;
        if (available && (size_t)job->next >= job->count) {
            WorkerJob **link = &queueHead;
            while (*link != NULL && *link != job) {
                link = &(*link)->queued;
            }
            if (*link == job) {
                *link = job->queued;
                job->queued = NULL;
            }
        }
        ReleaseSRWLockExclusive(&queueLock);
        if (!available) {
            break;
        }
        RunIndex(job, index);
    }
    WaitForSingleObject(job->done, INFINITE);
    CloseHandle(job->done);
    job->done = NULL;
}

void WorkersRun(WorkerTask *task, void *context, size_t count)
{
    WorkerJob job;
    if (WorkersStart(&job, task, NULL, context, count)) {
        WorkersWait(&job);
    } else {
        for (size_t i = 0; i < count; ++i) {
            task(context, i);
        }
    }
}

BOOL WorkersIsDone(const WorkerJob *job)
{
    return job->done == NULL || job->remaining == 0;
}

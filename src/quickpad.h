#pragma once

#include <windows.h>

#define QP_APP_NAME L"QuickPad"

static inline void *MemAlloc(size_t size)
{
    return HeapAlloc(GetProcessHeap(), 0, size);
}

static inline void *MemAllocZero(size_t size)
{
    return HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
}

static inline void MemFree(void *block)
{
    if (block != NULL) {
        HeapFree(GetProcessHeap(), 0, block);
    }
}

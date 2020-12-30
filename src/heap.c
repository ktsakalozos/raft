#include "heap.h"

#include <stdlib.h>
#include <unistd.h>

#include <execinfo.h>
#include <stdio.h>

#include "../include/raft.h"

static void *defaultMalloc(void *data, size_t size)
{
    (void)data;
    return malloc(size);
}

static void defaultFree(void *data, void *ptr)
{
    (void)data;
    free(ptr);
}

static void *defaultCalloc(void *data, size_t nmemb, size_t size)
{
    (void)data;
    return calloc(nmemb, size);
}

static void *defaultRealloc(void *data, void *ptr, size_t size)
{
    (void)data;
    return realloc(ptr, size);
}

static void *defaultAlignedAlloc(void *data, size_t alignment, size_t size)
{
    (void)data;
    return aligned_alloc(alignment, size);
}

static void defaultAlignedFree(void *data, size_t alignment, void *ptr)
{
    (void)alignment;
    defaultFree(data, ptr);
}

static struct raft_heap defaultHeap = {
    NULL,                /* data */
    defaultMalloc,       /* malloc */
    defaultFree,         /* free */
    defaultCalloc,       /* calloc */
    defaultRealloc,      /* realloc */
    defaultAlignedAlloc, /* aligned_alloc */
    defaultAlignedFree   /* aligned_free */
};

static struct raft_heap *currentHeap = &defaultHeap;

void Print_call_stack(void)
{
    int j, nptrs;
#define SIZE 100
    void *buffer[100];
    char **strings;

    nptrs = backtrace(buffer, SIZE);
    printf("backtrace() returned %d addresses\n", nptrs);

    /* The call backtrace_symbols_fd(buffer, nptrs, STDOUT_FILENO)
        would produce similar output to the following: */

    strings = backtrace_symbols(buffer, nptrs);
    if (strings == NULL) {
        perror("backtrace_symbols");
        return;
    }

    for (j = 0; j < nptrs; j++)
        printf("%s\n", strings[j]);

    free(strings);
}

void *HeapMalloc(size_t size)
{
    if (size > (20*1024*1024)){
          printf("%d: HealMalloc (inside) %lu\n", getpid(), size); fflush(stdout);
          Print_call_stack();
    }
    return currentHeap->malloc(currentHeap->data, size);
}

void HeapFree(void *ptr)
{
    if (ptr == NULL) {
        return;
    }
    currentHeap->free(currentHeap->data, ptr);
}

void *HeapCalloc(size_t nmemb, size_t size)
{
    if (size > (10*1024*1024)){
        printf("%d: HealCalloc (inside) %lu\n", getpid(), size); fflush(stdout);
    }
    return currentHeap->calloc(currentHeap->data, nmemb, size);
}

void *HeapRealloc(void *ptr, size_t size)
{
    return currentHeap->realloc(currentHeap->data, ptr, size);
}

void *raft_malloc(size_t size)
{
    return HeapMalloc(size);
}

void raft_free(void *ptr)
{
    HeapFree(ptr);
}

void *raft_calloc(size_t nmemb, size_t size)
{
    return HeapCalloc(nmemb, size);
}

void *raft_realloc(void *ptr, size_t size)
{
    return HeapRealloc(ptr, size);
}

void *raft_aligned_alloc(size_t alignment, size_t size)
{
    return currentHeap->aligned_alloc(currentHeap->data, alignment, size);
}

void raft_aligned_free(size_t alignment, void *ptr)
{
    currentHeap->aligned_free(currentHeap->data, alignment, ptr);
}

void raft_heap_set(struct raft_heap *heap)
{
    currentHeap = heap;
}

void raft_heap_set_default(void)
{
    currentHeap = &defaultHeap;
}

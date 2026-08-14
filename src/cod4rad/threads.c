/*
 * threads.c — Multi-threaded work dispatch with per-resource locking.
 */

#include "cod2rad64.h"

#define MAX_THREADS      64
#define NUM_THREAD_LOCKS 4099

static CRITICAL_SECTION g_threadLocks[NUM_THREAD_LOCKS];
static int g_threadLocksInitialized;
static void (*g_threadWorkFunc)(unsigned int, unsigned int);
static int g_threadWorkCounter;
static int g_threadWorkEnd;
static int g_threadCount;

/*
================
StartAddress

Thread worker function passed to CreateThread.
Grabs work items from shared counter under critical section.
================
*/
static DWORD WINAPI StartAddress(LPVOID lpThreadParameter)
{
    unsigned int threadId = (unsigned int)(uintptr_t)lpThreadParameter;
    long workIndex;

    for (;;)
    {
        workIndex = InterlockedIncrement((volatile long *)&g_threadWorkCounter) - 1;
        if (workIndex >= g_threadWorkEnd)
            return 0;
        UpdateProgress(1);
        g_threadWorkFunc((unsigned int)workIndex, threadId);
    }
}

/*
================
ThreadDispatch

Creates N threads and waits for all to complete.
Uses NUM_THREAD_LOCKS critical sections for per-resource locking.
================
*/
static void ThreadDispatch(void (*workFunc)(unsigned int, unsigned int), unsigned int threadCount)
{
    int i;
    HANDLE *handles;
    DWORD threadId;

    handles = (HANDLE *)malloc(threadCount * sizeof(HANDLE));

    if (!g_threadLocksInitialized)
    {
        for (i = 0; i < NUM_THREAD_LOCKS; i++)
            InitializeCriticalSection(&g_threadLocks[i]);
        g_threadLocksInitialized = 1;
    }

    g_threadWorkCounter = 0;
    g_threadWorkFunc = workFunc;

    for (i = 0; i < (int)threadCount; i++)
        handles[i] = CreateThread(NULL, 0, StartAddress, (LPVOID)(uintptr_t)i, 0, &threadId);

    for (i = 0; i < (int)threadCount; i += MAXIMUM_WAIT_OBJECTS)
    {
        int batch = (int)threadCount - i;
        if (batch > MAXIMUM_WAIT_OBJECTS) batch = MAXIMUM_WAIT_OBJECTS;
        WaitForMultipleObjects(batch, &handles[i], TRUE, INFINITE);
    }

    free(handles);
}

/*
================
ForEachQuantum

Dispatches work items across threads. If threadCount==1, runs single-threaded.
================
*/
void ForEachQuantum(unsigned int count, void (*workFunc)(unsigned int, unsigned int), unsigned int threadCount)
{
    int i;

    g_threadWorkEnd = count;
    SetProgress(0, count);
    g_threadCount = threadCount;

    if (threadCount == 1)
    {
        for (i = 0; i < (int)count; i++)
        {
            workFunc(i, 0);
            UpdateProgress(1);
        }
    }
    else
    {
        ThreadDispatch(workFunc, threadCount);
    }
}

/* Thin forward — cod2rad's lighting/lightgrid code calls this name while the real
 * dispatcher is ForEachQuantum. Callback signature is cast since the trampolines
 * at lighting_412230/412260 only use the first arg. */
void ForEachLightmapPixel(int count, void *callback, int threadCount)
{
    ForEachQuantum((unsigned int)count,
                   (void (*)(unsigned int, unsigned int))callback,
                   (unsigned int)threadCount);
}

/*
================
AcquireThreadLock

Lock a resource by index. Skipped if single-threaded.
================
*/
void AcquireThreadLock(unsigned int lockIndex)
{
    if (g_threadCount != 1)
        EnterCriticalSection(&g_threadLocks[lockIndex % NUM_THREAD_LOCKS]);
}

/*
================
ReleaseThreadLock

Unlock a resource by index. Skipped if single-threaded.
================
*/
void ReleaseThreadLock(unsigned int lockIndex)
{
    if (g_threadCount != 1)
        LeaveCriticalSection(&g_threadLocks[lockIndex % NUM_THREAD_LOCKS]);
}

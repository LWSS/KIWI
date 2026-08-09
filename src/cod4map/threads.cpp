/*
threads.c — Threading utilities

Reconstructed from cod2map.exe by Rose.
*/

#include "cod4map.h"

struct _RTL_CRITICAL_SECTION CriticalSection;

int   g_threadActive;
int   g_threadDispatch;
int   g_threadLocked;
int   g_threadOldProgress;
int   g_threadShowPacifier;
int   g_threadWorkCount;
int (*g_threadWorkFunction)(int);


/*
================
ThreadSetDefault

Detects CPU count and sets numthreads.
================
*/
int ThreadSetDefault(void)
{
  SYSTEM_INFO si;

  if ( numthreads == -1 )
  {
    GetSystemInfo(&si);
    numthreads = si.dwNumberOfProcessors;
    if ( numthreads < 1 || numthreads > MAX_THREADS )
      numthreads = 1;
  }

  return Com_DPrintf("%i threads\n", numthreads);
}

/*
================
RunThreadsOn

Creates worker threads and waits for completion.
================
*/
int RunThreadsOn(int workcnt, int showpacifier, LPTHREAD_START_ROUTINE func)
{
  int i, startTime, endTime;
  HANDLE threadHandles[128];

  startTime = (int)I_FloatTime();
  g_threadDispatch = 0;
  g_threadWorkCount = workcnt;
  g_threadOldProgress = -1;
  g_threadShowPacifier = showpacifier;
  g_threadActive = 1;
  InitializeCriticalSection(&CriticalSection);

  if ( numthreads == 1 )
  {
    func(0);
  }
  else
  {
    /* spawn worker threads */
    for ( i = 0; i < numthreads; i++ )
      threadHandles[i] = CreateThread(NULL, 0, func, (LPVOID)(intptr_t)i, 0, (LPDWORD)&threadHandles[i + 64]);

    /* wait for all threads to finish */
    for ( i = 0; i < numthreads; i++ )
      WaitForSingleObject(threadHandles[i], INFINITE);
  }

  DeleteCriticalSection(&CriticalSection);
  g_threadActive = 0;
  endTime = (int)I_FloatTime();

  if ( g_threadShowPacifier )
    return Com_Printf(" (%i)\n", endTime - startTime);

  return endTime;
}

/*
================
GetThreadWork

Thread-safe work dispatcher, returns next item index or -1 when done.
================
*/
int GetThreadWork(void)
{
  int workIdx;

  /* lock */
  if ( g_threadActive )
  {
    EnterCriticalSection(&CriticalSection);
    if ( g_threadLocked )
      Com_Error("Recursive ThreadLock\n");
    g_threadLocked = 1;
  }

  /* check if all work is done */
  if ( g_threadDispatch == g_threadWorkCount )
  {
    workIdx = -1;
  }
  else
  {
    /* progress display */
    if ( 10 * g_threadDispatch / g_threadWorkCount != g_threadOldProgress )
    {
      g_threadOldProgress = 10 * g_threadDispatch / g_threadWorkCount;
      if ( g_threadShowPacifier )
        Com_Printf("%i...", g_threadOldProgress);
    }

    workIdx = g_threadDispatch++;
  }

  /* unlock */
  if ( g_threadActive )
  {
    if ( !g_threadLocked )
      Com_Error("ThreadUnlock without lock\n");
    g_threadLocked = 0;
    LeaveCriticalSection(&CriticalSection);
  }

  return workIdx;
}

/*
================
ThreadWorkerFunction

Worker thread loop, gets work items and calls the work function.
================
*/
int ThreadWorkerFunction(int threadnum)
{
  int work;

  while ( (work = GetThreadWork()) != -1 )
  {
    Com_DPrintf("thread %i, work %i\n", threadnum, work);
    g_threadWorkFunction(work);
  }

  return work;
}

/*
================
RunThreadsOnIndividual

Sets the work function and dispatches via RunThreadsOn.
================
*/
int RunThreadsOnIndividual(int workcnt, int showpacifier, LPTHREAD_START_ROUTINE func)
{
  if ( numthreads == -1 )
    ThreadSetDefault();
  g_threadWorkFunction = (int (*)(int))func;
  return RunThreadsOn(workcnt, showpacifier, (LPTHREAD_START_ROUTINE)ThreadWorkerFunction);
}

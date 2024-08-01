#include "test.h"

#include "atomic.h"
#include "debug.h"
#include "event.h"
#include "mutex.h"
#include "semaphore.h"
#include "thread.h"
#include "heap.h"

#include <assert.h>
#include <stdbool.h>

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>

#pragma comment(lib, "winmm.lib")

#define LARGENUMBER 1000000

// ================================================
//					FILE I/O TEST
// ================================================


// ================================================
//					ALLOCATION TEST
// ================================================
void testLeakedHeapAllocation() {
	heap_t* heap = heapCreate(4096);
	void* block = heapAlloc(heap, 16 * 1024, 8);
	/* leaked memory */ heapAlloc(heap, 256, 8);
	/* leaked memory */ heapAlloc(heap, 16 * 1024, 8);
	heapFree(heap, block);
	heapDestroy(heap);

	// assert that the backtrace returns 
	// assert(!debugBacktraceManually());
}

// ================================================
//					THREADING TEST
// ================================================
typedef struct thread_info_t {
	int* count;
	mutex_t* mutex;
	event_t* _event;
} thread_info_t;

static int noSynchronizationTestFunc(void* user) {
	thread_info_t* thread_data = (thread_info_t*) user;
	eventWait(thread_data->_event);

	DWORD start = timeGetTime();

	for (int x = 0; x < LARGENUMBER; x++){
		*thread_data->count = *thread_data->count + 1;
	}

	return timeGetTime() - start;
}

static int atomicReadWriteTestFunc(void* user){
	thread_info_t* thread_data = (thread_info_t*)user;
	eventWait(thread_data->_event);

	DWORD start = timeGetTime();

	for (int x = 0; x < LARGENUMBER; x++){
		atomicWrite(thread_data->count, atomicRead(thread_data->count) + 1);
	}

	return timeGetTime() - start;
}

static int atomicIncrementTestFunc(void* user){
	thread_info_t* thread_data = (thread_info_t*)user;
	eventWait(thread_data->_event);

	DWORD start = timeGetTime();

	for (int x = 0; x < LARGENUMBER; x++){
		atomicInc(thread_data->count);
	}

	return timeGetTime() - start;
}

static int mutexTestFunc(void* user){
	thread_info_t* thread_data = (thread_info_t*)user;
	eventWait(thread_data->_event);

	DWORD start = timeGetTime();

	for (int x = 0; x < LARGENUMBER; x++){
		mutexLock(thread_data->mutex);
		*thread_data->count = *thread_data->count + 1;
		mutexUnlock(thread_data->mutex);
	}

	return timeGetTime() - start;
}

static void runThreadBenchmark(int(*function)(void*), const char* name) {
	int count = 0;
	thread_info_t info = {
		.count = &count,
		.mutex = mutexCreate(),
		._event = eventCreate()
	};

	thread_t* threads[8];
	for (int x = 0; x < _countof(threads); x++) {
		threads[x] = threadCreate(function, &info);
	}

	// start
	eventSignal(info._event);

	// Wait for threads
	int duration = 0;
	for (int x = 0; x < _countof(threads); x++) {
		duration += threadRun(threads[x]);
	}

	mutexDestroy(info.mutex);
	eventDestroy(info._event);

	debugPrintConsole(
		"[ %s ] ---- Elapsed: %3.3f s ---- Count: %d\n", 
		name, duration / 1000.0, count);
}


/*
[ noSyncTest ] ---- Elapsed: 0.374 s ---- Count: 1434803
[ atomicRW ] ---- Elapsed: 0.569 s ---- Count: 1284107
[ atomicInc ] ---- Elapsed: 1.102 s ---- Count: 8000000
[ mutex ] ---- Elapsed: 386.389 s ---- Count: 8000000
*/
void testThreading() {
	runThreadBenchmark(noSynchronizationTestFunc, "noSyncTest");
	runThreadBenchmark(atomicReadWriteTestFunc, "atomicRW");
	runThreadBenchmark(atomicIncrementTestFunc, "atomicInc");
	runThreadBenchmark(mutexTestFunc, "mutex");
}


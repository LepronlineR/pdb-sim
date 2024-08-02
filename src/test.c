#include "test.h"

#include "atomic.h"
#include "debug.h"
#include "event.h"
#include "mutex.h"
#include "semaphore.h"
#include "thread.h"
#include "heap.h"
#include "fs.h"

#include <assert.h>
#include <stdbool.h>

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#pragma comment(lib, "winmm.lib")
#include <Shlwapi.h> // Include for PathFileExists
#pragma comment(lib, "Shlwapi.lib")

#define LARGENUMBER 1000000

// ================================================
//					TRACING TEST
// ================================================

void testTraceSlowerFunction(trace_t* trace) {
	traceDurationPush(trace, "slower_function");
	threadSleep(500);
	traceDurationPop(trace);
}

void testTraceSlowFunction(trace_t* trace) {
	traceDurationPush(trace, "slow_function");
	threadSleep(200);
	testTraceSlowerFunction(trace);
	traceDurationPop(trace);
}

int testTraceFunc(void* data) {
	trace_t* trace = data;
	testTraceSlowFunction(trace);
	return 0;
}

void testTrace() {
	heap_t* heap = heapCreate(4096);

	// Create the tracing system with at least space for 100 *captured* events.
	// Each call to trace_duration_push is an event.
	// Each call to trace_duration_pop is an event.
	// Before trace_capture_start is called, and after trace_capture_stop is called,
	// duration events should not be generated.
	trace_t* trace = traceCreate(heap, 100);

	// Capturing has *not* started so these calls can safely be ignored.
	traceDurationPush(trace, "should be ignored");
	traceDurationPop(trace);

	// Start capturing events.
	// Eventually we will want to write events to a file -- "trace.json".
	// However we should *not* write to the file for each call to trace_duration_push or
	// trace_duration_pop. That would be much too slow. Instead, events should be buffered
	// (up to event_capacity) before writing to a file. For purposes of this homework,
	// it is entirely fine if you only capture the first event_capacity count events and
	// ignore any additional events.
	traceCaptureStart(trace, "trace.json");

	// Create a thread that will push/pop duration events.
	thread_t* thread = threadCreate(testTraceFunc, trace);

	// Call a function that will push/pop duration events.
	testTraceSlowFunction(trace);

	// Wait for thread to finish.
	threadDestroy(thread);

	// Finish capturing. Write the trace.json file in Chrome tracing format.
	traceCaptureStop(trace);

	traceDestroy(trace);

	heapDestroy(heap);
}

// ================================================
//					FILE I/O TEST
// ================================================

void testReadWriteAndCompression(heap_t* heap, fs_t* fs) {

	// =============== READ FILE (HARRY POTTER BOOK 1) ================

	fs_work_t* read_file_work = fsRead(fs, "assets\\fiotest.test", heap, true, false);
	fsWorkBlock(read_file_work);

	char* harry_potter = fsWorkGetBuffer(read_file_work);
	const size_t harry_potter_len = strlen(harry_potter);


	// =================== COMPRESS AND WRITE FILE ====================

	const char* write_data = harry_potter;
	fs_work_t* write_work = fsWrite(fs, "assets\\compressed.bar", write_data, harry_potter_len, true);
	fsWorkBlock(write_work);

	assert(fsWorkGetErrorCode(write_work) == 0);
	assert(fsWorkGetSize(write_work) == harry_potter_len);

	// ===================== READ COMPRESSED FILE =====================

	fs_work_t* read_work = fsRead(fs, "assets\\compressed.bar", heap, true, true);
	fsWorkBlock(read_work);

	// ===================== COMPARE TO PREV DATA =====================

	char* read_data = fsWorkGetBuffer(read_work);
	assert(read_data && strcmp(read_data, harry_potter) == 0);
	assert(fsWorkGetErrorCode(read_work) == 0);
	assert(fsWorkGetSize(read_work) == harry_potter_len);

	fsWorkDestroy(read_file_work);
	fsWorkDestroy(read_work);
	fsWorkDestroy(write_work);

	debugPrint(DEBUG_PRINT_INFO, "File I/O Test Success!\n");
}

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


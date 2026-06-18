#ifndef __TEST_H__
#define __TEST_H__

// typedef struct test_t test_t; use this for benchmarking + assertions + testing suites later

#include "thread.h"
#include "heap.h"
#include "fs.h"
#include "trace.h"

void testTraceSlowerFunction(trace_t* trace);
void testTraceSlowFunction(trace_t* trace);
int testTraceFunc(void* data);
void testTrace();

void testReadWriteAndCompression(heap_t* heap, fs_t* fs);

void testLeakedHeapAllocation();

typedef struct thread_data_t thread_data_t;
typedef struct performance_counter_t performance_counter_t;

int noSynchronizationTestFunc(void* user);
int atomicReadWriteTestFunc(void* user);
int atomicIncrementTestFunc(void* user);
int mutexTestFunc(void* user);
void runThreadBenchmark(int (*function)(void*), const char* name);
void testThreading();

#endif

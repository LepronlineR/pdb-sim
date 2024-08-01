#ifndef __TEST_H__
#define __TEST_H__

// typedef struct test_t test_t; use this for benchmarking + assertions + testing suites later

#include "thread.h"

void testLeakedHeapAllocation();

typedef struct thread_data_t thread_data_t;
typedef struct performance_counter_t performance_counter_t;

static int noSynchronizationTestFunc(void* user);
static int atomicReadWriteTestFunc(void* user);
static int atomicIncrementTestFunc(void* user);
static int mutexTestFunc(void* user);
static void runThreadBenchmark(int (*function)(void*), const char* name);

void testThreading();

#endif
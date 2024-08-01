#ifndef __THREAD_H__
#define __THREAD_H__

#include <stdint.h>

/* THREADS FOR MULTITHREADING
*/

typedef struct thread_t thread_t;

// Creates a new thread to run the function with data
//
// RETURN: the newly created thread
thread_t* threadCreate(int (*function)(void*), void* data);

// Waits for a thread to finish running and destroys it.
// 
// RETURN: the return value (code) for the thread function
int threadRun(thread_t* thread);

// Puts the calling thread to sleep for the specified number of milliseconds.
// 
void threadSleep(uint32_t ms);

#endif
#ifndef __SEMAPHORE_H__
#define __SEMAPHORE_H__

/* SEMAPHORES FOR THREAD SYNC
*	- 
*/

typedef struct semaphore_t semaphore_t;

// Creates a semaphore.
// 
// RETURN: the new semaphore
semaphore_t* semaphoreCreate(int init_v, int max_v);

// Destorys the semaphore.
//
void semaphoreDestroy(semaphore_t* semaphore);

// Get the block required for the semaphore
//
void semaphoreGet(semaphore_t* semaphore);

// Releases the semaphore 
//
void semaphoreRelease(semaphore_t* semaphore);

#endif
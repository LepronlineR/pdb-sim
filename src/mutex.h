#ifndef __MUTEX_H__
#define __MUTEX_H__

/* MUTEX FOR THREAD SYNC
*
*/

// Handle to a mutex.
typedef struct mutex_t mutex_t;

// Creates a new mutex.
mutex_t* mutexCreate();

// Destroys a previously created mutex.
void mutexDestroy(mutex_t* mutex);

// Locks a mutex. May block if another thread unlocks it.
// If a thread locks a mutex multiple times, it must be unlocked
// multiple times.
//
void mutexLock(mutex_t* mutex);

// Unlocks a mutex.
//
void mutexUnlock(mutex_t* mutex);

#endif
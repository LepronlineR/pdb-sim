#include "mutex.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

mutex_t* mutexCreate(){
	return (mutex_t*) CreateMutexW(NULL, FALSE, NULL);
}

void mutexDestroy(mutex_t* mutex){
	CloseHandle(mutex);
}

void mutexLock(mutex_t* mutex){
	WaitForSingleObject(mutex, INFINITE);
}

void mutexUnlock(mutex_t* mutex){
	ReleaseMutex(mutex);
}
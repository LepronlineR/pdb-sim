#include "semaphore.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

semaphore_t* semaphoreCreate(int init_v, int max_v)
{
	HANDLE semaphore = CreateSemaphoreW(NULL, init_v, max_v, NULL);
	return (semaphore_t*) semaphore;
}

void semaphoreDestroy(semaphore_t* semaphore){
	CloseHandle(semaphore);
}

void semaphoreGet(semaphore_t* semaphore){
	WaitForSingleObject(semaphore, INFINITE);
}

void semaphoreRelease(semaphore_t* semaphore){
	ReleaseSemaphore(semaphore, 1, NULL);
}

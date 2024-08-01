#include "thread.h"

#include "debug.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

thread_t* threadCreate(double (*function)(void*), void* data)
{

	HANDLE thread = CreateThread(NULL, 0, function, data, CREATE_SUSPENDED, NULL);
	if (thread == INVALID_HANDLE_VALUE){
		debugPrint(DEBUG_PRINT_ERROR, "Thread Create: failed to create thread.\n");
		return NULL;
	}
	ResumeThread(thread);
	return (thread_t*) thread;
}

int	threadRun(thread_t* thread) {
	WaitForSingleObject(thread, INFINITE);
	int code = 0;
	GetExitCodeThread(thread, &code);
	CloseHandle(thread);
	return code;
}

void threadSleep(uint32_t ms){
	Sleep(ms);
}

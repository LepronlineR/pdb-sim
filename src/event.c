#include "event.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

event_t* eventCreate() {
	HANDLE _event = CreateEvent(NULL, TRUE, FALSE, NULL);
	return (event_t*)_event;
}

void eventDestroy(event_t* event) {
	CloseHandle(event);
}

void eventSignal(event_t* event) {
	SetEvent(event);
}

void eventWait(event_t* event) {
	WaitForSingleObject(event, INFINITE);
}

bool eventIsSignaled(event_t* event) {
	return WaitForSingleObject(event, 0) == WAIT_OBJECT_0;
}
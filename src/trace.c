#include "trace.h"

#include "heap.h"
#include "timer.h"
#include "timer_object.h"
#include "deque.h"
#include "debug.h"
#include "mutex.h"

#include <stddef.h>
#include <stdbool.h>
#include <process.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct trace_event_t {
	const char* name;
	int pid;
	DWORD tid;
	int ts;
	char event_type;
} trace_event_t;

typedef struct trace_t {
	bool started;
	const char* path;
	uint16_t print_counter;
	heap_t* heap;
	deque_t* trace_event_queue;
	deque_t* trace_event_print;
	mutex_t* mutex;
} trace_t;

trace_t* traceCreate(heap_t* heap, int event_capacity) {
	trace_t* trace = heapAlloc(heap, sizeof(trace_t), 8);
	trace->heap = heap;
	trace->started = false;
	trace->trace_event_queue = dequeCreate(heap, event_capacity);
	trace->trace_event_print = dequeCreate(heap, event_capacity * 2);
	trace->mutex = mutexCreate();
	trace->print_counter = 0;
	return trace;
}

trace_event_t* traceEventCopy(trace_t* trace, trace_event_t* event, char event_type) {
	trace_event_t* n_event = heapAlloc(trace->heap, sizeof(trace_event_t), 8);
	n_event->event_type = event_type;
	n_event->name = event->name;
	n_event->pid = event->pid;
	n_event->tid = GetCurrentThreadId();
	n_event->ts = timerTicksToMs(timerGetTicks());

	return n_event;
}

void traceDestroy(trace_t* trace) {
	dequeDestroy(trace->trace_event_queue);
	dequeDestroy(trace->trace_event_print);
	mutexDestroy(trace->mutex);
	heapFree(trace->heap, trace);
}

void traceDurationPush(trace_t* trace, const char* name) {
	if (trace == NULL || !trace->started) // trace has not started or null
		return;

	mutexLock(trace->mutex);

	// create a new trace event that begins with the current name
	trace_event_t* trace_event = heapAlloc(trace->heap, sizeof(trace_event_t), 8);
	trace_event->name = name;
	trace_event->pid = _getpid();
	trace_event->event_type = 'B';
	trace_event->tid = GetCurrentThreadId();
	trace_event->ts = timerTicksToMs(timerGetTicks());

	// push trace into the queue and print
	dequePushBack(trace->trace_event_queue, trace_event);
	dequePushBack(trace->trace_event_print, trace_event);

	mutexUnlock(trace->mutex);
}

trace_event_t* traceDurationPop(trace_t* trace) {
	if (trace == NULL || !trace->started) // trace has not started
		return (trace_event_t*) 0;

	mutexLock(trace->mutex);

	trace_event_t* item = dequePopFront(trace->trace_event_queue);
	// copy this item and add it to the printing queue
	dequePushBack(trace->trace_event_print, traceEventCopy(trace, item, 'E'));

	mutexUnlock(trace->mutex);

	return item;
}

void traceCaptureStart(trace_t* trace, const char* path) {
	trace->started = true;
	trace->path = path;
}

void traceCaptureStop(trace_t* trace) {

	mutexLock(trace->mutex);

	// =======================================================================================
	//									   WRITE TO JSON
	// =======================================================================================

	// write to JSON format for each saved trace event in the trace event list starting from head
	wchar_t w_path[1024] = { 0 };
	if (MultiByteToWideChar(CP_UTF8, 0, trace->path, -1, w_path, 0) <= 0) {
		debugPrint(DEBUG_PRINT_ERROR, "In 'trace_capture_stop' creating wide_path is invalid.\n");
		return;
	}

	HANDLE handle = CreateFile(w_path, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (handle == INVALID_HANDLE_VALUE) {
		debugPrint(DEBUG_PRINT_ERROR, "In 'trace_capture_stop' creating the handle is invalid.\n");
		return;
	}

	// write the first starting two lines to the file
	char start_of_json_lines[47] = "{\n\t\"displayTimeUnit\": \"ns\", \"traceEvents\": [\n";

	if (!WriteFile(handle, (LPVOID)start_of_json_lines, strlen(start_of_json_lines), NULL, NULL)) {
		debugPrint(DEBUG_PRINT_ERROR, "In 'trace_capture_stop' unable to write to json file.\n");
		CloseHandle(handle);
		return;
	}

	// clean most of the trace
	trace->started = false;
	trace_event_t* trace_event = dequePopFront(trace->trace_event_print);

	while (trace_event != 0) {
		char event_str[2048];
		if (dequeGetCurrentSize(trace->trace_event_print) > 0) {
			// events from start to last - 1
			snprintf(event_str, sizeof(event_str),
				"\t\t{\"name\":\"%s\",\"ph\":\"%c\",\"pid\":%d,\"tid\":\"%lu\",\"ts\":\"%d\"},\n",
				trace_event->name, trace_event->event_type, trace_event->pid, trace_event->tid, trace_event->ts);
		} else {
			// last event remove the , from the event str
			snprintf(event_str, sizeof(event_str),
				"\t\t{\"name\":\"%s\",\"ph\":\"%c\",\"pid\":%d,\"tid\":\"%lu\",\"ts\":\"%d\"}\n",
				trace_event->name, trace_event->event_type, trace_event->pid, trace_event->tid, trace_event->ts);
		}

		if (!WriteFile(handle, (LPVOID)event_str, strlen(event_str), NULL, NULL)) {
			debugPrint(DEBUG_PRINT_ERROR, "In 'trace_capture_stop' unable to write to json file.\n");
			CloseHandle(handle);
			return;
		}

		heapFree(trace->heap, trace_event);

		if (dequeGetCurrentSize(trace->trace_event_print) == 0)
			break;

		trace_event = dequePopFront(trace->trace_event_print);
	}

	// write the last two lines
	char end_of_json_lines[6] = "\t]\n}";

	if (!WriteFile(handle, (LPVOID)end_of_json_lines, strlen(end_of_json_lines), NULL, NULL)) {
		debugPrint(DEBUG_PRINT_ERROR, "In 'trace_capture_stop' unable to write to json file.\n");
		CloseHandle(handle);
		return;
	}

	mutexUnlock(trace->mutex);

	CloseHandle(handle);

}

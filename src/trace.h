#ifndef __TRACE_H__
#define __TRACE_H__

#include "heap.h"

/* A trace, defines a trace structure that can be use to trace processes

A trace contains a path to the file, a heap, a definition for when the trace process begins, and a mutex
- the first trace event element to add every recorded trace event
- a trace event queue to push and pop elements
*/
typedef struct trace_t trace_t;

/* A trace event, in a single list format :

A trace event contains:
- name of function
- process ID
- thread ID
- time
- event type (supports [B: Begin] and [E: End] for now)
*/
typedef struct trace_event_t trace_event_t;

// Creates a CPU performance tracing system.
// Event capacity is the maximum number of durations that can be traced.
// 
// RETURN: a new trace object
trace_t* traceCreate(heap_t* heap, int event_capacity);

// Copies and returns a new allocated trace event
//
// RETURN: the copy
trace_event_t* traceEventCopy(trace_t* trace, trace_event_t* event, char event_type);

// Destroys a CPU performance tracing system.
//
void traceDestroy(trace_t* trace);

// Begin tracing a named duration on the current thread.
// It is okay to nest multiple durations at once.
// We push the function name into a trace queue, and append it to an event list
//
void traceDurationPush(trace_t* trace, const char* name);

// End tracing the currently active duration on the current thread and append
// it to the event list
//
trace_event_t* traceDurationPop(trace_t* trace);

// Start recording trace events.
// A Chrome trace file will be written to path.
//
void traceCaptureStart(trace_t* trace, const char* path);

// Stop recording trace events and write the saved trace events to the path.
//
void traceCaptureStop(trace_t* trace);

#endif
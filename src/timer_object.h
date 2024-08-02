#ifndef __TIMER_OBJECT_H__
#define __TIMER_OBJECT_H__

#include "heap.h"
#include <stdint.h>

/* TIMER OBJECT
*  - used to track time of the system it can:
*	  - pause/resume time
*     - scale time (slowing, speeding time)
*     - inheritance (child inherits parent's base time)
*/

// Handle to a time object.
typedef struct timer_object_t timer_object_t;

// Create a new time object with the defined parent.
// If parent is NULL, use system timer as base time.


// Creates a new time object. If there is a parent, then this timer will be a child
// under the parent. Otherwise this timer will start as a base time object (root).
//
// RETURN: new timer object
timer_object_t* timerObjectCreate(heap_t* heap, timer_object_t* parent);

// Destroy previously created time object.
//
void timerObjectDestroy(timer_object_t* t);

// Per-frame update for time object.
// Updates current time and delta time.
// 
void timerObjectUpdate(timer_object_t* t);

// Get current time in microseconds.
//
// RETURN: us
uint64_t timerObjectGetUs(timer_object_t* t);

// Get current time in milliseconds.
//
// RETURN: ms
uint32_t timerObjectGetMs(timer_object_t* t);

// Get frame time in microseconds.
//
// RETURN: us (in deltatime)
uint64_t timerObjectGetUsDeltaTime(timer_object_t* t);

// Get frame time in milliseconds.
//
// RETURN: ms (in deltatime)
uint32_t timerObjectGetMsDeltaTime(timer_object_t* t);

// Set time scale value.
// Smaller values slow down, larger values speed up.
// A value of 1.0 is normal speed.
//
void timerObjectSetScale(timer_object_t* t, float s);

// Pause time on the object.
// 
void timerObjectPause(timer_object_t* t);

// Resume time on the object.
// 
void timerObjectResume(timer_object_t* t);

#endif
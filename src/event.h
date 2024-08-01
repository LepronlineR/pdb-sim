#ifndef __EVENT_H__
#define __EVENT_H__

#include <stdbool.h>

/* EVENT THREAD SYNC
*  - syncs events for multithreading 
*	 (a use all for semaphores)
*/

typedef struct event_t event_t; 

// Creates a new event
//
// RETURN: new event
event_t* eventCreate();

// Destroy the event
//
void eventDestroy(event_t* event);

// Set an event
//
void eventSignal(event_t* event);

// Wait for the event to be signaled
//
void eventWait(event_t* event);

// Look
//
// RETURN: true if the event is signaled, false otherwise
bool eventIsSignaled(event_t* event);


#endif
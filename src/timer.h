#ifndef __TIMER_H__
#define __TIMER_H__

// High resolution timer support.

#include <stdint.h>

// Start the timer (ONLY NEEDS TO BE INITIALIZED ONCE)
// 
void timerStartup();

// Get the number of OS-defined ticks that have elapsed since startup.
//
// RETURN: total ticks since the timer startup
uint64_t timerGetTicks();

// Get the OS-defined tick frequency.
//
// RETURN: frequency of the tick
uint64_t timerGetTicksPerSecond();

// Convert a number of OS-defined ticks to microseconds.
//
// RETURN: ticks in us
uint64_t timerTicksToUs(uint64_t t);

// Convert a number of OS-defined ticks to milliseconds.
//
// RETURN: ticks in ms
uint32_t timerTicksToMs(uint64_t t);

#endif
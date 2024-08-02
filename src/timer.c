#include "timer.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static uint64_t s_ticks_start = 0;
static double s_us_per_tick = 0.001;
static double s_ms_per_tick = 0.000001;

void timerStartup(){
	s_ticks_start = timerGetTicks();
	uint64_t ticks_per_second = timerGetTicksPerSecond();
	s_us_per_tick = 1000000.0 / ticks_per_second;
	s_ms_per_tick = 1000.0 / ticks_per_second;
}

uint64_t timerTicksToUs(uint64_t t){
	return (uint64_t)((double)t * s_us_per_tick);
}

uint32_t timerTicksToMs(uint64_t t){
	return (uint32_t)((double)t * s_ms_per_tick);
}

uint64_t timerGetTicks(){
	LARGE_INTEGER now;
	QueryPerformanceCounter(&now);
	return now.QuadPart - s_ticks_start;
}

uint64_t timerGetTicksPerSecond(){
	LARGE_INTEGER freq;
	QueryPerformanceFrequency(&freq);
	return freq.QuadPart;
}

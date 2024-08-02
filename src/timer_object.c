#include "timer_object.h"
#include "timer.h"
#include <stdbool.h>

typedef struct timer_object_t
{
	heap_t* heap;
	uint64_t current_ticks;
	uint64_t delta_ticks;
	timer_object_t* parent;
	uint64_t bias_ticks;
	double scale;
	bool paused;
} timer_object_t;

timer_object_t* timerObjectCreate(heap_t* heap, timer_object_t* parent){
	timer_object_t* t = heapAlloc(heap, sizeof(timer_object_t), 8);
	t->heap = heap;
	t->current_ticks = 0;
	t->delta_ticks = 0;
	t->parent = parent;
	t->bias_ticks = parent ? parent->current_ticks : timer_get_ticks();
	t->scale = 1.0;
	t->paused = false;
	return t;
}

void timerObjectDestroy(timer_object_t* t){
	heapFree(t->heap, t);
}

void timerObjectUpdate(timer_object_t* t){
	if (!t->paused){
		uint64_t parent_ticks = t->parent ? t->parent->current_ticks : timerGetTicks();
		t->delta_ticks = (uint64_t)((parent_ticks - t->bias_ticks) * t->scale);
		t->current_ticks += t->delta_ticks;
		t->bias_ticks = parent_ticks;
	}
}

uint64_t timerObjectGetUs(timer_object_t* t){
	return timerTicksToUs(t->current_ticks);
}

uint32_t timerObjectGetMs(timer_object_t* t){
	return timerTicksToMs(t->current_ticks);
}

uint64_t timerObjectGetUsDeltaTime(timer_object_t* t){
	return timerTicksToUs(t->delta_ticks);
}

uint32_t timerObjectGetMsDeltaTime(timer_object_t* t){
	return timerTicksToMs(t->delta_ticks);
}

void timerObjectSetScale(timer_object_t* t, float s){
	t->scale = s;
}

void timerObjectPause(timer_object_t* t){
	t->paused = true;
}

void timerObjectResume(timer_object_t* t){
	if (t->paused){
		t->bias_ticks = t->parent ? t->parent->current_ticks : timerGetTicks();
		t->paused = false;
	}
}

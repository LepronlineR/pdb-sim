#include "atomic.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

int atomicInc(int* address){ 
	return InterlockedIncrement(address) - 1;
}

int atomicDec(int* address){
	return InterlockedDecrement(address) + 1;
}

int atomicCompareAssign(int* address, int value, int new_value){
	// InterlockedCompareExchange(destination, exchange, comparand)
	return InterlockedCompareExchange(address, new_value, value);
}

int atomicRead(int* address){
	return *(volatile int*) address;
}

void atomicWrite(int* address, int value){
	*(volatile int*) address = value;
}

#ifndef __ATOMIC_H__
#define __ATOMIC_H__

/* ATOMIC INSTRUCTIONS USED FOR MULTITHREADING
*	- supports uint32
*/

// Increment the value from the address.
// 
// RETURN: the old value of the address before the increment operation
int atomicInc(int* address);

// Decrement the value from the address.
// 
// RETURN: the old value of the address before the decrement operation
int atomicDec(int* address);

// Compare the value of the address to value, then changes the address
// value to new_value
// 
// RETURN: the old value from the address before it got overwritten
int atomicCompareAssign(int* address, int value, int new_value);

// Reads an integer from an address.
// All writes that occurred before the last atomic_store to this address are flushed.

// Reads an int from the address.
// 
// RETURN: value from address
int atomicRead(int* address);

// Writes an int (value) to the address.
//
void atomicWrite(int* address, int value);

#endif
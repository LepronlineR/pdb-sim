#ifndef __DEQUE_H__
#define __DEQUE_H__

/*	A DEQUE DATA STRUCTURE
*	- You can insert elements at the front and back of a deque as
*	  well as remove elements at the front and back. 
*     (both operations are O(1))
*	- the capacity is static
*	- thread safe
*/

#define INITIAL_DEQUE_CAPACITY 16

typedef struct deque_t deque_t;
typedef struct heap_t heap_t;

// Create the deque
// 
deque_t* dequeCreate(heap_t* heap, int length);

// Destroy the deque
//
void dequeDestroy(deque_t* deque);

// Push an item to the front
//
void dequePushFront(deque_t* deque, void* item);

// Pop the element at the front
//
// RETURN: item that has been popped
void* dequePopFront(deque_t* deque);

// Push an item to the back
//
void dequePushBack(deque_t* deque, void* item);

// Pop the element at the back
//
// RETURN: item that has been popped
void* dequePopBack(deque_t* deque);

#endif
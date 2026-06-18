#include "hashtable.h"
#include "heap.h"
#include "semaphore.h"

typedef struct ht_item_t {
	const void* key;
	void* value;
} ht_item_t;

typedef struct hash_table_t {
	heap_t* heap;
	ht_item_t* items;

	semaphore_t* used;
	semaphore_t* free;

	size_t size;
	size_t capacity;
} hash_table_t;

unsigned int DJBHash(const char* str, unsigned int length);

hash_table_t* hashTableCreate(heap_t* heap, int capacity) {
	hash_table_t* ht = heapAlloc(heap, sizeof(hash_table_t), 8);
	ht->capacity = capacity;
	ht->size = 0;
	ht->items = heapAlloc(heap, sizeof(ht_item_t*) * capacity, 8);
	ht->heap = heap;
	ht->used = semaphoreCreate(0, capacity);
	return ht;
}

void hashTableDestroy(hash_table_t* ht) {

}


/*
	DJB Hash Function

	An algorithm produced by Professor Daniel J. Bernstein and shown 
	first to the world on the usenet newsgroup comp.lang.c. It is one 
	of the most efficient hash functions ever published.
*/
unsigned int DJBHash(const char* str, unsigned int length) {
	unsigned int hash = 5381;
	unsigned int i = 0;

	for (i = 0; i < length; ++str, ++i) {
		hash = ((hash << 5) + hash) + (*str);
	}

	return hash;
}
#ifndef __HASHTABLE_H__
#define __HASHTABLE_H__

#include <stdint.h>

/* 
* 
* 
* 
* 
* FROM: https://www.partow.net/programming/hashfunctions/#top
*/

#define INITIAL_HASH_TABLE_CAPACITY 128

typedef struct hash_table_t hash_table_t;

typedef struct ht_item_t ht_item_t;
typedef struct heap_t heap_t;

hash_table_t* hashTableCreate(heap_t* heap, int capacity);

void hashTableDestroy(hash_table_t* ht);

void* hashTableGet(hash_table_t* ht, void* key);



#endif

#ifndef __HASHTABLE_H__
#define __HASHTABLE_H__

#include <stdbool.h>
#include <stdint.h>

/*		A HASH TABLE
*	- Maps 64-bit integer keys to 64-bit values (ids, packed pairs, pointers).
*	- Open addressing with linear probing over power-of-two arrays: a lookup is
*	  a hash and, usually, one or two adjacent slots, with no per-item allocation.
*	- Grows (doubling) to stay at most half full; removal shifts later entries
*	  back, so there are no tombstones and lookups stay short.
*	- Not thread safe.
*	- HASH_TABLE_EMPTY_KEY is reserved and cannot be stored.
*/

#define HASH_TABLE_EMPTY_KEY UINT64_MAX

typedef struct hash_table_t hash_table_t;
typedef struct heap_t heap_t;

// Create a table that holds capacity items before it has to grow.
//
hash_table_t* hashTableCreate(heap_t* heap, int capacity);

void hashTableDestroy(hash_table_t* table);

// Insert key, or replace its value if it is already present.
//
void hashTableSet(hash_table_t* table, uint64_t key, uint64_t value);

// RETURN: true and the key's value in *value (when value is not NULL), or false
// if the key is not present
bool hashTableGet(const hash_table_t* table, uint64_t key, uint64_t* value);

// RETURN: whether the key is present
bool hashTableContains(const hash_table_t* table, uint64_t key);

// RETURN: true if the key was present and has been removed
bool hashTableRemove(hash_table_t* table, uint64_t key);

// Remove every item, keeping the allocated capacity.
//
void hashTableClear(hash_table_t* table);

// RETURN: number of items in the table
int hashTableCount(const hash_table_t* table);

// A key for an unordered pair of 32-bit ids: (a, b) and (b, a) give the same key.
//
uint64_t hashTablePairKey(uint32_t a, uint32_t b);

#endif

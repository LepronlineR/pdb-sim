#include "hashtable.h"
#include "heap.h"

#include <assert.h>
#include <string.h>

#define HASH_TABLE_MIN_SLOTS 16

typedef struct hash_table_t {
	heap_t* heap;
	/* Keys and values in separate arrays, so a probe walks keys only. */
	uint64_t* keys;
	uint64_t* values;
	uint32_t mask;   // slot count - 1 (slot count is a power of two)
	int count;
} hash_table_t;

/* splitmix64's finalizer: sequential ids and packed pairs spread evenly over
   the low bits the mask keeps. */
static uint64_t hashTableHash(uint64_t key) {
	key ^= key >> 30;
	key *= 0xbf58476d1ce4e5b9ull;
	key ^= key >> 27;
	key *= 0x94d049bb133111ebull;
	key ^= key >> 31;
	return key;
}

static void hashTableAllocate(hash_table_t* table, uint32_t slots) {
	table->keys = heapAlloc(table->heap, sizeof(uint64_t) * slots, 8);
	table->values = heapAlloc(table->heap, sizeof(uint64_t) * slots, 8);
	memset(table->keys, 0xff, sizeof(uint64_t) * slots);  // every slot HASH_TABLE_EMPTY_KEY
	table->mask = slots - 1;
	table->count = 0;
}

hash_table_t* hashTableCreate(heap_t* heap, int capacity) {
	hash_table_t* table = heapAlloc(heap, sizeof(hash_table_t), 8);
	table->heap = heap;
	uint32_t slots = HASH_TABLE_MIN_SLOTS;
	while (slots < (uint32_t)capacity * 2) slots <<= 1;
	hashTableAllocate(table, slots);
	return table;
}

void hashTableDestroy(hash_table_t* table) {
	if (!table) return;
	heapFree(table->heap, table->values);
	heapFree(table->heap, table->keys);
	heapFree(table->heap, table);
}

/* RETURN: the key's slot, or the empty slot where it would go */
static uint32_t hashTableFind(const hash_table_t* table, uint64_t key) {
	uint32_t slot = (uint32_t)hashTableHash(key) & table->mask;
	while (table->keys[slot] != key && table->keys[slot] != HASH_TABLE_EMPTY_KEY) {
		slot = (slot + 1) & table->mask;
	}
	return slot;
}

static void hashTableGrow(hash_table_t* table) {
	uint64_t* keys = table->keys;
	uint64_t* values = table->values;
	const uint32_t old_slots = table->mask + 1;
	hashTableAllocate(table, old_slots * 2);
	for (uint32_t i = 0; i < old_slots; ++i) {
		if (keys[i] != HASH_TABLE_EMPTY_KEY) {
			const uint32_t slot = hashTableFind(table, keys[i]);
			table->keys[slot] = keys[i];
			table->values[slot] = values[i];
			table->count++;
		}
	}
	heapFree(table->heap, values);
	heapFree(table->heap, keys);
}

void hashTableSet(hash_table_t* table, uint64_t key, uint64_t value) {
	assert(key != HASH_TABLE_EMPTY_KEY);
	uint32_t slot = hashTableFind(table, key);
	if (table->keys[slot] == HASH_TABLE_EMPTY_KEY) {
		/* At most half full, so probes stay short and always end at an empty slot. */
		if ((uint32_t)(table->count + 1) * 2 > table->mask + 1) {
			hashTableGrow(table);
			slot = hashTableFind(table, key);
		}
		table->keys[slot] = key;
		table->count++;
	}
	table->values[slot] = value;
}

bool hashTableGet(const hash_table_t* table, uint64_t key, uint64_t* value) {
	if (key == HASH_TABLE_EMPTY_KEY) return false;
	const uint32_t slot = hashTableFind(table, key);
	if (table->keys[slot] == HASH_TABLE_EMPTY_KEY) return false;
	if (value) *value = table->values[slot];
	return true;
}

bool hashTableContains(const hash_table_t* table, uint64_t key) {
	return hashTableGet(table, key, NULL);
}

bool hashTableRemove(hash_table_t* table, uint64_t key) {
	if (key == HASH_TABLE_EMPTY_KEY) return false;
	uint32_t hole = hashTableFind(table, key);
	if (table->keys[hole] == HASH_TABLE_EMPTY_KEY) return false;
	/* Backward-shift deletion: move each following entry of the run into the
	   hole unless its home slot lies cyclically after the hole (it would then
	   become unreachable). */
	uint32_t slot = hole;
	for (;;) {
		slot = (slot + 1) & table->mask;
		const uint64_t moving = table->keys[slot];
		if (moving == HASH_TABLE_EMPTY_KEY) break;
		const uint32_t home = (uint32_t)hashTableHash(moving) & table->mask;
		if (((slot - home) & table->mask) >= ((slot - hole) & table->mask)) {
			table->keys[hole] = moving;
			table->values[hole] = table->values[slot];
			hole = slot;
		}
	}
	table->keys[hole] = HASH_TABLE_EMPTY_KEY;
	table->count--;
	return true;
}

void hashTableClear(hash_table_t* table) {
	memset(table->keys, 0xff, sizeof(uint64_t) * (table->mask + 1));
	table->count = 0;
}

int hashTableCount(const hash_table_t* table) {
	return table->count;
}

uint64_t hashTablePairKey(uint32_t a, uint32_t b) {
	return a < b ? ((uint64_t)a << 32) | b : ((uint64_t)b << 32) | a;
}

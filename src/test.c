#include "test.h"

#include "atomic.h"
#include "debug.h"
#include "event.h"
#include "mutex.h"
#include "semaphore.h"
#include "thread.h"
#include "heap.h"
#include "fs.h"

#include <assert.h>
#include <stdbool.h>

#include <windows.h>
#include <mmsystem.h>
#include <stdio.h>
#pragma comment(lib, "winmm.lib")
#include <Shlwapi.h> // Include for PathFileExists
#pragma comment(lib, "Shlwapi.lib")

#define LARGENUMBER 1000000

// ================================================
//					TRACING TEST
// ================================================

void testTraceSlowerFunction(trace_t* trace) {
	traceDurationPush(trace, "slower_function");
	threadSleep(500);
	traceDurationPop(trace);
}

void testTraceSlowFunction(trace_t* trace) {
	traceDurationPush(trace, "slow_function");
	threadSleep(200);
	testTraceSlowerFunction(trace);
	traceDurationPop(trace);
}

int testTraceFunc(void* data) {
	trace_t* trace = data;
	testTraceSlowFunction(trace);
	return 0;
}

void testTrace() {
	heap_t* heap = heapCreate(4096);

	// Create the tracing system with at least space for 100 *captured* events.
	// Each call to trace_duration_push is an event.
	// Each call to trace_duration_pop is an event.
	// Before trace_capture_start is called, and after trace_capture_stop is called,
	// duration events should not be generated.
	trace_t* trace = traceCreate(heap, 100);

	// Capturing has *not* started so these calls can safely be ignored.
	traceDurationPush(trace, "should be ignored");
	traceDurationPop(trace);

	// Start capturing events.
	// Eventually we will want to write events to a file -- "trace.json".
	// However we should *not* write to the file for each call to trace_duration_push or
	// trace_duration_pop. That would be much too slow. Instead, events should be buffered
	// (up to event_capacity) before writing to a file. For purposes of this homework,
	// it is entirely fine if you only capture the first event_capacity count events and
	// ignore any additional events.
	traceCaptureStart(trace, "trace.json");

	// Create a thread that will push/pop duration events.
	thread_t* thread = threadCreate(testTraceFunc, trace);

	// Call a function that will push/pop duration events.
	testTraceSlowFunction(trace);

	// Wait for thread to finish.
	threadDestroy(thread);

	// Finish capturing. Write the trace.json file in Chrome tracing format.
	traceCaptureStop(trace);

	traceDestroy(trace);

	heapDestroy(heap);
}

// ================================================
//					FILE I/O TEST
// ================================================

void testReadWriteAndCompression(heap_t* heap, fs_t* fs) {

	// =============== READ FILE (HARRY POTTER BOOK 1) ================

	fs_work_t* read_file_work = fsRead(fs, "assets\\fiotest.test", heap, true, false);
	fsWorkBlock(read_file_work);

	char* harry_potter = fsWorkGetBuffer(read_file_work);
	const size_t harry_potter_len = strlen(harry_potter);


	// =================== COMPRESS AND WRITE FILE ====================

	const char* write_data = harry_potter;
	fs_work_t* write_work = fsWrite(fs, "assets\\compressed.bar", write_data, harry_potter_len, true);
	fsWorkBlock(write_work);

	assert(fsWorkGetErrorCode(write_work) == 0);
	assert(fsWorkGetSize(write_work) == harry_potter_len);

	// ===================== READ COMPRESSED FILE =====================

	fs_work_t* read_work = fsRead(fs, "assets\\compressed.bar", heap, true, true);
	fsWorkBlock(read_work);

	// ===================== COMPARE TO PREV DATA =====================

	char* read_data = fsWorkGetBuffer(read_work);
	assert(read_data && strcmp(read_data, harry_potter) == 0);
	assert(fsWorkGetErrorCode(read_work) == 0);
	assert(fsWorkGetSize(read_work) == harry_potter_len);

	fsWorkDestroy(read_file_work);
	fsWorkDestroy(read_work);
	fsWorkDestroy(write_work);

	debugPrint(DEBUG_PRINT_INFO, "File I/O Test Success!\n");
}

// ================================================
//					ALLOCATION TEST
// ================================================
void testLeakedHeapAllocation() {
	heap_t* heap = heapCreate(4096);
	void* block = heapAlloc(heap, 16 * 1024, 8);
	/* leaked memory */ heapAlloc(heap, 256, 8);
	/* leaked memory */ heapAlloc(heap, 16 * 1024, 8);
	heapFree(heap, block);
	heapDestroy(heap);

	// assert that the backtrace returns 
	// assert(!debugBacktraceManually());
}

// ================================================
//					THREADING TEST
// ================================================
typedef struct thread_info_t {
	int* count;
	mutex_t* mutex;
	event_t* _event;
} thread_info_t;

int noSynchronizationTestFunc(void* user) {
	thread_info_t* thread_data = (thread_info_t*) user;
	eventWait(thread_data->_event);

	DWORD start = timeGetTime();

	for (int x = 0; x < LARGENUMBER; x++){
		*thread_data->count = *thread_data->count + 1;
	}

	return timeGetTime() - start;
}

int atomicReadWriteTestFunc(void* user){
	thread_info_t* thread_data = (thread_info_t*)user;
	eventWait(thread_data->_event);

	DWORD start = timeGetTime();

	for (int x = 0; x < LARGENUMBER; x++){
		atomicWrite(thread_data->count, atomicRead(thread_data->count) + 1);
	}

	return timeGetTime() - start;
}

int atomicIncrementTestFunc(void* user){
	thread_info_t* thread_data = (thread_info_t*)user;
	eventWait(thread_data->_event);

	DWORD start = timeGetTime();

	for (int x = 0; x < LARGENUMBER; x++){
		atomicInc(thread_data->count);
	}

	return timeGetTime() - start;
}

int mutexTestFunc(void* user){
	thread_info_t* thread_data = (thread_info_t*)user;
	eventWait(thread_data->_event);

	DWORD start = timeGetTime();

	for (int x = 0; x < LARGENUMBER; x++){
		mutexLock(thread_data->mutex);
		*thread_data->count = *thread_data->count + 1;
		mutexUnlock(thread_data->mutex);
	}

	return timeGetTime() - start;
}

void runThreadBenchmark(int(*function)(void*), const char* name) {
	int count = 0;
	thread_info_t info = {
		.count = &count,
		.mutex = mutexCreate(),
		._event = eventCreate()
	};

	thread_t* threads[8];
	for (int x = 0; x < _countof(threads); x++) {
		threads[x] = threadCreate(function, &info);
	}

	// start
	eventSignal(info._event);

	// Wait for threads
	int duration = 0;
	for (int x = 0; x < _countof(threads); x++) {
		duration += threadRun(threads[x]);
	}

	mutexDestroy(info.mutex);
	eventDestroy(info._event);

	debugPrintConsole(
		"[ %s ] ---- Elapsed: %3.3f s ---- Count: %d\n", 
		name, duration / 1000.0, count);
}


/*
[ noSyncTest ] ---- Elapsed: 0.374 s ---- Count: 1434803
[ atomicRW ] ---- Elapsed: 0.569 s ---- Count: 1284107
[ atomicInc ] ---- Elapsed: 1.102 s ---- Count: 8000000
[ mutex ] ---- Elapsed: 386.389 s ---- Count: 8000000
*/
void testThreading() {
	runThreadBenchmark(noSynchronizationTestFunc, "noSyncTest");
	runThreadBenchmark(atomicReadWriteTestFunc, "atomicRW");
	runThreadBenchmark(atomicIncrementTestFunc, "atomicInc");
	runThreadBenchmark(mutexTestFunc, "mutex");
}


// ================================================
//					PHYSICS TESTS
// ================================================

#include "ball.h"
#include "cloth.h"
#include "physics.h"
#include <float.h>
#include <math.h>
#include <string.h>

typedef struct physics_test_scene_t {
	heap_t* heap;
	physics_t* physics;
	physics_particle_id_t first[8];
	int count[8];
	int body_count;
} physics_test_scene_t;

static int test_failures;

static void testCheck(bool passed, const char* name, const char* detail) {
	debugPrintConsole("[ %s ] %s -- %s\n", passed ? "PASS" : "FAIL", name, detail);
	if (!passed) ++test_failures;
}

static void testSceneBegin(physics_test_scene_t* scene) {
	scene->first[scene->body_count] =
		(physics_particle_id_t)physicsGetStats(scene->physics).particle_count;
}

static void testSceneEnd(physics_test_scene_t* scene) {
	scene->count[scene->body_count] = physicsGetStats(scene->physics).particle_count -
		(int)scene->first[scene->body_count];
	scene->body_count++;
}

static void testSceneAddBall(physics_test_scene_t* scene, vec3f_t position, float mass, float radius) {
	ball_info_t info = ballInfoDefault(position, vec3fOne(), mass);
	info.radius = radius;
	testSceneBegin(scene);
	ballCreate(scene->heap, scene->physics, &info);
	testSceneEnd(scene);
}

static void testSceneAddCloth(physics_test_scene_t* scene, cloth_info_t info) {
	testSceneBegin(scene);
	clothCreate(scene->heap, scene->physics, &info);
	testSceneEnd(scene);
}

static vec3f_t testCentroid(physics_test_scene_t* scene, int body) {
	vec3f_t sum = vec3fZero();
	for (int i = 0; i < scene->count[body]; ++i) {
		sum = vec3fAdd(sum, physicsGetParticlePosition(scene->physics, scene->first[body] + i));
	}
	return vec3fScale(sum, 1.0f / (float)scene->count[body]);
}

static vec3f_t testAverageVelocity(physics_test_scene_t* scene, int body) {
	vec3f_t sum = vec3fZero();
	for (int i = 0; i < scene->count[body]; ++i) {
		sum = vec3fAdd(sum, physicsGetParticleVelocity(scene->physics, scene->first[body] + i));
	}
	return vec3fScale(sum, 1.0f / (float)scene->count[body]);
}

static void testSceneRun(physics_test_scene_t* scene, int frames) {
	for (int f = 0; f < frames; ++f) physicsUpdate(scene->physics, 1.0f / 60.0f);
}

/* Balls dropped in a pile: no NaNs, nothing under the ground, bounded overlap
   between bodies, and the same result on every run. */
static void testPhysicsPile(heap_t* heap) {
	vec3f_t final_positions[2][4];
	for (int run = 0; run < 2; ++run) {
		physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
		for (int i = 0; i < 8; ++i) {
			testSceneAddBall(&scene, (vec3f_t){ (float)(i % 4) * 1.5f, 3.0f + (float)(i / 4) * 2.5f,
				(float)(i % 3) * 0.7f }, 1.0f, 1.0f);
		}
		testSceneRun(&scene, 360);
		int nan = 0;
		float lowest = FLT_MAX;
		float worst_overlap = 0.0f;
		for (int a = 0; a < scene.body_count; ++a) {
			for (int i = 0; i < scene.count[a]; ++i) {
				const physics_particle_id_t pa = scene.first[a] + i;
				const vec3f_t p = physicsGetParticlePosition(scene.physics, pa);
				if (isnan(p.x) || isnan(p.y) || isnan(p.z)) ++nan;
				lowest = fminf(lowest, p.y - physicsGetParticleRadius(scene.physics, pa));
				for (int b = a + 1; b < scene.body_count; ++b) {
					for (int j = 0; j < scene.count[b]; ++j) {
						const physics_particle_id_t pb = scene.first[b] + j;
						const float overlap = physicsGetParticleRadius(scene.physics, pa) +
							physicsGetParticleRadius(scene.physics, pb) -
							vec3fDistance(p, physicsGetParticlePosition(scene.physics, pb));
						worst_overlap = fmaxf(worst_overlap, overlap);
					}
				}
			}
		}
		for (int i = 0; i < 4; ++i) final_positions[run][i] = testCentroid(&scene, i);
		if (run == 0) {
			char detail[128];
			const float diameter = 2.0f * physicsGetParticleRadius(scene.physics, scene.first[0]);
			sprintf_s(detail, sizeof(detail), "nan=%d lowest=%.4f overlap=%.0f%% of a contact diameter",
				nan, lowest, 100.0f * worst_overlap / diameter);
			testCheck(nan == 0 && lowest > -0.01f && worst_overlap < 0.25f * diameter,
				"pile stays sane", detail);
		}
		physicsDestroy(scene.physics);
	}
	bool same = memcmp(final_positions[0], final_positions[1], sizeof(final_positions[0])) == 0;
	testCheck(same, "pile is deterministic", same ? "identical runs" : "runs differ");
}

/* Deformation damping must leave rigid rotation alone. */
static void testPhysicsSpin(heap_t* heap) {
	physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
	physicsSetGravity(scene.physics, 0.0f);
	testSceneAddBall(&scene, (vec3f_t){ 0, 5, 0 }, 1.0f, 1.0f);
	const vec3f_t center = testCentroid(&scene, 0);
	for (int i = 0; i < scene.count[0]; ++i) {
		const vec3f_t r = vec3fSub(physicsGetParticlePosition(scene.physics, scene.first[0] + i), center);
		physicsSetParticleVelocity(scene.physics, scene.first[0] + i,
			(vec3f_t){ -5.0f * r.y, 5.0f * r.x, 0 });
	}
	testSceneRun(&scene, 60);
	const vec3f_t c = testCentroid(&scene, 0);
	float momentum = 0.0f, inertia = 0.0f;
	for (int i = 0; i < scene.count[0]; ++i) {
		const vec3f_t r = vec3fSub(physicsGetParticlePosition(scene.physics, scene.first[0] + i), c);
		const vec3f_t v = physicsGetParticleVelocity(scene.physics, scene.first[0] + i);
		momentum += r.x * v.y - r.y * v.x;
		inertia += r.x * r.x + r.y * r.y;
	}
	char detail[64];
	sprintf_s(detail, sizeof(detail), "spin 5.00 -> %.2f rad/s after 1 s", momentum / inertia);
	testCheck(momentum / inertia > 4.5f, "free spin is preserved", detail);
	physicsDestroy(scene.physics);
}

/* A ball pushed along the ground rolls and then comes to rest. */
static void testPhysicsRollingStops(heap_t* heap) {
	physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
	testSceneAddBall(&scene, (vec3f_t){ 0, 1.1f, 0 }, 1.0f, 1.0f);
	testSceneRun(&scene, 60);
	for (int i = 0; i < scene.count[0]; ++i) {
		physicsSetParticleVelocity(scene.physics, scene.first[0] + i, (vec3f_t){ 3, 0, 0 });
	}
	testSceneRun(&scene, 30);
	const float rolled_from = testCentroid(&scene, 0).x;
	testSceneRun(&scene, 420);
	const vec3f_t velocity = testAverageVelocity(&scene, 0);
	const float speed = sqrtf(velocity.x * velocity.x + velocity.z * velocity.z);
	char detail[96];
	sprintf_s(detail, sizeof(detail), "rolled %.2f m, speed after 7.5 s %.3f m/s",
		testCentroid(&scene, 0).x - rolled_from, speed);
	testCheck(speed < 0.1f, "rolling comes to rest", detail);
	physicsDestroy(scene.physics);
}

/* A banner pinned along one edge falls and hangs straight down. */
static void testPhysicsBannerHangs(heap_t* heap) {
	physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
	cloth_info_t banner = clothInfoDefault((vec3f_t){ 0, 5, 0 }, vec3fOne());
	banner.resolution = 20;
	banner.size = 2.0f;
	banner.pin = CLOTH_PIN_EDGE;
	testSceneAddCloth(&scene, banner);
	testSceneRun(&scene, 600);
	float lowest = FLT_MAX;
	int nan = 0;
	for (int i = 0; i < scene.count[0]; ++i) {
		const vec3f_t p = physicsGetParticlePosition(scene.physics, scene.first[0] + i);
		if (isnan(p.y)) ++nan;
		lowest = fminf(lowest, p.y);
	}
	char detail[80];
	sprintf_s(detail, sizeof(detail), "bottom edge at %.2f m (expected about 3.0), nan=%d", lowest, nan);
	testCheck(nan == 0 && fabsf(lowest - 3.0f) < 0.25f, "banner hangs from its edge", detail);
	physicsDestroy(scene.physics);
}

/* A ball dropped onto a border-pinned sheet is caught by it. */
static void testPhysicsTrampolineCatches(heap_t* heap) {
	physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
	cloth_info_t trampoline = clothInfoDefault((vec3f_t){ 0, 3, 0 }, vec3fOne());
	trampoline.pin = CLOTH_PIN_BORDER;
	testSceneAddCloth(&scene, trampoline);
	testSceneAddBall(&scene, (vec3f_t){ 0, 6, 0 }, 1.0f, 1.0f);
	testSceneRun(&scene, 360);
	const vec3f_t ball = testCentroid(&scene, 1);
	char detail[80];
	sprintf_s(detail, sizeof(detail), "ball rests at y=%.2f with the sheet pinned at 3.0", ball.y);
	testCheck(ball.y > 3.0f && fabsf(ball.x) < 1.5f && fabsf(ball.z) < 1.5f,
		"trampoline catches a ball", detail);
	physicsDestroy(scene.physics);
}

/* Picking straight down finds the ball, and dragging the grab lifts it. */
static void testPhysicsGrab(heap_t* heap) {
	physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
	testSceneAddBall(&scene, (vec3f_t){ 2, 1.1f, 0 }, 1.0f, 1.0f);
	testSceneRun(&scene, 60);
	const vec3f_t before = testCentroid(&scene, 0);
	const vec3f_t origin = { before.x, before.y + 5.0f, before.z };
	const vec3f_t down = { 0, -1, 0 };
	physics_particle_id_t particle;
	float distance;
	const bool picked = physicsPickParticle(scene.physics, origin, down, false, &particle, &distance);
	testCheck(picked && particle >= scene.first[0] &&
		(int)(particle - scene.first[0]) < scene.count[0],
		"pick finds the ball under the ray", picked ? "hit" : "missed");
	if (picked) {
		const vec3f_t grab_point = vec3fAdd(origin, vec3fScale(down, distance));
		physicsGrab(scene.physics, particle, grab_point, 0.35f);
		for (int f = 0; f < 120; ++f) {
			const float lift = 2.0f * fminf(1.0f, (float)f / 60.0f);
			physicsMoveGrab(scene.physics, vec3fAdd(grab_point, (vec3f_t){ 0, lift, 0 }));
			physicsUpdate(scene.physics, 1.0f / 60.0f);
		}
		const float lifted = testCentroid(&scene, 0).y - before.y;
		physicsReleaseGrab(scene.physics);
		testSceneRun(&scene, 180);
		char detail[96];
		sprintf_s(detail, sizeof(detail), "lifted %.2f of 2.00 m, back at y=%.2f after release",
			lifted, testCentroid(&scene, 0).y);
		testCheck(lifted > 1.6f && fabsf(testCentroid(&scene, 0).y - before.y) < 0.2f,
			"grab drags and releases", detail);
	}
	physicsDestroy(scene.physics);
}

/* A free cloth falls onto a static sphere collider: it must rest on top without
   any particle inside the sphere, and drape down around it. */
static void testPhysicsClothDrapesOverSphere(heap_t* heap) {
	physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
	const vec3f_t center = { 0, 1.2f, 0 };
	const float sphere_radius = 1.2f;
	physicsAddSphereCollider(scene.physics, center, sphere_radius);
	cloth_info_t cloth = clothInfoDefault((vec3f_t){ 0, 3.5f, 0 }, vec3fOne());
	cloth.size = 3.0f;
	cloth.pin = CLOTH_PIN_NONE;
	testSceneAddCloth(&scene, cloth);
	testSceneRun(&scene, 360);
	float deepest = 0.0f, top = -FLT_MAX, lowest = FLT_MAX;
	int nan = 0;
	for (int i = 0; i < scene.count[0]; ++i) {
		const physics_particle_id_t particle = scene.first[0] + i;
		const vec3f_t p = physicsGetParticlePosition(scene.physics, particle);
		if (isnan(p.x) || isnan(p.y) || isnan(p.z)) ++nan;
		const float gap = vec3fDistance(p, center) - sphere_radius -
			physicsGetParticleRadius(scene.physics, particle);
		deepest = fminf(deepest, gap);
		top = fmaxf(top, p.y);
		lowest = fminf(lowest, p.y);
	}
	char detail[128];
	sprintf_s(detail, sizeof(detail), "deepest penetration %.3f m, top %.2f m, lowest %.2f m, nan=%d",
		-deepest, top, lowest, nan);
	testCheck(nan == 0 && deepest > -0.02f && top > 2.3f && top < 2.7f && lowest < 1.5f,
		"cloth drapes over a solid sphere", detail);
	physicsDestroy(scene.physics);
}

/* A cloth with one hand-placed pin and no preset pins hangs from that point: the
   pinned particle stays put and the rest falls below it. */
static void testPhysicsClothHangsFromCustomPin(heap_t* heap) {
	physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
	cloth_info_t cloth = clothInfoDefault((vec3f_t){ 0, 4, 0 }, vec3fOne());
	cloth.resolution = 24;
	cloth.size = 2.0f;
	cloth.pin = CLOTH_PIN_NONE;
	clothInfoTogglePin(&cloth, 12, 12);
	testSceneAddCloth(&scene, cloth);
	const physics_particle_id_t pinned = scene.first[0] + 12 * 24 + 12;
	const vec3f_t pinned_before = physicsGetParticlePosition(scene.physics, pinned);
	testSceneRun(&scene, 300);
	const vec3f_t pinned_after = physicsGetParticlePosition(scene.physics, pinned);
	float lowest = FLT_MAX;
	for (int i = 0; i < scene.count[0]; ++i) {
		lowest = fminf(lowest, physicsGetParticlePosition(scene.physics, scene.first[0] + i).y);
	}
	char detail[128];
	sprintf_s(detail, sizeof(detail), "%d pin, pinned point moved %.4f m, lowest point %.2f m",
		clothInfoCountPins(&cloth), vec3fDistance(pinned_before, pinned_after), lowest);
	testCheck(clothInfoCountPins(&cloth) == 1 && vec3fDistance(pinned_before, pinned_after) == 0.0f &&
		lowest < 3.2f, "cloth hangs from a hand-placed pin", detail);
	physicsDestroy(scene.physics);
}

// ================================================
//					DATA STRUCTURES
// ================================================

#include "deque.h"
#include "hashtable.h"

/* Random inserts, overwrites and removals checked against a plain array, so
   growth and backward-shift deletion are both exercised. */
static void testHashTable(heap_t* heap) {
	enum { KEYS = 4096, OPERATIONS = 200000 };
	uint64_t* reference = heapAlloc(heap, sizeof(uint64_t) * KEYS, 8);
	bool* present = heapAlloc(heap, sizeof(bool) * KEYS, 8);
	memset(present, 0, sizeof(bool) * KEYS);
	hash_table_t* table = hashTableCreate(heap, 4);
	uint32_t random = 12345u;
	int mismatches = 0;
	for (int op = 0; op < OPERATIONS; ++op) {
		random = random * 1664525u + 1013904223u;
		/* Keys spread over the high bits too, like packed pairs. */
		const int k = (int)((random >> 8) % KEYS);
		const uint64_t key = hashTablePairKey((uint32_t)k * 7u, (uint32_t)k * 13u + 1u);
		if ((random & 3u) == 0u) {
			const bool removed = hashTableRemove(table, key);
			if (removed != present[k]) ++mismatches;
			present[k] = false;
		}
		else {
			hashTableSet(table, key, (uint64_t)op);
			reference[k] = (uint64_t)op;
			present[k] = true;
		}
	}
	int expected_count = 0;
	for (int k = 0; k < KEYS; ++k) {
		const uint64_t key = hashTablePairKey((uint32_t)k * 7u, (uint32_t)k * 13u + 1u);
		uint64_t value = 0;
		const bool found = hashTableGet(table, key, &value);
		if (found != present[k] || (found && value != reference[k])) ++mismatches;
		expected_count += present[k];
	}
	if (hashTableCount(table) != expected_count) ++mismatches;
	if (hashTablePairKey(3, 9) != hashTablePairKey(9, 3)) ++mismatches;
	hashTableClear(table);
	if (hashTableCount(table) != 0 || hashTableContains(table, hashTablePairKey(0, 1))) ++mismatches;
	char detail[96];
	sprintf_s(detail, sizeof(detail), "%d operations on %d keys, %d mismatches", OPERATIONS, KEYS,
		mismatches);
	testCheck(mismatches == 0, "hash table matches a reference", detail);
	hashTableDestroy(table);
	heapFree(heap, present);
	heapFree(heap, reference);
}

/* Order at both ends, and FIFO use long enough to wrap the slot index many
   times over (a capacity that is not a power of two). */
static void testDeque(heap_t* heap) {
	deque_t* deque = dequeCreate(heap, 5);
	int values[8];
	for (int i = 0; i < 8; ++i) values[i] = i;
	bool ordered = true;
	dequePushBack(deque, &values[1]);
	dequePushBack(deque, &values[2]);
	dequePushFront(deque, &values[0]);
	dequePushBack(deque, &values[3]);
	ordered = ordered && dequePeekFront(deque) == &values[0] && dequeGetCurrentSize(deque) == 4;
	ordered = ordered && dequePopBack(deque) == &values[3];
	ordered = ordered && dequePopFront(deque) == &values[0];
	ordered = ordered && dequePopFront(deque) == &values[1];
	ordered = ordered && dequePopBack(deque) == &values[2];
	ordered = ordered && dequeGetCurrentSize(deque) == 0;
	for (int round = 0; round < 100000 && ordered; ++round) {
		dequePushBack(deque, &values[round % 8]);
		dequePushBack(deque, &values[(round + 1) % 8]);
		ordered = dequePopFront(deque) == &values[round % 8] &&
			dequePopFront(deque) == &values[(round + 1) % 8];
	}
	testCheck(ordered, "deque keeps order at both ends", "push/pop front and back, 200000 FIFO items");
	dequeDestroy(deque);
}

/* Cloth left alone comes to rest: over a long run nothing may feed it energy
   (too-soft bending once made a crumpled cloth writhe at ~15 m/s forever).
   The fastest particle over the last five seconds must be slow. */
static float testClothTopSpeedAfter(heap_t* heap, cloth_info_t cloth, bool sphere, int seconds) {
	physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
	if (sphere) physicsAddSphereCollider(scene.physics, (vec3f_t){ 0, 1.2f, 0 }, 1.2f);
	testSceneAddCloth(&scene, cloth);
	float top_speed = 0.0f;
	for (int second = 1; second <= seconds; ++second) {
		testSceneRun(&scene, 60);
		if (second <= seconds - 5) continue;
		for (int i = 0; i < scene.count[0]; ++i) {
			top_speed = fmaxf(top_speed,
				vec3fMagnitude(physicsGetParticleVelocity(scene.physics, scene.first[0] + i)));
		}
	}
	physicsDestroy(scene.physics);
	return top_speed;
}

/* Two free cloths dropped over the sphere: the upper one slides onto the
   ground and the cloths rest folded on each other. The fastest particle of
   either over the last five seconds. */
static float testStackedClothTopSpeedAfter(heap_t* heap, int seconds) {
	physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
	physicsAddSphereCollider(scene.physics, (vec3f_t){ 0, 1.2f, 0 }, 1.2f);
	cloth_info_t lower = clothInfoDefault((vec3f_t){ 0.4f, 3.6f, 0 }, vec3fOne());
	lower.resolution = 48;
	lower.size = 4.4f;
	lower.pin = CLOTH_PIN_NONE;
	testSceneAddCloth(&scene, lower);
	cloth_info_t upper = clothInfoDefault((vec3f_t){ -0.6f, 5.0f, 0.3f }, vec3fOne());
	upper.pin = CLOTH_PIN_NONE;
	testSceneAddCloth(&scene, upper);
	float top_speed = 0.0f;
	for (int second = 1; second <= seconds; ++second) {
		testSceneRun(&scene, 60);
		if (second <= seconds - 5) continue;
		for (int body = 0; body < scene.body_count; ++body) {
			for (int i = 0; i < scene.count[body]; ++i) {
				top_speed = fmaxf(top_speed, vec3fMagnitude(
					physicsGetParticleVelocity(scene.physics, scene.first[body] + i)));
			}
		}
	}
	physicsDestroy(scene.physics);
	return top_speed;
}

static void testPhysicsClothSettles(heap_t* heap) {
	cloth_info_t drape = clothInfoDefault((vec3f_t){ 0, 3.6f, 0 }, vec3fOne());
	drape.resolution = 48;
	drape.size = 4.4f;
	drape.pin = CLOTH_PIN_NONE;
	const float drape_speed = testClothTopSpeedAfter(heap, drape, true, 40);
	cloth_info_t sheet = clothInfoDefault((vec3f_t){ 0, 6, 0 }, vec3fOne());
	const float sheet_speed = testClothTopSpeedAfter(heap, sheet, false, 40);
	const float stacked_speed = testStackedClothTopSpeedAfter(heap, 40);
	char detail[160];
	sprintf_s(detail, sizeof(detail),
		"top speed over 35-40 s: drape %.3f, corner-pinned sheet %.3f, two stacked cloths %.3f m/s",
		drape_speed, sheet_speed, stacked_speed);
	testCheck(drape_speed < 0.5f && sheet_speed < 0.5f && stacked_speed < 0.2f,
		"cloth comes to rest", detail);
}

int testPhysics(heap_t* heap) {
	test_failures = 0;
	testHashTable(heap);
	testDeque(heap);
	testPhysicsClothHangsFromCustomPin(heap);
	testPhysicsClothDrapesOverSphere(heap);
	testPhysicsClothSettles(heap);
	testPhysicsPile(heap);
	testPhysicsSpin(heap);
	testPhysicsRollingStops(heap);
	testPhysicsBannerHangs(heap);
	testPhysicsTrampolineCatches(heap);
	testPhysicsGrab(heap);
	debugPrintConsole("%d physics test(s) failed\n", test_failures);
	return test_failures;
}

// ================================================
//					PHYSICS BENCHMARK
// ================================================

typedef enum bench_scene_t {
	BENCH_DRAPE,   // the drape demo: a 48 x 48 cloth over a solid sphere
	BENCH_DEMO,    // the trampoline demo: two cloths and four balls
	BENCH_PILE,    // sixteen balls dropped in a heap
	BENCH_SHEET,   // the largest cloth (64 x 64), hanging from its corners
	BENCH_SCENE_COUNT
} bench_scene_t;

static const char* bench_scene_names[BENCH_SCENE_COUNT] = {
	"drape 48x48 over sphere", "trampoline demo", "pile of 16 balls", "sheet 64x64 corners"
};

static void benchBuildScene(physics_test_scene_t* scene, bench_scene_t which) {
	switch (which) {
		case BENCH_DRAPE: {
			physicsAddSphereCollider(scene->physics, (vec3f_t){ 0, 1.2f, 0 }, 1.2f);
			cloth_info_t cloth = clothInfoDefault((vec3f_t){ 0, 3.6f, 0 }, vec3fOne());
			cloth.resolution = 48;
			cloth.size = 4.4f;
			cloth.pin = CLOTH_PIN_NONE;
			testSceneAddCloth(scene, cloth);
			break;
		}
		case BENCH_DEMO: {
			cloth_info_t trampoline = clothInfoDefault((vec3f_t){ 0, 3, 0 }, vec3fOne());
			trampoline.pin = CLOTH_PIN_BORDER;
			testSceneAddCloth(scene, trampoline);
			cloth_info_t banner = clothInfoDefault((vec3f_t){ -4.5f, 5.5f, 1.5f }, vec3fOne());
			banner.resolution = 24;
			banner.size = 2.5f;
			banner.pin = CLOTH_PIN_EDGE;
			testSceneAddCloth(scene, banner);
			testSceneAddBall(scene, (vec3f_t){ -.85f, 5, .45f }, 1.0f, 1.0f);
			testSceneAddBall(scene, (vec3f_t){ .85f, 6.5f, -.45f }, 1.5f, 1.0f);
			testSceneAddBall(scene, (vec3f_t){ -3, 1.2f, -3 }, 0.4f, 0.6f);
			testSceneAddBall(scene, (vec3f_t){ 4.5f, 1.2f, 1 }, 0.5f, 1.0f);
			break;
		}
		case BENCH_PILE:
			for (int i = 0; i < 16; ++i) {
				testSceneAddBall(scene, (vec3f_t){ (float)(i % 4) * 1.6f - 2.4f,
					2.0f + (float)(i / 4) * 2.3f, (float)(i % 3) * 0.6f - 0.6f }, 1.0f, 1.0f);
			}
			break;
		default: {
			cloth_info_t sheet = clothInfoDefault((vec3f_t){ 0, 6, 0 }, vec3fOne());
			sheet.resolution = 64;
			sheet.size = 5.0f;
			testSceneAddCloth(scene, sheet);
			break;
		}
	}
}

/* Each scene runs for ten simulated seconds at 60 Hz, twice; the faster run's
   averages are reported (the first second, while things fall, is included). */
int testBenchmark(heap_t* heap) {
	enum { BENCH_FRAMES = 600, BENCH_RUNS = 2 };
	debugPrintConsole("%-26s %8s %8s %8s %8s %8s %8s %8s %8s\n", "scene", "total", "max",
		"detect", "constr", "contact", "integr", "veloc", "damp");
	for (int which = 0; which < BENCH_SCENE_COUNT; ++which) {
		physics_profile_t best = { .total_ms = FLT_MAX };
		float best_max = 0.0f;
		int particles = 0, contacts = 0;
		for (int run = 0; run < BENCH_RUNS; ++run) {
			physics_test_scene_t scene = { .heap = heap, .physics = physicsCreate(heap) };
			benchBuildScene(&scene, (bench_scene_t)which);
			physics_profile_t sum = { 0 };
			float max_ms = 0.0f;
			int contact_sum = 0;
			for (int frame = 0; frame < BENCH_FRAMES; ++frame) {
				physicsUpdate(scene.physics, 1.0f / 60.0f);
				const physics_profile_t p = physicsGetProfile(scene.physics);
				sum.detect_ms += p.detect_ms;
				sum.constraints_ms += p.constraints_ms;
				sum.contacts_ms += p.contacts_ms;
				sum.integrate_ms += p.integrate_ms;
				sum.velocity_ms += p.velocity_ms;
				sum.damping_ms += p.damping_ms;
				sum.total_ms += p.total_ms;
				max_ms = fmaxf(max_ms, p.total_ms);
				contact_sum += physicsGetStats(scene.physics).contact_count;
			}
			if (sum.total_ms < best.total_ms) {
				best = sum;
				best_max = max_ms;
				particles = physicsGetStats(scene.physics).particle_count;
				contacts = contact_sum / BENCH_FRAMES;
			}
			physicsDestroy(scene.physics);
		}
		const float n = (float)BENCH_FRAMES;
		debugPrintConsole("%-26s %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f %8.3f   (%d particles, %d contacts)\n",
			bench_scene_names[which], best.total_ms / n, best_max, best.detect_ms / n,
			best.constraints_ms / n, best.contacts_ms / n, best.integrate_ms / n,
			best.velocity_ms / n, best.damping_ms / n, particles, contacts);
	}
	return 0;
}

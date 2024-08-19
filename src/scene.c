#include "scene.h"

#include "heap.h"
#include "fs.h"
#include "wm.h"
#include "renderer.h"
#include "gpu.h"
#include "transform.h"
#include "timer_object.h"
#include "debug.h"
#include "vec3f.h"

#define _USE_MATH_DEFINES
#include <math.h>
#include <string.h>
#include <stdint.h>

scene_t* sceneCreate(heap_t* heap, fs_t* fs, wm_window_t* window, renderer_t* render) {

}

void sceneDestroy(scene_t* scene);

void sceneUpdate(scene_t* scene);
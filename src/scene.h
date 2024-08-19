#ifndef __SCENE_H__
#define __SCENE_H__

typedef struct scene_t scene_t;

typedef struct fs_t fs_t;
typedef struct heap_t heap_t;
typedef struct renderer_t renderer_t;
typedef struct wm_window_t wm_window_t;

scene_t* sceneCreate(heap_t* heap, fs_t* fs, wm_window_t* window, renderer_t* render);

void sceneDestroy(scene_t* scene);

void sceneUpdate(scene_t* scene);

#endif
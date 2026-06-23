#ifndef __SHADER_LOADER_H__
#define __SHADER_LOADER_H__

typedef struct fs_t fs_t;
typedef struct heap_t heap_t;
typedef struct gpu_shader_info_t gpu_shader_info_t;
typedef struct shader_loader_t shader_loader_t;

shader_loader_t* shaderLoaderCreate(heap_t* heap, fs_t* fs);
void shaderLoaderDestroy(shader_loader_t* loader);
gpu_shader_info_t* shaderLoaderGetFlat(shader_loader_t* loader);
gpu_shader_info_t* shaderLoaderGetPbr(shader_loader_t* loader);

#endif

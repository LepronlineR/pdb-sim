#include "shader_loader.h"

#include "fs.h"
#include "gpu.h"
#include "heap.h"

#include <string.h>

typedef struct shader_asset_t {
	gpu_shader_info_t info;
	fs_work_t* vertex_work;
	fs_work_t* fragment_work;
} shader_asset_t;

typedef struct shader_loader_t {
	heap_t* heap;
	fs_t* fs;
	shader_asset_t flat;
	shader_asset_t pbr;
} shader_loader_t;

static void shaderLoaderLoadAsset(shader_loader_t* loader, shader_asset_t* asset,
	const char* vertex_path, const char* fragment_path) {
	asset->vertex_work = fsRead(loader->fs, vertex_path, loader->heap, false, false);
	asset->fragment_work = fsRead(loader->fs, fragment_path, loader->heap, false, false);
	asset->info = (gpu_shader_info_t){
		.vtx_shader_data = fsWorkGetBuffer(asset->vertex_work),
		.vtx_shader_size = fsWorkGetSize(asset->vertex_work),
		.frag_shader_data = fsWorkGetBuffer(asset->fragment_work),
		.frag_shader_size = fsWorkGetSize(asset->fragment_work),
		.uniform_buffer_count = 1
	};
}

static void shaderLoaderUnloadAsset(shader_asset_t* asset) {
	fsWorkDestroy(asset->vertex_work);
	fsWorkDestroy(asset->fragment_work);
}

shader_loader_t* shaderLoaderCreate(heap_t* heap, fs_t* fs) {
	shader_loader_t* loader = heapAlloc(heap, sizeof(*loader), _Alignof(shader_loader_t));
	memset(loader, 0, sizeof(*loader));
	loader->heap = heap;
	loader->fs = fs;

	shaderLoaderLoadAsset(loader, &loader->flat,
		"shaders/triangle.vert.spv", "shaders/triangle.frag.spv");
	shaderLoaderLoadAsset(loader, &loader->pbr,
		"shaders/pbr.vert.spv", "shaders/pbr.frag.spv");

	return loader;
}

void shaderLoaderDestroy(shader_loader_t* loader) {
	if (!loader) return;
	shaderLoaderUnloadAsset(&loader->flat);
	shaderLoaderUnloadAsset(&loader->pbr);
	heapFree(loader->heap, loader);
}

gpu_shader_info_t* shaderLoaderGetFlat(shader_loader_t* loader) {
	return &loader->flat.info;
}

gpu_shader_info_t* shaderLoaderGetPbr(shader_loader_t* loader) {
	return &loader->pbr.info;
}

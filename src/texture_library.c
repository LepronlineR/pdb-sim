#include "texture_library.h"

#include "debug.h"
#include "fs.h"
#include "gpu.h"
#include "heap.h"
#include "image.h"

#include <string.h>

typedef struct texture_asset_t {
	gpu_texture_info_t info;
	image_t image;
	bool loaded;
} texture_asset_t;

typedef struct texture_library_t {
	heap_t* heap;
	texture_asset_t assets[TEXTURE_COUNT];
} texture_library_t;

typedef struct texture_source_t {
	const char* name;
	const char* path;  // NULL: generated
	bool srgb;
	bool clamp_v;
} texture_source_t;

static const texture_source_t texture_sources[TEXTURE_COUNT] = {
	[TEXTURE_NONE] = { "None", NULL, false, false },
	[TEXTURE_GRASS] = { "Grass", "textures/grass.png", true, false },
	[TEXTURE_ROCK] = { "Rock", "textures/rock.png", true, false },
	[TEXTURE_SNOW] = { "Snow", "textures/snow.png", true, false },
	[TEXTURE_CHECKER] = { "Checker", NULL, true, false },
	[TEXTURE_SKY] = { "Sky", "textures/sky_daylight.png", true, true },
};

/* An 8 x 8 two-tone checker, so uvs and deformation are easy to read. Mild, so
   it shades the object's color rather than replacing it. */
static void textureLibraryMakeChecker(heap_t* heap, image_t* image) {
	const int size = 256;
	const int square = size / 8;
	image->width = size;
	image->height = size;
	image->pixels = heapAlloc(heap, (size_t)size * size * 4, 16);
	for (int y = 0; y < size; ++y) {
		for (int x = 0; x < size; ++x) {
			const bool light = ((x / square) + (y / square)) % 2 == 0;
			unsigned char* pixel = image->pixels + ((size_t)y * size + x) * 4;
			pixel[0] = pixel[1] = pixel[2] = light ? 255 : 199;  // 0.78 when dark
			pixel[3] = 255;
		}
	}
}

texture_library_t* textureLibraryCreate(heap_t* heap, fs_t* fs) {
	texture_library_t* library = heapAlloc(heap, sizeof(*library), _Alignof(texture_library_t));
	memset(library, 0, sizeof(*library));
	library->heap = heap;

	/* Queue every read first so fs can work on them while earlier ones decode. */
	fs_work_t* reads[TEXTURE_COUNT] = { 0 };
	for (int id = 0; id < TEXTURE_COUNT; ++id) {
		if (texture_sources[id].path) {
			reads[id] = fsRead(fs, texture_sources[id].path, heap, false, false);
		}
	}
	for (int id = 0; id < TEXTURE_COUNT; ++id) {
		texture_asset_t* asset = &library->assets[id];
		if (id == TEXTURE_CHECKER) {
			textureLibraryMakeChecker(heap, &asset->image);
			asset->loaded = true;
		}
		else if (reads[id]) {
			const bool read = fsWorkGetErrorCode(reads[id]) == 0 && fsWorkGetSize(reads[id]) > 0;
			asset->loaded = read && imageDecode(heap, fsWorkGetBuffer(reads[id]),
				fsWorkGetSize(reads[id]), &asset->image);
			if (!asset->loaded) {
				debugPrint(DEBUG_PRINT_WARNING, "Texture library: could not load %s.\n",
					texture_sources[id].path);
			}
			fsWorkDestroy(reads[id]);
		}
		if (asset->loaded) {
			asset->info = (gpu_texture_info_t){
				.id = gpuTextureInfoNextId(),
				.width = asset->image.width,
				.height = asset->image.height,
				.pixels = asset->image.pixels,
				.srgb = texture_sources[id].srgb,
				.clamp_v = texture_sources[id].clamp_v
			};
		}
	}
	return library;
}

void textureLibraryDestroy(texture_library_t* library) {
	if (!library) return;
	for (int id = 0; id < TEXTURE_COUNT; ++id) {
		imageFree(library->heap, &library->assets[id].image);
	}
	heapFree(library->heap, library);
}

gpu_texture_info_t* textureLibraryGet(texture_library_t* library, texture_id_t id) {
	if (!library || id <= TEXTURE_NONE || id >= TEXTURE_COUNT || !library->assets[id].loaded) {
		return NULL;
	}
	return &library->assets[id].info;
}

const char* textureLibraryName(texture_id_t id) {
	return id >= 0 && id < TEXTURE_COUNT ? texture_sources[id].name : "?";
}

int textureLibraryLoadedCount(texture_library_t* library) {
	int count = 0;
	for (int id = TEXTURE_NONE + 1; id < TEXTURE_COUNT; ++id) {
		if (library->assets[id].loaded) ++count;
	}
	return count;
}

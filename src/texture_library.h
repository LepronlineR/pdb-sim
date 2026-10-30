#ifndef __TEXTURE_LIBRARY_H__
#define __TEXTURE_LIBRARY_H__

#include <stdbool.h>

/*		TEXTURE LIBRARY
*
*	The textures the scene can use, loaded once at startup (like shader_loader):
*	files under textures/ are read through fs and decoded, and the checker is
*	generated. A texture that fails to load is missing (NULL) and objects that
*	ask for it render untextured.
*/

typedef struct fs_t fs_t;
typedef struct heap_t heap_t;
typedef struct gpu_texture_info_t gpu_texture_info_t;
typedef struct texture_library_t texture_library_t;

typedef enum texture_id_t {
	TEXTURE_NONE,     // plain vertex color
	TEXTURE_GRASS,
	TEXTURE_ROCK,
	TEXTURE_SNOW,
	TEXTURE_CHECKER,  // generated, no file
	TEXTURE_SKY,      // equirectangular (2:1) sky panorama
	TEXTURE_COUNT
} texture_id_t;

#ifdef __cplusplus
extern "C" {
#endif

texture_library_t* textureLibraryCreate(heap_t* heap, fs_t* fs);
void textureLibraryDestroy(texture_library_t* library);
// RETURN: the texture, or NULL for TEXTURE_NONE and textures that failed to load
gpu_texture_info_t* textureLibraryGet(texture_library_t* library, texture_id_t id);
const char* textureLibraryName(texture_id_t id);
// Number of textures that loaded (TEXTURE_NONE excluded).
int textureLibraryLoadedCount(texture_library_t* library);

#ifdef __cplusplus
}
#endif

#endif

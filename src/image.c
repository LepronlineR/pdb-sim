#include "image.h"

#include "heap.h"

#include <string.h>

/* Only the decoders the engine uses, reading from memory (files come from fs). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#include "stb/stb_image.h"

bool imageDecode(heap_t* heap, const void* data, size_t size, image_t* image) {
	memset(image, 0, sizeof(*image));
	if (!data || size == 0 || size > 0x7fffffff) return false;
	int width, height, channels;
	stbi_uc* decoded = stbi_load_from_memory((const stbi_uc*)data, (int)size, &width, &height,
		&channels, 4);
	if (!decoded) return false;
	/* stb allocates with malloc; keep the result on the engine heap like
	   everything else. */
	const size_t bytes = (size_t)width * (size_t)height * 4;
	image->pixels = heapAlloc(heap, bytes, 16);
	memcpy(image->pixels, decoded, bytes);
	stbi_image_free(decoded);
	image->width = width;
	image->height = height;
	return true;
}

void imageFree(heap_t* heap, image_t* image) {
	if (image && image->pixels) heapFree(heap, image->pixels);
	if (image) memset(image, 0, sizeof(*image));
}

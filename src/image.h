#ifndef __IMAGE_H__
#define __IMAGE_H__

#include <stdbool.h>
#include <stddef.h>

/*		IMAGE
*
*	Decodes PNG and JPEG files (already read into memory, e.g. by fs) into 8-bit
*	RGBA pixels, using stb_image.
*/

typedef struct heap_t heap_t;

typedef struct image_t {
	int width;
	int height;
	unsigned char* pixels;  // width * height * 4 bytes, rows top to bottom
} image_t;

// Decodes an encoded image into RGBA pixels allocated from the heap.
//
// RETURN: false if the data is not an image stb_image can read
bool imageDecode(heap_t* heap, const void* data, size_t size, image_t* image);

void imageFree(heap_t* heap, image_t* image);

#endif

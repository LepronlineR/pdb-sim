#ifndef __FS_H__
#define __FS_H__

#include <stdbool.h>
#include <stdint.h> 
/* ASYNC R/W FILE SYSTEM 
*/

// Handle to file system.
typedef struct fs_t fs_t;

// Handle to file work.
typedef struct fs_work_t fs_work_t;

typedef struct heap_t heap_t;

// Create a new file system.
// Provided heap will be used to allocate space for queue and work buffers.
// Provided queue size defines number of in-flight file operations.
fs_t* fsCreate(heap_t* heap, int queue_capacity);

// Destroy a previously created file system.
void fsDestroy(fs_t* fs);

// if file compression is used, then send the 
// compression size to the compression buffer.
// void fileReadCompressionSize(fs_work_t* work);

// Queue a file read.
// File at the specified path will be read in full.
// Memory for the file will be allocated out of the provided heap.
// It is the calls responsibility to free the memory allocated!
// Returns a work object.
fs_work_t* fsRead(fs_t* fs, const char* path, heap_t* heap, bool null_term, bool compress);

// Queue a file write.
// File at the specified path will be written in full.
// Returns a work object.
fs_work_t* fsWrite(fs_t* fs, const char* path, const void* buffer, size_t size, bool compress);

// If true, the file work is complete.
bool fsWorkCheckStatus(fs_work_t* work);

// Block for the file work to complete.
void fsWorkBlock(fs_work_t* work);

// Get the error code for the file work.
// A value of zero generally indicates success.
int fsWorkGetErrorCode(fs_work_t* work);

// Get the buffer associated with the file operation.
void* fsWorkGetBuffer(fs_work_t* work);

// Get the size associated with the file operation.
size_t fsWorkGetSize(fs_work_t* work);

// Free a file work object.
void fsWorkDestroy(fs_work_t* work);

#endif
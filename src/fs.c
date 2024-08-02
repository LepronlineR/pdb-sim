#include "fs.h"

#include "event.h"
#include "heap.h"
#include "deque.h"
#include "thread.h"
#include "debug.h"

#include <stddef.h>
#include <stdio.h>
#include <lz4/lz4.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

typedef struct fs_t {
	heap_t* heap;
	deque_t* file_queue;
	thread_t* file_thread;
	deque_t* compression_file_queue;
	thread_t* compression_file_thread;
} fs_t;

typedef enum fs_work_op_t {
	FS_OP_WRITE,
	FS_OP_READ,
} fs_work_op_t;

typedef struct fs_work_t {
	heap_t* heap;
	fs_work_op_t op;
	char path[1024];
	bool null_term;
	bool compress;
	char* buffer;
	bool allocated_buffer;
	size_t size;
	size_t compressed_size;
	event_t* done;
	int result;
} fs_work_t;

static int fileThreadFunc(void* user);
static int compressThreadFunc(void* user);

fs_t* fsCreate(heap_t* heap, int queue_capacity) {
	fs_t* fs = heapAlloc(heap, sizeof(fs_t), 8);
	fs->heap = heap;
	fs->file_queue = dequeCreate(heap, queue_capacity);
	fs->file_thread = threadCreate(fileThreadFunc, fs);
	// Create the compressor thread and queue for file compression/decompression
	fs->compression_file_queue = dequeCreate(heap, queue_capacity);
	fs->compression_file_thread = threadCreate(compressThreadFunc, fs);
	return fs;
}

void fsDestroy(fs_t* fs) {
	// remove the compressor/decompressor
	dequePushBack(fs->compression_file_queue, NULL);
	threadDestroy(fs->compression_file_thread);
	dequeDestroy(fs->compression_file_queue);
	// remove everything else
	dequePushBack(fs->file_queue, NULL);
	threadDestroy(fs->file_thread);
	dequeDestroy(fs->file_queue);
	heapFree(fs->heap, fs);
}

fs_work_t* fsRead(fs_t* fs, const char* path, heap_t* heap, bool null_term, bool compress) {
	fs_work_t* work = heapAlloc(fs->heap, sizeof(fs_work_t), 8);
	work->heap = heap;
	work->op = FS_OP_READ;
	strcpy_s(work->path, sizeof(work->path), path);
	work->buffer = NULL;
	work->allocated_buffer = false;
	work->size = 0;
	work->compressed_size = 0;
	work->done = eventCreate();
	work->result = 0;
	work->null_term = null_term;
	work->compress = compress;

	// NOTE: if we need to decompress it, then it will automatically send it to
	//		 the decompression queue once it has been read by the work
	dequePushBack(fs->file_queue, work);

	return work;
}

fs_work_t* fsWrite(fs_t* fs, const char* path, const void* buffer, size_t size, bool compress) {
	fs_work_t* work = heapAlloc(fs->heap, sizeof(fs_work_t), 8);
	work->heap = fs->heap;
	work->op = FS_OP_WRITE;
	strcpy_s(work->path, sizeof(work->path), path);
	work->buffer = (char*)buffer;
	work->allocated_buffer = true;
	work->size = size;
	work->compressed_size = 0;
	work->done = eventCreate();
	work->result = 0;
	work->null_term = false;
	work->compress = compress;

	if (compress) {
		dequePushBack(fs->compression_file_queue, work);
	} else {
		dequePushBack(fs->file_queue, work);
	}

	return work;
}

bool fsWorkCheckStatus(fs_work_t* work) {
	return work ? eventIsSignaled(work->done) : true;
}

void fsWorkBlock(fs_work_t* work) {
	if (work)
		eventWait(work->done);
}

int fsWorkGetErrorCode(fs_work_t* work) {
	fsWorkBlock(work);
	return work ? work->result : -1;
}

void* fsWorkGetBuffer(fs_work_t* work) {
	fsWorkBlock(work);
	return work ? work->buffer : NULL;
}

size_t fsWorkGetSize(fs_work_t* work) {
	fsWorkBlock(work);
	return work ? work->size : 0;
}

void fsWorkDestroy(fs_work_t* work) {
	if (work) {
		// remove the event
		eventWait(work->done);
		eventDestroy(work->done);
		if (work->allocated_buffer) {
			heapFree(work->heap, work->buffer);
		}
		heapFree(work->heap, work);
	}
}

static void fileRead(fs_t* fs, fs_work_t* work) {
	wchar_t w_path[1024] = { 0 };
	if (MultiByteToWideChar(CP_UTF8, 0, work->path,
		-1, w_path, sizeof(w_path)) <= 0) {
		work->result = -1;
		return;
	}

	HANDLE file = CreateFile(w_path, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
		FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		work->result = GetLastError();
		return;
	}

	if (!GetFileSizeEx(file, (PLARGE_INTEGER)&work->size)) {
		work->result = GetLastError();
		CloseHandle(file);
		return;
	}

	work->buffer = heapAlloc(work->heap, work->null_term ? work->size + 1 : work->size, 8);

	DWORD bytes_read = 0;
	BOOL read_result = ReadFile(file, work->buffer, (DWORD)work->size, &bytes_read, NULL);
	if (!read_result || bytes_read != work->size) {
		work->result = GetLastError();
		CloseHandle(file);
		return;
	}

	work->allocated_buffer = true;
	work->size = bytes_read;

	if (work->null_term) { // set last as the null term
		((char*)work->buffer)[bytes_read] = 0;
	}

	CloseHandle(file);

	if (work->compress) {
		dequePushBack(fs->compression_file_queue, work);
	} else {
		eventSignal(work->done);
	}
}

static void fileWrite(fs_work_t* work) {
	wchar_t w_path[1024] = { 0 };
	if (MultiByteToWideChar(CP_UTF8, 0, work->path,
		-1, w_path, sizeof(w_path)) <= 0) {
		work->result = -1;
		return;
	}

	HANDLE file = CreateFile(w_path, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
		CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
	if (file == INVALID_HANDLE_VALUE) {
		work->result = GetLastError();
		return;
	}

	DWORD bytes_written = 0;
	if (!WriteFile(file, work->buffer, (DWORD)work->size, &bytes_written, NULL)) {
		work->result = GetLastError();
		CloseHandle(file);
		return;
	}

	work->size = bytes_written;

	CloseHandle(file);

	if (work->compress) {
		// free the buffer (we don't need the compressed buffer)
		heapFree(work->heap, work->buffer);
		work->allocated_buffer = false;
	}

	eventSignal(work->done);
}

static void fileDecompress(fs_work_t* work) {
	int compressed_size = 0;
	memcpy(&compressed_size, work->buffer, sizeof(int));
	int dst_buffer_size = work->size;

	char* dst_buffer = heapAlloc(work->heap, dst_buffer_size, 0);
	int decompressed_size = LZ4_decompress_safe((char*)work->buffer + sizeof(int), dst_buffer, compressed_size, dst_buffer_size);

	if (decompressed_size < 0) {
		heapFree(work->heap, dst_buffer);
		work->result = GetLastError();
		eventSignal(work->done);
		return;
	}

	// Free the original buffer
	heapFree(work->heap, work->buffer);

	// Update work struct with new buffer and size
	work->buffer = dst_buffer;
	work->size = decompressed_size;
	work->compress = true;

	// Signal completion
	eventSignal(work->done);
}

static void fileCompress(fs_t* fs, fs_work_t* work) {
	int dst_buffer_size = LZ4_compressBound(work->size);
	char* dst_buffer = heapAlloc(work->heap, dst_buffer_size + sizeof(int), 8);
	int compressed_size = LZ4_compress_default(work->buffer, dst_buffer + sizeof(int), (int)work->size, dst_buffer_size);
	memcpy(dst_buffer, &compressed_size, sizeof(int));

	// finish the work, push it back to the writing queue to write to file
	work->buffer = dst_buffer;
	work->compressed_size = compressed_size + sizeof(int);
	work->op = FS_OP_WRITE;
	dequePushBack(fs->file_queue, work);
}

static int fileThreadFunc(void* user) {
	fs_t* fs = user;
	while (true) {
		fs_work_t* work = (fs_work_t*) dequePopFront(fs->file_queue);
		if (work == NULL) {
			break;
		}

		switch (work->op) {
			case FS_OP_READ:
				fileRead(fs, work);
				break;
			case FS_OP_WRITE:
				fileWrite(work);
				break;
			default:
				debugPrint(DEBUG_PRINT_ERROR, "File Thread Func: file operation not found.");
				return -1;
		}
	}
	return 0;
}

static int compressThreadFunc(void* user) {
	fs_t* fs = user;
	while (true) {
		fs_work_t* work = (fs_work_t*) dequePopFront(fs->compression_file_queue);
		if (work == NULL) {
			break;
		}

		switch (work->op) {
			case FS_OP_READ:
				fileDecompress(work);
				break;
			case FS_OP_WRITE:
				fileCompress(fs, work);
				break;
			default:
				debugPrint(DEBUG_PRINT_ERROR, "File Thread Func: file operation not found.");
				return -1;
		}
	}
	return 0;
}
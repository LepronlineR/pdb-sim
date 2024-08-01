#include "debug.h"

// set to default (k_print_info) 1 << 0
static uint32_t debug_mask = 0x0000001;

static LONG debugExceptionHandler(LPEXCEPTION_POINTERS pointer) {

	// XXX: MS uses 0xE06D7363 to indicate C++ language exception.
	// We're just to going to ignore them. Sometimes Vulkan throws them on startup?
	// https://devblogs.microsoft.com/oldnewthing/20100730-00/?p=13273
	if (pointer->ExceptionRecord->ExceptionCode == 0xE06D7363) {
		return EXCEPTION_EXECUTE_HANDLER;
	}

	debugPrint(DEBUG_PRINT_ERROR, "Exception Handler: Caught an error. Memory has been dumped.\n");
	
	HANDLE file = CreateFile(L"crash.dmp", GENERIC_READ | GENERIC_WRITE,
		0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);

	if (file != INVALID_HANDLE_VALUE) {
		// create the dump
		MINIDUMP_EXCEPTION_INFORMATION exception = {
			.ThreadId = GetCurrentThreadId(),
			.ExceptionPointers = pointer,
			.ClientPointers = FALSE
		};

		MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
			MiniDumpWithThreadInfo, &exception, NULL, NULL);
		
		CloseHandle(file);
	}

	return EXCEPTION_EXECUTE_HANDLER;
}


void debugInstallExceptionHandler() {
	AddVectoredExceptionHandler(TRUE, debugExceptionHandler);
}

void debugSetPrintMask(uint32_t mask) {
	debug_mask = mask;
}

void debugPrint(uint32_t type, _Printf_format_string_ const char* format, ...) {
	if (debug_mask & type == 0)
		return;

	va_list args;
	va_start(args, format);
	char buffer[256] = { 0 };
	vsnprintf(buffer, 256, format, args);
	va_end(args);

	OutputDebugStringA(buffer);

	DWORD w = 0;
	HANDLE handle = GetStdHandle(STD_OUTPUT_HANDLE);
	WriteConsoleA(handle, buffer, (DWORD)strlen(buffer), &w, NULL);
}

void debugBacktrace(size_t mem_size, USHORT frames, char** backtrace) {
	debugPrint(DEBUG_PRINT_WARNING,
		"Memory Leak: (size: %zu):\n", mem_size);
	for (int x = 0; x < frames; x++) {
		debugPrint(DEBUG_PRINT_WARNING, "[%d] %s", x, backtrace[x]);
	}
}
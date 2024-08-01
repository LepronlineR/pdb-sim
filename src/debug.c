#include "debug.h"

static uint32_t debug_mask = 0xFFFFFFFF;

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

	// Create a callstack
	debugBacktraceManually();

	return EXCEPTION_EXECUTE_HANDLER;
}

void debugPrintConsole(const char* format, ...) {
	char buffer[256] = { 0 };
	va_list args;
	va_start(args, format);
	OutputDebugStringA(buffer);
	va_end(args);

	OutputDebugStringA(buffer);

	DWORD bytes = (DWORD) strlen(buffer);
	DWORD charsWritten = 0;
	DWORD written = 0;
	HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);
	WriteConsoleA(console, buffer, bytes, &charsWritten, NULL);
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

	debugPrintConsole(format);

	if (type == DEBUG_PRINT_ERROR) // enable backtrace for all errors
		debugBacktraceManually();
}

void debugBacktraceManually(){
	SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(1, sizeof(SYMBOL_INFO) + 256 * sizeof(TCHAR));
	symbol->MaxNameLen = 256;
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);

	void* stack[32];
	HANDLE h_proc = GetCurrentProcess();
	SymInitialize(h_proc, NULL, TRUE);
	unsigned short frames = debugBacktrace(stack, 32);

	HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);

	debugPrintConsole(&console, "\n---------------------------- CALL STACK -------------------------------\n");
	for (USHORT x = 1; x < frames; x++) {
		DWORD64 address = (DWORD64)(stack[x]);
		DWORD displacement = { 0 };
		IMAGEHLP_LINE64 line = { 0 };
		line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

		if (SymFromAddr(h_proc, (DWORD64)(stack[x]), 0, &symbol)
			&& SymGetLineFromAddr64(h_proc, (DWORD64)(stack[x]), &displacement, &line)) {
			debugPrintConsole(&console, "[%i] %s - 0x%0llX (%s:%lu)\n", frames - x - 1, symbol->Name, symbol->Address, line.FileName, line.LineNumber);
		} else {
			debugPrintConsole(&console, "%i: [Error: %lu] - 0x%0llX\n", frames - x - 1, GetLastError(), address);
		}
	}
	debugPrintConsole(&console, "-----------------------------------------------------------------------\n");

	SymCleanup(h_proc);
	free(symbol);
}

void debugBacktraceLeakedMemory(void** stack, int frames) {
	SYMBOL_INFO* symbol = (SYMBOL_INFO*)calloc(1, sizeof(SYMBOL_INFO) + 256 * sizeof(TCHAR));
	symbol->MaxNameLen = 256;
	symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
	HANDLE h_proc = GetCurrentProcess();
	SymInitialize(h_proc, NULL, TRUE);

	HANDLE console = GetStdHandle(STD_OUTPUT_HANDLE);

	debugPrintConsole(&console, "\n------------------------ MEMORY HAS LEAKED ---------------------------\n");
	for (USHORT x = 1; x < frames; x++) {
		DWORD64 address = (DWORD64)(stack[x]);
		DWORD displacement = { 0 };
		IMAGEHLP_LINE64 line = { 0 };
		line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);

		if (SymFromAddr(h_proc, (DWORD64)(stack[x]), 0, &symbol)
			&& SymGetLineFromAddr64(h_proc, (DWORD64)(stack[x]), &displacement, &line)) {
			debugPrintConsole(&console, "[%i] %s - 0x%0llX (%s:%lu)\n", frames - x - 1, symbol->Name, symbol->Address, line.FileName, line.LineNumber);
		}
		else {
			debugPrintConsole(&console, "%i: [Error: %lu] - 0x%0llX\n", frames - x - 1, GetLastError(), address);
		}
	}
	debugPrintConsole(&console, "-----------------------------------------------------------------------\n");

	SymCleanup(h_proc);
	free(symbol);
}

int debugBacktrace(void** stack, int stack_capacity) {
	return CaptureStackBackTrace(1, stack_capacity, stack, NULL);
}
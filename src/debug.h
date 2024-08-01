#ifndef __DEBUG_H__
#define __DEBUG_H__

#include <stdio.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdbool.h>

#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <DbgHelp.h>

#include "heap.h"

/*		DEBUGGING SUPPORT
*
*	for particular flags, this system will send debug messages for debugging purposes
*/

// Flags for debug_print().
typedef enum debug_print_t {
	DEBUG_PRINT_INFO = 1 << 0,
	DEBUG_PRINT_WARNING = 1 << 1,
	DEBUG_PRINT_ERROR = 1 << 2,
} debug_print_t;

// Setup flags for debug printing
//
// RETURN:
static LONG debugExceptionHandler(LPEXCEPTION_POINTERS pointer);


// Prints the format to the console
//
void debugPrintConsole(const char* format, ...);

// Install unhandled exception handler. This will log any errors and release a memory dump.
// 
void debugInstallExceptionHandler();

// Sets the mask of the print type, default is 1 (which is the print info)
//
void debugSetPrintMask(uint32_t mask);

// Logs a message to the console
//
void debugPrint(uint32_t type, _Printf_format_string_ const char* format, ...);

// Given the backtrace of the stack with the total stack frame and size of memory, we debug
// the memory leaks for that particular trace
//
// RETURN: the total frames for the backtrace
int debugBacktrace(void** stack, int stack_capacity);

// Call and print the backtrace manually
//
void debugBacktraceManually();

// Call and print the backtrace of the memory that has leaked
//
void debugBacktraceLeakedMemory(void** stack, int frames);
#endif
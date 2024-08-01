#ifndef __WM_H__
#define __WM_H__

#include <stdint.h>
#include <stdbool.h>

/*		WINDOWS MANAGER
*    
*	wm_window_t represents a single-rendered window
*		- the engine takes the particular window and pumps it every frame, which updates the status of the window
*		- after the window is pumped, user input will be read and evaluated
*/

typedef struct wm_window_t wm_window_t;
typedef struct heap_t heap_t;

// mouse input
enum {
	k_mouse_button_left = 1 << 0,
	k_mouse_button_right = 1 << 1,	
	k_mouse_button_middle = 1 << 2
};

// keyboard input
enum {
	k_key_arr_up = 1 << 0,
	k_key_arr_down = 1 << 1,
	k_key_arr_right = 1 << 2,
	k_key_arr_left = 1 << 3,
	k_key_zero = 1 << 4,
	k_key_one = 1 << 5,
	k_key_two = 1 << 6,
	k_key_three = 1 << 7,
	k_key_four = 1 << 8,
	k_key_five = 1 << 9,
	k_key_six = 1 << 10,
	k_key_seven = 1 << 11,
	k_key_eight = 1 << 12,
	k_key_nine = 1 << 13,
};

// Creates and allocates for a new window
// 
// RETURN: the window process, NULL if the process cannot be allocated
wm_window_t* wmCreateWindow(heap_t* heap);

// Destroy the window
//
void wmDestroyWindow(wm_window_t* window);


// Pumps the messages for a window
//
// RETURN: false if the program stops (quits), true otherwise
bool wmPumpWindow(wm_window_t* window);

// Gets a mask of all mouse buttons that is being held by the user
//
// RETURN: mask of mouse inputs
uint32_t wmGetMouseMask(wm_window_t* window);

// Gets a mask of all key buttons that is being held by the user
//
// RETURN: mask of key inputs
uint32_t wmGetKeyMask(wm_window_t* window);

// Get the mouse location from the window and places it in (x, y)
//
void wmGetMouseLoc(wm_window_t* window, int* x, int* y);

// Get the raw OS window object (HWND)
//
// RETURN: window object
void* wmGetHWND(wm_window_t* window);

#endif
#include <assert.h>
#include <stdlib.h>

#include "wm.h"
#include "heap.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>

typedef struct wm_window_t {
	HWND hwnd;
	heap_t* heap;
	bool quit;
	bool focused;
	uint32_t mouse_mask;
	uint32_t key_mask;
	int mouse_x;
	int mouse_y;
} wm_window_t ;

const struct
{
	int virtual_key;
	int ga_key;
}

key_map_k[] =
{
	{.virtual_key = VK_LEFT, .ga_key = k_key_arr_left, },
	{.virtual_key = VK_RIGHT, .ga_key = k_key_arr_right, },
	{.virtual_key = VK_UP, .ga_key = k_key_arr_up, },
	{.virtual_key = VK_DOWN, .ga_key = k_key_arr_down, },
	{.virtual_key = 0x30, .ga_key = k_key_zero, },
	{.virtual_key = 0x31, .ga_key = k_key_one, },
	{.virtual_key = 0x32, .ga_key = k_key_two, },
	{.virtual_key = 0x33, .ga_key = k_key_three, },
	{.virtual_key = 0x34, .ga_key = k_key_four, },
	{.virtual_key = 0x35, .ga_key = k_key_five, },
	{.virtual_key = 0x36, .ga_key = k_key_six, },
	{.virtual_key = 0x37, .ga_key = k_key_seven, },
	{.virtual_key = 0x38, .ga_key = k_key_eight, },
	{.virtual_key = 0x39, .ga_key = k_key_nine, }
};

// https://learn.microsoft.com/en-us/windows/win32/learnwin32/your-first-windows-program
static LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
	wm_window_t* win = (wm_window_t*)GetWindowLongPtr(hwnd, GWLP_USERDATA);
	if (win) {
		switch (uMsg) {
			case WM_KEYDOWN:
				for (int x = 0; x < _countof(key_map_k); x++) {
					if (key_map_k[x].virtual_key == wParam) {
						win->key_mask |= key_map_k[x].ga_key;
						break;
					}
				}
				break;
			case WM_KEYUP:
				for (int x = 0; x < _countof(key_map_k); x++) {
					if (key_map_k[x].virtual_key == wParam) {
						win->key_mask &= ~key_map_k[x].ga_key;
						break;
					}
				}
				break;
			// L MOUSE BUTTON
			case WM_LBUTTONDOWN:
				win->mouse_mask |= k_mouse_button_left; break;
			case WM_LBUTTONUP:
				win->mouse_mask &= ~k_mouse_button_left; break;
			// R MOUSE BUTTON
			case WM_RBUTTONDOWN:
				win->mouse_mask |= k_mouse_button_right; break;
			case WM_RBUTTONUP:
				win->mouse_mask |= k_mouse_button_right; break;
			// M MOUSE BUTTON
			case WM_MBUTTONDOWN:
				win->mouse_mask |= k_mouse_button_middle; break;
			case WM_MBUTTONUP:
				win->mouse_mask |= k_mouse_button_middle; break;
			// MOVE MOUSE
			case WM_MOUSEMOVE:
				if (win->focused) { // get relative mouse position
					POINT cur_cursor;
					GetCursorPos(&cur_cursor);

					RECT window_rect;
					GetWindowRect(hwnd, &window_rect);
					SetCursorPos(
						(window_rect.left + window_rect.right) / 2,
						(window_rect.top + window_rect.bottom) / 2);

					POINT new_cursor;
					GetCursorPos(&new_cursor);

					win->mouse_x = cur_cursor.x - new_cursor.x;
					win->mouse_y = cur_cursor.y - new_cursor.y;
				}
				break;
			case WM_ACTIVATEAPP:
				ShowCursor(!wParam);
				win->focused = wParam;
				break;
			case WM_CLOSE:
				win->quit = true;
				break;
		}
	}

	return DefWindowProc(hwnd, uMsg, wParam, lParam);
}

wm_window_t* wmCreateWindow(heap_t* heap) {

	WNDCLASS win_class = {
		.lpfnWndProc = WindowProc,
		.hInstance = GetModuleHandle(NULL),
		.lpszClassName = L"PBR Simulation"
	};

	RegisterClass(&win_class);

	HWND hwnd = CreateWindowExW(
		0,
		win_class.lpszClassName,
		L"PBR Simulation",
		WS_OVERLAPPEDWINDOW,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		CW_USEDEFAULT,
		NULL,
		NULL,
		win_class.hInstance,
		NULL
	);

	if (!hwnd) {
		return NULL;
	}

	wm_window_t* win = heapAlloc(heap, sizeof(wm_window_t), 8);
	win->focused = false;
	win->hwnd = hwnd;
	win->key_mask = 0;
	win->mouse_mask = 0;
	win->quit = 0;
	win->heap = heap;

	// place win as a long ptr through the hwnd so it can be accessible
	SetWindowLongPtr(hwnd, GWLP_USERDATA, (LONG_PTR)win);
	
	// Windows begins as hidden so we have to show it
	ShowWindow(hwnd, TRUE);

	return win;
}

void wmDestroyWindow(wm_window_t* window) {
	DestroyWindow(window->hwnd);
	heapFree(window->heap, window);
}

bool wmPumpWindow(wm_window_t* window) {
	MSG msg = { 0 };
	while (PeekMessage(&msg, window->hwnd, 0, 0, PM_REMOVE)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	return !window->quit;
}

uint32_t wmGetMouseMask(wm_window_t* window) {
	return window->mouse_mask;
}

uint32_t wmGetKeyMask(wm_window_t* window) {
	return window->key_mask;
}

void wmGetMouseLoc(wm_window_t* window, int* x, int* y) {
	*x = window->mouse_x;
	*y = window->mouse_y;
}

void* wmGetHWND(wm_window_t* window) {
	return window->hwnd;
}
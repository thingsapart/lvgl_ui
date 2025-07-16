#ifndef SDL_VIEWER_H
#define SDL_VIEWER_H

#include "lvgl.h"

// Initialize SDL and LVGL display
// Returns 0 on success, -1 on error
int sdl_viewer_init(void);

// Create a basic LVGL screen to serve as the root for UI elements
// Returns a pointer to the created screen object
lv_obj_t* sdl_viewer_create_main_screen(void);

// Handle SDL events and LVGL tasks for a single frame.
// Returns 1 on quit event, 0 to continue.
int sdl_viewer_tick(void);

// Handle SDL events and LVGL tasks for a specific duration.
// This is used for automated testing to let the UI stabilize.
void sdl_viewer_render_for_time(uint32_t ms_to_run);

// Take a screenshot of the current window content using the SDL renderer.
// path: The full path where the PNG file should be saved.
void sdl_viewer_take_screenshot(const char* path);

// Take a screenshot of the active screen using LVGL's snapshot API.
// This is more reliable for testing as it waits for the draw to complete.
// path: The full path where the PNG file should be saved.
void sdl_viewer_take_snapshot_lvgl(const char* path);

void sdl_viewer_render_for_time_and_snapshot(uint32_t ms_to_run, const char *path);

// Deinitialize SDL and LVGL
void sdl_viewer_deinit(void);

// Simple delay/sleep.
void sdl_viewer_delay(int ms);

#define HIGH_DPI

#endif // SDL_VIEWER_H

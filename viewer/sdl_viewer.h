#ifndef SDL_VIEWER_H
#define SDL_VIEWER_H

#include "lvgl.h"

// Initialize SDL and LVGL display
// Returns 0 on success, -1 on error
int sdl_viewer_init(void);

// Create a basic LVGL screen to serve as the root for UI elements
// Returns a pointer to the created screen object
lv_obj_t* sdl_viewer_create_main_screen(void);

// Handle SDL events and LVGL tasks
// This function should be called in a loop
void sdl_viewer_loop(void);

// Deinitialize SDL and LVGL
void sdl_viewer_deinit(void);

#endif // SDL_VIEWER_H


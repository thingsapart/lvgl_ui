#include "sdl_viewer.h"
#include <SDL2/SDL.h>
#include "lvgl.h"

#include <unistd.h>
#define SDL_MAIN_HANDLED        /*To fix SDL's "undefined reference to WinMain" issue*/
#include <SDL2/SDL.h>
#include "drivers/sdl/lv_sdl_mouse.h"
#include "drivers/sdl/lv_sdl_mousewheel.h"
#include "drivers/sdl/lv_sdl_keyboard.h"

static lv_display_t *lvDisplay;
static lv_indev_t *lvMouse;
static lv_indev_t *lvMouseWheel;
static lv_indev_t *lvKeyboard;

#if LV_USE_LOG != 0
static void lv_log_print_g_cb(lv_log_level_t level, const char * buf) {
    LV_UNUSED(level);
    LV_UNUSED(buf);
}
#endif

// #define HIGH_DPI

#define DEFAULT_WIDTH 1024
#define DEFAULT_HEIGHT 480

void check_dpi(lv_display_t *disp) {
    int rw = 0, rh = 0;
    SDL_GetRendererOutputSize(lv_sdl_window_get_renderer(disp), &rw, &rh);
    if(rw != DEFAULT_WIDTH) {
        float widthScale = (float)rw / (float) DEFAULT_WIDTH;
        float heightScale = (float)rh / (float) DEFAULT_HEIGHT;

        if(widthScale != heightScale) {
            fprintf(stderr, "WARNING: width scale != height scale\n");
        }

        SDL_RenderSetScale(lv_sdl_window_get_renderer(disp), widthScale, heightScale);
    }
}

int sdl_viewer_init(void) {
    /* initialize lvgl */
    lv_init();

    // Workaround for sdl2 `-m32` crash
    // https://bugs.launchpad.net/ubuntu/+source/libsdl2/+bug/1775067/comments/7
    #ifndef WIN32
        setenv("DBUS_FATAL_WARNINGS", "0", 1);
    #endif

    /* Register the log print callback */
    #if LV_USE_LOG != 0
    lv_log_register_print_cb(lv_log_print_g_cb);
    #endif

    /* Add a display
    * Use the 'monitor' driver which creates window on PC's monitor to simulate a display*/

#ifndef HIGH_DPI
    lvDisplay = lv_sdl_window_create(DEFAULT_WIDTH, DEFAULT_HEIGHT);
#else
    lvDisplay = lv_sdl_window_create(DEFAULT_WIDTH * 2, DEFAULT_HEIGHT * 2);
    lv_sdl_window_set_zoom(lvDisplay, 2.0);
#endif
    
    if (!lvDisplay) {
        // Handle error, perhaps log and return -1
        return -1;
    }
    lvMouse = lv_sdl_mouse_create();
    lvMouseWheel = lv_sdl_mousewheel_create();
    lvKeyboard = lv_sdl_keyboard_create();

    return 0; // Success
}

lv_obj_t* sdl_viewer_create_main_screen(void) {
    /* create Widgets on the screen */
    lv_obj_t *screen = lv_scr_act();
    return screen;
}

void sdl_viewer_loop(void) {
    Uint32 lastTick = SDL_GetTicks();
    while(1) {
        SDL_Delay(5);
        Uint32 current = SDL_GetTicks();
        lv_tick_inc(current - lastTick); // Update the tick timer. Tick is new for LVGL 9
        lastTick = current;
        lv_timer_handler(); // Update the UI-
    }

    // This part is unreachable due to the infinite loop, but if it were, it shouldn't return a value.
    // return 0;
}

void sdl_viewer_deinit(void) {
  // Nothing to do here, SDL cleans up after itself when done.
}

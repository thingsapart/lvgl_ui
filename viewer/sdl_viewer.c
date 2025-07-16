#include "sdl_viewer.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include "lvgl.h"
#include "libs/lodepng.h" // For saving snapshots as PNG
#include "lvgl_renderer.h"

#include <unistd.h>
#include <sys/stat.h>
#define SDL_MAIN_HANDLED        /*To fix SDL's "undefined reference to WinMain" issue*/
#include <SDL2/SDL.h>
#include "drivers/sdl/lv_sdl_mouse.h"
#include "drivers/sdl/lv_sdl_mousewheel.h"
#include "drivers/sdl/lv_sdl_keyboard.h"

static lv_display_t *lvDisplay;
static lv_indev_t *lvMouse;
static lv_indev_t *lvMouseWheel;
static lv_indev_t *lvKeyboard;
static SDL_Window* window = NULL;
static SDL_Renderer* renderer = NULL;

#if LV_USE_LOG != 0
static void lv_log_print_g_cb(lv_log_level_t level, const char * buf) {
    LV_UNUSED(level);
    LV_UNUSED(buf);
}
#endif

// #define HIGH_DPI

#define DEFAULT_WIDTH 1024
#define DEFAULT_HEIGHT 480

/**
 * @brief Converts a buffer from LVGL's ARGB8888 to LodePNG's expected RGBA8888.
 * This is an in-place conversion, swapping the R and B channels.
 * @param buf The pixel buffer to convert.
 * @param width The width of the image in pixels.
 * @param height The height of the image in pixels.
 */
static void _convert_argb8888_to_rgba8888(uint8_t* buf, uint32_t width, uint32_t height) {
    if (!buf) return;
    uint32_t pixel_count = width * height;
    for (uint32_t i = 0; i < pixel_count; ++i) {
        uint8_t* p = &buf[i * 4];
        // LVGL's ARGB8888 is stored as [B, G, R, A] in memory (little-endian).
        // LodePNG's expected RGBA8888 is [R, G, B, A].
        // We just need to swap the R and B channels.
        uint8_t temp_b = p[0];
        p[0] = p[2]; // Move R to the first byte
        p[2] = temp_b; // Move B to the third byte
    }
}


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

    // Store window and renderer for screenshot function
    renderer = lv_sdl_window_get_renderer(lvDisplay);
    window = SDL_RenderGetWindow(renderer);

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
        lv_timer_handler();
     }
}

static long get_file_mod_time(const char* path) {
    struct stat attr;
    if (stat(path, &attr) == 0) {
        return attr.st_mtime;
    }
    return -1;
}

static bool quit_flag = false;

static void close_event_cb(lv_event_t * e) {
    // The main screen's delete event is a good proxy for the window being closed.
    // lv_timer_handler -> sdl_event_handler -> lv_sdl_quit_request=true -> lv_display_delete -> LV_EVENT_DELETE
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_DELETE) {
        quit_flag = true;
    }
}

void sdl_viewer_loop_watch_mode(const char* ui_spec_path, ApiSpec* api_spec, lv_obj_t* preview_panel, lv_obj_t* inspector_panel) {
    quit_flag = false;

    // Attach a callback to the screen to detect when the window is closed
    lv_obj_add_event_cb(lv_screen_active(), close_event_cb, LV_EVENT_DELETE, NULL);

    // Initial load
    lvgl_renderer_reload_ui(ui_spec_path, api_spec, preview_panel, inspector_panel);
    long last_mod_time = get_file_mod_time(ui_spec_path);
    Uint32 lastTick = SDL_GetTicks();
    int timer_counter = 0;

    while (!quit_flag) {
        // Check for file changes every 200ms
        if (++timer_counter % 40 == 0) {
            long current_mod_time = get_file_mod_time(ui_spec_path);
            if (current_mod_time != -1 && current_mod_time != last_mod_time) {
                last_mod_time = current_mod_time;
                lvgl_renderer_reload_ui(ui_spec_path, api_spec, preview_panel, inspector_panel);
            }
        }

        // Standard LVGL/SDL loop
        SDL_Delay(5);
        Uint32 current = SDL_GetTicks();
        lv_tick_inc(current - lastTick);
        lastTick = current;
        lv_timer_handler();
    }
}


/**
void sdl_viewer_loop(void) {
    Uint32 lastTick = SDL_GetTicks();
    while(1) {
        SDL_Event e;
        while(SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) {
                return; // Exit loop if window is closed
            }
        }
        
        Uint32 current = SDL_GetTicks();
        uint32_t elapsed = current - lastTick;
        if (elapsed < 5) {
             SDL_Delay(5 - elapsed);
             current = SDL_GetTicks();
        }
        
        lv_tick_inc(current - lastTick);
        lastTick = current;
        lv_timer_handler();
    }
}
*/

void sdl_viewer_render_for_time(uint32_t ms_to_run) {
    uint32_t start_tick = SDL_GetTicks();
    uint32_t last_tick = start_tick;
    while (SDL_GetTicks() - start_tick < ms_to_run) {
        uint32_t current_tick = SDL_GetTicks();
        lv_tick_inc(current_tick - last_tick);
        last_tick = current_tick;
        lv_timer_handler();
        SDL_Delay(5); // Yield to prevent 100% CPU usage
    }
}

void sdl_viewer_take_screenshot(const char* path) {
    if (!renderer || !window) return;

    int w, h;
    SDL_GetWindowSize(window, &w, &h);

    SDL_Surface* sshot = SDL_CreateRGBSurface(0, w, h, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0xff000000);
    if (!sshot) {
        fprintf(stderr, "SDL_CreateRGBSurface failed: %s\n", SDL_GetError());
        return;
    }
    
    SDL_RenderReadPixels(renderer, NULL, sshot->format->format, sshot->pixels, sshot->pitch);
    
    unsigned error = lodepng_encode32_file(path, (const unsigned char*)sshot->pixels, w, h);
    if (error) {
        fprintf(stderr, "lodepng_encode32_file failed with error %u: %s\n", error, lodepng_error_text(error));
    }
    
    SDL_FreeSurface(sshot);
}

void sdl_viewer_take_snapshot_lvgl(const char* path) {
    // 1. Force a synchronous redraw of the screen to ensure all drawing is complete.
    lv_refr_now(lv_display_get_default());
    
    lv_obj_t* screen = lv_screen_active();
    if (!screen) {
        fprintf(stderr, "Cannot take snapshot: no active screen.\n");
        return;
    }

    // 2. Get screen dimensions and calculate buffer size.
    int32_t width = lv_obj_get_width(screen);
    int32_t height = lv_obj_get_height(screen);
    uint32_t bpp = lv_color_format_get_bpp(LV_COLOR_FORMAT_ARGB8888);
    size_t buf_size = width * height * (bpp / 8);

    // 3. Allocate the buffer using standard malloc, bypassing LVGL's memory manager.
    void* buf_data = malloc(buf_size);
    if (!buf_data) {
        fprintf(stderr, "Error: Failed to malloc snapshot buffer of size %zu\n", buf_size);
        return;
    }

    // 4. Initialize a draw buffer descriptor on the stack to wrap our malloc'd buffer.
    lv_draw_buf_t draw_buf;
    lv_draw_buf_init(&draw_buf, width, height, LV_COLOR_FORMAT_ARGB8888, width * (bpp / 8), buf_data, buf_size);

    // 5. Take the snapshot into our manually allocated buffer.
    lv_result_t res = lv_snapshot_take_to_draw_buf(screen, LV_COLOR_FORMAT_ARGB8888, &draw_buf);
    
    if (res != LV_RESULT_OK) {
        fprintf(stderr, "lv_snapshot_take_to_draw_buf failed with code %d\n", res);
        free(buf_data); // Clean up our buffer on failure
        return;
    }

    // 6. Convert the pixel format from LVGL's ARGB to LodePNG's expected RGBA.
    _convert_argb8888_to_rgba8888((uint8_t*)draw_buf.data, width, height);

    // 7. Save the corrected raw RGBA buffer to a PNG file using lodepng.
    unsigned error = lodepng_encode32_file(
        path,
        (const unsigned char*)draw_buf.data,
        width,
        height
    );

    if (error) {
        fprintf(stderr, "lodepng_encode32_file failed with error %u: %s\n", error, lodepng_error_text(error));
    }
    
    // 8. Clean up the malloc'd buffer.
    free(buf_data);
}


void sdl_viewer_deinit(void) {
  // Nothing to do here, SDL cleans up after itself when done.
}

#include <stdio.h>
#include <stdlib.h>
#include "lvgl.h"
#include "viewer/sdl_viewer.h"
#include "c_gen/create_ui.h"    // The generated UI
#include "c_gen/data_binding.h" // The data binding library
#include "cnc_app.h"            // Our CNC application logic

static void tick_timer_cb(lv_timer_t* timer) {
    (void)timer;
    // Periodically call our application's tick function
    cnc_app_tick();
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    printf("Starting Native CNC Application...\n");

    // --- LVGL and SDL Initialization ---
    if (sdl_viewer_init() != 0) {
        fprintf(stderr, "FATAL: Failed to initialize SDL viewer.\n");
        return 1;
    }

    lv_obj_t* screen = sdl_viewer_create_main_screen();
    if (!screen) {
        fprintf(stderr, "FATAL: Failed to create main screen.\n");
        sdl_viewer_deinit();
        return 1;
    }

    // --- Application Initialization ---
    // Initialize the data binding system first
    data_binding_init();
    // Initialize our CNC application logic, which registers its action handler
    cnc_app_init();
    
    // --- UI Creation ---
    // This function is generated from our ui.yaml by the c_code backend
    create_ui(screen);

    // --- Timers and Main Loop ---
    // Create a timer to drive the CNC simulation
    lv_timer_create(tick_timer_cb, 50, NULL);
    
    printf("Starting main loop. Close the window to exit.\n");
    sdl_viewer_loop();

    // The sdl_viewer_loop is infinite, so code below is for completeness.
    sdl_viewer_deinit();
    return 0;
}

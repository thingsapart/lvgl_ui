#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cJSON.h>

#include "utils.h"
#include "api_spec.h"
#include "generator.h"
#include "ir.h"
#include "debug_log.h"
#include "viewer/sdl_viewer.h"
#include "data_binding.h"
#include "lvgl_renderer.h"
#include "registry.h"
#include "yaml_parser.h"

#include <SDL2/SDL.h>


#include "cnc_app.h"

// --- Global Configuration ---
bool g_strict_mode = false;
bool g_strict_registry_mode = false;

void render_abort(const char *msg) {
    fprintf(stderr, ANSI_BOLD_RED "\nFATAL ERROR: %s\n\n" ANSI_RESET, msg);
    fflush(stderr);
    exit(1);
}

int main(int argc, char* argv[]) {
    // --- Initialize Base Systems ---
    if (sdl_viewer_init() != 0) {
        render_abort("Failed to initialize SDL viewer.");
    }
    cnc_app_init();
    
    data_binding_init();
    data_binding_register_action_handler(cnc_action_handler);

    lv_obj_t* screen = sdl_viewer_create_main_screen();

#if defined(CNC_LIVE_RENDER_MODE)
    // --- Option A: Live Rendering Mode ---
    printf("--- Running CNC Example in LIVE RENDER mode ---\n");
    debug_log_init();

    char* api_spec_content = read_file("api_spec.json");
    cJSON* api_spec_json = cJSON_Parse(api_spec_content);
    ApiSpec* api_spec = api_spec_parse(api_spec_json);

    char* ui_spec_content = read_file("ex_cnc/cnc_ui.yaml");
    char* error_msg = NULL;
    cJSON* ui_spec_json = yaml_to_cjson(ui_spec_content, &error_msg);
     if (error_msg) render_abort(error_msg);
    
    IRRoot* ir_root = generate_ir_from_ui_spec(ui_spec_json, api_spec);
    Registry* registry = registry_create();
    lvgl_render_backend(ir_root, api_spec, screen, registry);

#elif defined(CNC_STATIC_BUILD_MODE)
    // --- Option B: Static Compiled Mode ---
    printf("--- Running CNC Example in STATIC COMPILED mode ---\n");
    // This function is declared because it's in a separate, generated C file.

    create_ui(screen);
#else
    #error "No build mode defined. Please compile with -DCNC_LIVE_RENDER_MODE or -DCNC_STATIC_BUILD_MODE"
#endif

    // --- Main Simulation Loop (common to both modes) ---
    printf("Starting CNC simulation. Close the window to exit.\n");
    while(1) {
        uint32_t start_tick = lv_tick_get();
        
        lv_timer_handler();
        cnc_app_tick();

        uint32_t time_elapsed = lv_tick_elaps(start_tick);
        if(time_elapsed < 20) {
            SDL_Delay(20 - time_elapsed);
        }
    }

    return 0;
}

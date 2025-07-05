#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

#include "utils.h"
#include "api_spec.h"
#include "generator.h"
#include "ir.h"
#include "debug_log.h"
#include "lvgl_renderer.h"
#include "viewer/sdl_viewer.h"
#include "viewer/view_inspector.h"
#include "c_gen/lvgl_dispatch.h"
#include "data_binding.h"
#include "yaml_parser.h"
#include "warning_printer.h"
#include "registry.h"
#include "cnc_app.h"

bool g_strict_registry_mode = false;
bool g_strict_mode = false;

static void cnc_tick_timer_cb(lv_timer_t* timer) {
    (void)timer;
    cnc_app_tick();
}

void render_abort(const char *msg) {
    fprintf(stderr, ANSI_BOLD_RED "\nFATAL ERROR: %s\n\n" ANSI_RESET, msg);
    fflush(stderr);
    exit(1);
}

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // --- Resource Declarations ---
    int return_code = 0;
    char* api_spec_content = NULL;
    cJSON* api_spec_json = NULL;
    char* ui_spec_content = NULL;
    cJSON* ui_spec_json = NULL;
    ApiSpec* api_spec = NULL;
    IRRoot* ir_root = NULL;
    Registry* renderer_registry = NULL;

    const char* api_spec_path = "../data/api_spec.json";
    const char* ui_spec_path = "./ui.yaml";

    printf("--- Starting CNC Application (Renderer Mode) ---\n");
    debug_log_init();

    // --- File Loading ---
    api_spec_content = read_file(api_spec_path);
    ui_spec_content = read_file(ui_spec_path);
    if (!api_spec_content || !ui_spec_content) {
        fprintf(stderr, "Error reading spec files.\n");
        return_code = 1;
        goto cleanup;
    }

    // --- JSON/YAML Parsing ---
    api_spec_json = cJSON_Parse(api_spec_content);
    char* error_msg = NULL;
    ui_spec_json = yaml_to_cjson(ui_spec_content, &error_msg);
    if (!api_spec_json || !ui_spec_json || error_msg) {
        fprintf(stderr, "Error parsing spec files. YAML error: %s\n", error_msg ? error_msg : "none");
        free(error_msg);
        return_code = 1;
        goto cleanup;
    }

    // --- IR Generation ---
    api_spec = api_spec_parse(api_spec_json);
    ir_root = generate_ir_from_ui_spec(ui_spec_json, api_spec);
    if (!api_spec || !ir_root) {
        fprintf(stderr, "Failed to generate IR from specs.\n");
        return_code = 1;
        goto cleanup;
    }

    // --- SDL/LVGL and UI Initialization ---
    if (sdl_viewer_init() != 0) {
        fprintf(stderr, "FATAL: Failed to initialize SDL viewer.\n");
        return_code = 1;
        goto cleanup;
    }
    lv_obj_t* screen = sdl_viewer_create_main_screen();
    
    // --- Renderer and App Logic ---
    renderer_registry = registry_create();
    
    // This call initializes the UI, the data_binding module, and the object registry
    lvgl_render_backend(ir_root, api_spec, screen, renderer_registry);
    
    // Initialize our CNC application logic, which registers its action handler
    cnc_app_init();
    
    // Create a timer to drive the CNC simulation
    lv_timer_create(cnc_tick_timer_cb, 50, NULL);
    
    // --- Main Loop ---
    printf("Starting main loop. Close the window to exit.\n");
    sdl_viewer_loop();

cleanup:
    // --- Cleanup ---
    if(renderer_registry) registry_free(renderer_registry);
    if(ir_root) ir_free((IRNode*)ir_root);
    if(api_spec) api_spec_free(api_spec);
    if(api_spec_json) cJSON_Delete(api_spec_json);
    if(api_spec_content) free(api_spec_content);
    
    sdl_viewer_deinit();
    return return_code;
}

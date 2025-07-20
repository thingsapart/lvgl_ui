#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

#include "utils.h"
#include "api_spec.h"
#include "generator.h"
#include "ir.h"
#include "debug_log.h"
#include "ir_printer.h"
#include "ir_debug_printer.h"
#include "c_code_printer.h"
#include "lvgl_renderer.h"
#include "viewer/sdl_viewer.h"
#include "viewer/view_inspector.h"
#include "c_gen/lvgl_dispatch.h"
#include "data_binding.h"
#include "yaml_parser.h"
#include "warning_printer.h"
#include "registry.h" // Include for registry management
#include "ui_sim.h"     // ADDED: For UI-Sim testing

// For getpid() to create unique temporary filenames
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif


// --- Global Configuration ---
bool g_strict_mode = false;
bool g_strict_registry_mode = false;
bool g_ui_sim_trace_enabled = false; // ADDED: For UI-Sim test tracing


// --- Function Declarations ---
void print_usage(const char* prog_name);
int run_sim_test_mode(const char* api_spec_path, const char* ui_spec_path, int num_ticks);
int run_yaml_parse_mode(const char* yaml_path);


// --- Main Application ---

void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s <api_spec.json> <ui_spec.json|yaml> [options]\n", prog_name);
    fprintf(stderr, "Special Modes (override standard usage):\n");
    fprintf(stderr, "  --parse-yaml-to-json <file.yaml>  Parse YAML and print resulting JSON to stdout.\n");
    fprintf(stderr, "  --run-sim-test <ticks> --api-spec <api.json> --ui-spec <ui.yaml> Run UI-Sim test.\n");
    fprintf(stderr, "\nStandard Options:\n");
    fprintf(stderr, "  --codegen <backends>     Comma-separated list of backends (ir_print, c_code, lvgl_render).\n");
    fprintf(stderr, "  --debug_out <modules>    Comma-separated list of debug modules to enable (e.g., 'GENERATOR,RENDERER' or 'ALL').\n");
    fprintf(stderr, "  --strict                 Enable strict mode (fail on warnings).\n");
    fprintf(stderr, "  --strict-registry        Fail only on unresolved registry references.\n");
    fprintf(stderr, "  --screenshot-and-exit <path> For visual testing. Renders UI, saves screenshot, and exits.\n");
    fprintf(stderr, "  --watch                  Enable live-reloading of the UI spec file.\n");
    fprintf(stderr, "  --trace-sim              Enable UI-Sim tracing in normal lvgl_render mode.\n");
}

void render_abort(const char *msg) {
    fprintf(stderr, ANSI_BOLD_RED "\nFATAL ERROR: %s\n\n" ANSI_RESET, msg);
    fflush(stderr);
    exit(1);
}

int run_yaml_parse_mode(const char* yaml_path) {
    char* yaml_content = read_file(yaml_path);
    if (!yaml_content) {
        fprintf(stderr, "Error reading YAML file: %s\n", yaml_path);
        return 1;
    }

    char* error_msg = NULL;
    cJSON* json = yaml_to_cjson(yaml_content, &error_msg);
    free(yaml_content);

    if (error_msg) {
        render_abort(error_msg);
        free(error_msg);
        return 1;
    }

    if (!json) {
        render_abort("YAML parser returned NULL without an error message.");
        return 1;
    }

    char* json_string = cJSON_Print(json);
    printf("%s\n", json_string);
    free(json_string);
    cJSON_Delete(json);

    return 0;
}


int run_sim_test_mode(const char* api_spec_path, const char* ui_spec_path, int num_ticks) {
    g_ui_sim_trace_enabled = true;

    // --- 1. Load API Spec ---
    char* api_spec_content = read_file(api_spec_path);
    if (!api_spec_content) { fprintf(stderr, "Error reading API spec file: %s\n", api_spec_path); return 1; }
    cJSON* api_spec_json = cJSON_Parse(api_spec_content);
    if (!api_spec_json) { fprintf(stderr, "Error parsing API spec JSON: %s\n", cJSON_GetErrorPtr()); free(api_spec_content); return 1; }
    ApiSpec* api_spec = api_spec_parse(api_spec_json);
    if (!api_spec) { fprintf(stderr, "Failed to parse API spec.\n"); cJSON_Delete(api_spec_json); free(api_spec_content); return 1; }

    // --- 2. Load UI Spec & Process UI-Sim block ---
    // This call will populate the g_sim structure
    IRRoot* ir_root = generate_ir_from_file(ui_spec_path, api_spec);
    if (!ir_root) {
        fprintf(stderr, "Aborting due to IR generation failure.\n");
        api_spec_free(api_spec);
        cJSON_Delete(api_spec_json);
        free(api_spec_content);
        return 1;
    }

    // --- 3. Run Simulation ---
    printf("--- UI-Sim Trace Start ---\n");
    ui_sim_start();
    for(int i = 0; i < num_ticks; i++) {
        printf("\n--- TICK %d ---\n", i + 1);
        ui_sim_tick(0.033f);
    }
    printf("\n--- UI-Sim Trace End ---\n");

    // --- 4. Cleanup ---
    ui_sim_init();
    ir_free((IRNode*)ir_root);
    api_spec_free(api_spec);
    cJSON_Delete(api_spec_json);
    free(api_spec_content);
    return 0;
}


int main(int argc, char* argv[]) {
    // --- Resource Declarations for robust cleanup ---
    int return_code = 0;
    char* api_spec_content = NULL;
    cJSON* api_spec_json = NULL;
    ApiSpec* api_spec = NULL;
    IRRoot* ir_root = NULL;
    char* backend_list_copy = NULL;
    Registry* renderer_registry = NULL;

    // --- 1. Argument Parsing for Special Modes ---
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--parse-yaml-to-json") == 0 && i + 1 < argc) {
            return run_yaml_parse_mode(argv[++i]);
        }
        if (strcmp(argv[i], "--run-sim-test") == 0 && i + 1 < argc) {
            int ticks = atoi(argv[++i]);
            const char* sim_api_spec = NULL;
            const char* sim_ui_spec = NULL;
            // Scan for the spec files AFTER the run-sim-test flag.
            for (int j = i + 1; j < argc; ++j) {
                if (strcmp(argv[j], "--api-spec") == 0 && j + 1 < argc) sim_api_spec = argv[++j];
                if (strcmp(argv[j], "--ui-spec") == 0 && j + 1 < argc) sim_ui_spec = argv[++j];
            }
             if (!sim_api_spec || !sim_ui_spec) { print_usage(argv[0]); return 1; }
            return run_sim_test_mode(sim_api_spec, sim_ui_spec, ticks);
        }
    }

    // --- 2. Standard Argument Parsing ---
    const char* api_spec_path = NULL;
    const char* ui_spec_path = NULL;
    const char* codegen_list_str = "ir_print";
    const char* debug_out_str = NULL;
    const char* screenshot_path = NULL;
    bool watch_mode = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--codegen") == 0 && i + 1 < argc) { codegen_list_str = argv[++i]; }
        else if (strcmp(argv[i], "--debug_out") == 0 && i + 1 < argc) { debug_out_str = argv[++i]; }
        else if (strcmp(argv[i], "--screenshot-and-exit") == 0 && i + 1 < argc) { screenshot_path = argv[++i]; }
        else if (strcmp(argv[i], "--strict") == 0) { g_strict_mode = true; }
        else if (strcmp(argv[i], "--strict-registry") == 0) { g_strict_registry_mode = true; }
        else if (strcmp(argv[i], "--watch") == 0) { watch_mode = true; }
        else if (strcmp(argv[i], "--trace-sim") == 0) { g_ui_sim_trace_enabled = true; }
        else if (strcmp(argv[i], "--run-sim-test") == 0) { i++; continue; } // Skip already handled args
        else if (strcmp(argv[i], "--api-spec") == 0) { i++; continue; }   // Skip already handled args
        else if (strcmp(argv[i], "--ui-spec") == 0) { i++; continue; }    // Skip already handled args
        else if (!api_spec_path) { api_spec_path = argv[i]; }
        else if (!ui_spec_path) { ui_spec_path = argv[i]; }
        // else ignore args already handled by special modes
    }

    debug_log_init();
    if (debug_out_str) { debug_log_parse_modules_str(debug_out_str); }
    if (g_strict_mode) { DEBUG_LOG(LOG_MODULE_MAIN, "--- Strict mode enabled ---\n"); }
    if (g_strict_registry_mode && !g_strict_mode) { DEBUG_LOG(LOG_MODULE_MAIN, "--- Strict registry mode enabled ---\n"); }

    if (!api_spec_path || !ui_spec_path) { print_usage(argv[0]); return 1; }

    // --- 3. File Loading & JSON/YAML Parsing ---
    api_spec_content = read_file(api_spec_path);
    if (!api_spec_content) { fprintf(stderr, "Error reading API spec file: %s\n", api_spec_path); return_code = 1; goto cleanup; }
    api_spec_json = cJSON_Parse(api_spec_content);
    if (!api_spec_json) { fprintf(stderr, "Error parsing API spec JSON: %s\n", cJSON_GetErrorPtr()); return_code = 1; goto cleanup; }

    // --- 4. IR Generation ---
    api_spec = api_spec_parse(api_spec_json);
    if (!api_spec) { fprintf(stderr, "Failed to parse the loaded API spec into internal structures.\n"); return_code = 1; goto cleanup; }

    if (!watch_mode) {
        ir_root = generate_ir_from_file(ui_spec_path, api_spec);
        if (!ir_root) { fprintf(stderr, "Aborting due to IR generation failure.\n"); return_code = 1; goto cleanup; }
    }


    // --- 5. Run Code Generation Backends ---
    backend_list_copy = strdup(codegen_list_str);
    if (!backend_list_copy) render_abort("Failed to duplicate codegen string.");

    char* backend_name = strtok(backend_list_copy, ",");
    while (backend_name != NULL) {
        if (strcmp(backend_name, "ir_print") == 0) { ir_print_backend(ir_root, api_spec); }
        else if (strcmp(backend_name, "ir_debug_print") == 0) { ir_debug_print_backend(ir_root, api_spec); }
        else if (strcmp(backend_name, "c_code") == 0) { c_code_print_backend(ir_root, api_spec); }
        else if (strcmp(backend_name, "lvgl_render") == 0) {
            DEBUG_LOG(LOG_MODULE_MAIN, "Executing 'lvgl_render' backend.");

            if (sdl_viewer_init() != 0) { fprintf(stderr, "FATAL: Failed to initialize SDL viewer.\n"); return_code = 1; goto cleanup; }

            lv_obj_t* screen = sdl_viewer_create_main_screen();
            if (!screen) { fprintf(stderr, "FATAL: Failed to create main screen.\n"); sdl_viewer_deinit(); return_code = 1; goto cleanup; }

            lv_obj_t* preview_panel = screen;
            lv_obj_t* inspector_panel = NULL;

            if(!screenshot_path) {
                lv_obj_t* main_container = lv_obj_create(screen);
                lv_obj_set_size(main_container, lv_pct(100), lv_pct(100));
                lv_obj_set_flex_flow(main_container, LV_FLEX_FLOW_ROW);
                lv_obj_center(main_container);
                preview_panel = lv_obj_create(main_container);
                lv_obj_set_width(preview_panel, lv_pct(100));
                lv_obj_set_flex_grow(preview_panel, 1);
                lv_obj_set_height(preview_panel, lv_pct(100));
                lv_obj_set_style_pad_all(preview_panel, 0, 0);
                lv_obj_set_style_border_width(preview_panel, 0, 0);
                inspector_panel = lv_obj_create(main_container);
                lv_obj_set_width(inspector_panel, 350);
                lv_obj_set_height(inspector_panel, lv_pct(100));
                lv_obj_set_style_pad_all(inspector_panel, 0, 0);
                lv_obj_set_style_border_width(inspector_panel, 0, 0);
            }

            if (watch_mode && !screenshot_path) {
                DEBUG_LOG(LOG_MODULE_MAIN, "Starting viewer in watch mode for '%s'. Close window to exit.\n", ui_spec_path);
                sdl_viewer_loop_watch_mode(ui_spec_path, api_spec, preview_panel, inspector_panel);
                DEBUG_LOG(LOG_MODULE_MAIN, "SDL viewer exited from watch mode.\n");
            } else {
                if (inspector_panel) { view_inspector_init(inspector_panel, ir_root, api_spec); }

                renderer_registry = registry_create();
                lvgl_render_backend(ir_root, api_spec, preview_panel, renderer_registry);

                if (screenshot_path) {
                    sdl_viewer_render_for_time(250);
                    sdl_viewer_take_snapshot_lvgl(screenshot_path);
                    DEBUG_LOG(LOG_MODULE_MAIN, "Screenshot saved to '%s'. Exiting.", screenshot_path);
                } else {
                    DEBUG_LOG(LOG_MODULE_MAIN, "Starting SDL viewer loop. Close the window to exit.\n");
                    sdl_viewer_loop();
                    DEBUG_LOG(LOG_MODULE_MAIN, "SDL viewer exited.\n");
                }
            }

            sdl_viewer_deinit();
            obj_registry_deinit();

        } else {
            fprintf(stderr, "Warning: Unknown codegen backend '%s'.\n", backend_name);
        }
        backend_name = strtok(NULL, ",");
    }

    // --- 6. Run Warning Summary Backend (Always runs last) ---
    if (ir_root) {
        warning_print_backend(ir_root);
    }

cleanup:
    // --- 7. Cleanup ---
    if(renderer_registry) registry_free(renderer_registry);
    if(backend_list_copy) free(backend_list_copy);
    if(ir_root) ir_free((IRNode*)ir_root);
    if(api_spec) api_spec_free(api_spec);
    if(api_spec_json) cJSON_Delete(api_spec_json);
    if(api_spec_content) free(api_spec_content);

    return return_code;
}

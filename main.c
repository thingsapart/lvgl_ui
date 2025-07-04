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


// --- Function Declarations ---
void print_usage(const char* prog_name);
static void sample_action_handler(const char* action_name, binding_value_t value);
static lv_timer_t* data_update_timer;
static void data_updater_timer_cb(lv_timer_t* timer);


// --- Main Application ---

void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s <api_spec.json> <ui_spec.json|yaml> [--codegen <backends>] [--debug_out <modules>] [--strict] [--strict-registry]\n", prog_name);
    fprintf(stderr, "  <backends> is a comma-separated list of code generation backends.\n");
    fprintf(stderr, "    Available: ir_print, ir_debug_print, c_code, lvgl_render\n");
    fprintf(stderr, "    Default: ir_print\n");
    fprintf(stderr, "  <modules> is a comma-separated list of debug modules to enable (e.g., 'GENERATOR,RENDERER' or 'ALL').\n");
    fprintf(stderr, "    This is an alternative to the LVGL_DEBUG_MODULES environment variable.\n");
    fprintf(stderr, "  --strict: Enables strict mode. Fails on argument count mismatches and unresolved registry references.\n");
    fprintf(stderr, "  --strict-registry: Fails only on unresolved registry references.\n");
}

void render_abort(const char *msg) {
    fprintf(stderr, ANSI_BOLD_RED "\nFATAL ERROR: %s\n\n" ANSI_RESET, msg);
    fflush(stderr);
    exit(1);
}


int main(int argc, char* argv[]) {
    // --- Resource Declarations for robust cleanup ---
    int return_code = 0;
    char* api_spec_content = NULL;
    cJSON* api_spec_json = NULL;
    char* ui_spec_content = NULL;
    cJSON* ui_spec_json = NULL;
    ApiSpec* api_spec = NULL;
    IRRoot* ir_root = NULL;
    char* backend_list_copy = NULL;
    Registry* renderer_registry = NULL; // Moved registry for renderer here

    // --- 1. Argument Parsing ---
    const char* api_spec_path = NULL;
    const char* ui_spec_path = NULL;
    const char* codegen_list_str = "ir_print"; // Default backend
    const char* debug_out_str = NULL;          // For --debug_out flag

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--codegen") == 0) {
            if (i + 1 < argc) {
                codegen_list_str = argv[++i];
            } else {
                fprintf(stderr, "Error: --codegen option requires an argument.\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "--debug_out") == 0) {
            if (i + 1 < argc) {
                debug_out_str = argv[++i];
            } else {
                fprintf(stderr, "Error: --debug_out option requires an argument.\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (strcmp(argv[i], "--strict") == 0) {
            g_strict_mode = true;
        } else if (strcmp(argv[i], "--strict-registry") == 0) {
            g_strict_registry_mode = true;
        } else if (!api_spec_path) {
            api_spec_path = argv[i];
        } else if (!ui_spec_path) {
            ui_spec_path = argv[i];
        } else {
            fprintf(stderr, "Error: Too many file arguments.\n");
            print_usage(argv[0]);
            return 1;
        }
    }

    // Initialize logging system first from ENV, then from command line
    debug_log_init();
    if (debug_out_str) {
        debug_log_parse_modules_str(debug_out_str);
    }
    if (g_strict_mode) {
        printf("--- Strict mode enabled ---\n");
    }
    if (g_strict_registry_mode && !g_strict_mode) {
        printf("--- Strict registry mode enabled ---\n");
    }

    if (!api_spec_path || !ui_spec_path) {
        print_usage(argv[0]);
        return 1;
    }


    // --- 2. File Loading & JSON/YAML Parsing ---
    api_spec_content = read_file(api_spec_path);
    if (!api_spec_content) {
        fprintf(stderr, "Error reading API spec file: %s\n", api_spec_path);
        return_code = 1;
        goto cleanup;
    }
    api_spec_json = cJSON_Parse(api_spec_content);
    if (!api_spec_json) {
        fprintf(stderr, "Error parsing API spec JSON: %s\n", cJSON_GetErrorPtr());
        return_code = 1;
        goto cleanup;
    }

    ui_spec_content = read_file(ui_spec_path);
    if (!ui_spec_content) {
        fprintf(stderr, "Error reading UI spec file: %s\n", ui_spec_path);
        return_code = 1;
        goto cleanup;
    }

    const char* extension = strrchr(ui_spec_path, '.');
    if (extension && (strcmp(extension, ".yaml") == 0 || strcmp(extension, ".yml") == 0)) {
        printf("YAML file detected: '%s'. Parsing with built-in parser...\n", ui_spec_path);
        char* error_msg = NULL;
        ui_spec_json = yaml_to_cjson(ui_spec_content, &error_msg);

        char *out  = cJSON_Print(ui_spec_json);
        printf("YAML -> JSON:\n\n%s\n\n", out);
        free(out);
        printf("------------------------------\n");

        if (error_msg) {
            fprintf(stderr, "%s\n", error_msg);
            free(error_msg);
            return_code = 1;
            goto cleanup;
        }
    } else {
        // Assume JSON for any other file type
        ui_spec_json = cJSON_Parse(ui_spec_content);
        if (!ui_spec_json) {
            fprintf(stderr, "Error parsing UI spec JSON: %s\n", cJSON_GetErrorPtr());
            return_code = 1;
            goto cleanup;
        }
    }


    // --- 3. IR Generation ---
    api_spec = api_spec_parse(api_spec_json);
    if (!api_spec) {
        fprintf(stderr, "Failed to parse the loaded API spec into internal structures.\n");
        return_code = 1;
        goto cleanup;
    }

    ir_root = generate_ir_from_ui_spec(ui_spec_json, api_spec);
    if (!ir_root) {
        fprintf(stderr, "Failed to generate IR from the UI spec.\n");
        return_code = 1;
        goto cleanup;
    }


    // --- 4. Run Code Generation Backends ---
    backend_list_copy = strdup(codegen_list_str);
    if (!backend_list_copy) render_abort("Failed to duplicate codegen string.");

    char* backend_name = strtok(backend_list_copy, ",");
    while (backend_name != NULL) {
        if (strcmp(backend_name, "ir_print") == 0) {
            ir_print_backend(ir_root, api_spec);
        } else if (strcmp(backend_name, "ir_debug_print") == 0) {
            ir_debug_print_backend(ir_root, api_spec);
        } else if (strcmp(backend_name, "c_code") == 0) {
            c_code_print_backend(ir_root, api_spec);
        } else if (strcmp(backend_name, "lvgl_render") == 0) {
            DEBUG_LOG(LOG_MODULE_MAIN, "Executing 'lvgl_render' backend.");

            if (sdl_viewer_init() != 0) {
                fprintf(stderr, "FATAL: Failed to initialize SDL viewer.\n");
                return_code = 1;
                goto cleanup;
            }

            lv_obj_t* screen = sdl_viewer_create_main_screen();
            if (!screen) {
                fprintf(stderr, "FATAL: Failed to create main screen.\n");
                sdl_viewer_deinit();
                return_code = 1;
                goto cleanup;
            }

            // Create the two-pane layout for preview and inspector
            lv_obj_t* main_container = lv_obj_create(screen);
            lv_obj_set_size(main_container, lv_pct(100), lv_pct(100));
            lv_obj_set_flex_flow(main_container, LV_FLEX_FLOW_ROW);
            lv_obj_center(main_container);

            lv_obj_t* preview_panel = lv_obj_create(main_container);
            lv_obj_set_width(preview_panel, lv_pct(100)); // <-- FIX: Give the panel a base width.
            lv_obj_set_flex_grow(preview_panel, 1);
            lv_obj_set_height(preview_panel, lv_pct(100));
            lv_obj_set_style_pad_all(preview_panel, 0, 0);
            lv_obj_set_style_border_width(preview_panel, 0, 0);

            lv_obj_t* inspector_panel = lv_obj_create(main_container);
            lv_obj_set_width(inspector_panel, 350);
            lv_obj_set_height(inspector_panel, lv_pct(100));
            lv_obj_set_style_pad_all(inspector_panel, 0, 0);
            lv_obj_set_style_border_width(inspector_panel, 0, 0);

            // Init the inspector UI inside its panel, passing the IR root and API spec
            view_inspector_init(inspector_panel, ir_root, api_spec);

            // Create the registry that will live for the duration of the renderer
            renderer_registry = registry_create();
            
            // Render the IR onto the PREVIEW PANEL. This now also inits data_binding.
            lvgl_render_backend(ir_root, api_spec, preview_panel, renderer_registry);
            
            // Register our sample action handler
            data_binding_register_action_handler(sample_action_handler);

            // Create a timer to simulate model changes
            data_update_timer = lv_timer_create(data_updater_timer_cb, 50, NULL);


            // Enter the main rendering loop. This will block until the user closes the window.
            printf("Starting SDL viewer loop. Close the window to exit.\n");
            sdl_viewer_loop();

            // Clean up resources used by the runtime renderer
            obj_registry_deinit();
            sdl_viewer_deinit();

            printf("SDL viewer exited.\n");
        } else {
            fprintf(stderr, "Warning: Unknown codegen backend '%s'.\n", backend_name);
        }
        backend_name = strtok(NULL, ",");
    }

    // --- 4.5. Run Warning Summary Backend (Always runs last) ---
    warning_print_backend(ir_root);

cleanup:
    // --- 5. Cleanup ---
    if(renderer_registry) registry_free(renderer_registry);
    if(backend_list_copy) free(backend_list_copy);
    if(ir_root) ir_free((IRNode*)ir_root);
    if(api_spec) api_spec_free(api_spec);
    if(api_spec_json) cJSON_Delete(api_spec_json);
    if(api_spec_content) free(api_spec_content);

    return return_code;
}


// --- Sample data binding and state management for lvgl_render ---

// 1. A simple "model" for our application state
static struct {
    float x_pos;
    float y_pos;
    bool spindle_on;
    int feed_override;
} g_app_state = { 0.0, 0.0, false, 100 };

// 2. The single action handler function
static void sample_action_handler(const char* action_name, binding_value_t value) {
    printf("Action Received: name='%s', type=%d, ", action_name, value.type);
    switch(value.type) {
        case BINDING_TYPE_INT: printf("value=%d\n", value.as.i_val); break;
        case BINDING_TYPE_BOOL: printf("value=%s\n", value.as.b_val ? "true" : "false"); break;
        case BINDING_TYPE_FLOAT: printf("value=%f\n", value.as.f_val); break;
        default: printf("value=(null)\n"); break;
    }

    if (strcmp(action_name, "spindle_enable") == 0) {
        g_app_state.spindle_on = value.as.b_val;
        // Also notify the UI in case other widgets are observing this state
        data_binding_notify_state_changed("spindle_on", value);
    } 
    else if (strcmp(action_name, "feed_override") == 0) {
        g_app_state.feed_override = value.as.i_val;
        // The button's label is observing this state, so we must notify
        data_binding_notify_state_changed("feed_override", value);
    }
    else if (strcmp(action_name, "position::home") == 0) {
        g_app_state.x_pos = 0.0;
        g_app_state.y_pos = 0.0;
        // The position labels will be updated by the timer callback
    }
}

// 3. A timer to simulate the model changing from an external source
static void data_updater_timer_cb(lv_timer_t* timer) {
    (void)timer; // Unused
    
    // Update state
    g_app_state.x_pos += 0.13f;
    g_app_state.y_pos -= 0.07f;
    if (g_app_state.x_pos > 1000.0) g_app_state.x_pos = -200.0;
    if (g_app_state.y_pos < -1000.0) g_app_state.y_pos = 200.0;
    
    // Notify the UI of the changes
    data_binding_notify_state_changed("position::absolute::x", (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val = g_app_state.x_pos});
    data_binding_notify_state_changed("position::absolute::y", (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val = g_app_state.y_pos});
}

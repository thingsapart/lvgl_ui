#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h> // For strcmp
#include <cJSON.h>

#include "debug_log.h" // For new logging system
#include "utils.h"    // For read_file
#include "api_spec.h" // For ApiSpec & functions
#include "registry.h" // For Registry (though mainly used by generator)
#include "generator.h"  // For generate_ir_from_ui_spec
#include "ir.h"       // For IRRoot, IRNode, ir_free
#include "codegen.h"  // For codegen_generate_c
#include "viewer/sdl_viewer.h" // For SDL viewer functions
#include "lvgl_ui_renderer.h" // For LVGL UI rendering functions
#include "lvgl.h" // For lv_obj_t

typedef enum {
    CODEGEN_MODE_C_CODE,
    CODEGEN_MODE_LVGL_UI,
    CODEGEN_MODE_C_CODE_AND_LVGL_UI
} CodegenMode;

void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s [--codegen <mode>] <api_spec.json> <ui_spec.json>\n", prog_name);
    fprintf(stderr, "Modes: c_code, lvgl_ui, c_code_and_lvgl_ui (default)\n");
}

// Implementation of render_abort
void render_abort(const char *msg) {
    fprintf(stderr, "RENDER ABORT: %s\n", msg);
    fflush(stderr); // Ensure this message is written before exit
    exit(1);
}

int main(int argc, char* argv[]) {
    debug_log_init(); // Initialize the logging system

    CodegenMode mode = CODEGEN_MODE_C_CODE_AND_LVGL_UI; // Default mode
    char* api_spec_path = NULL;
    char* ui_spec_path = NULL;
    int i;

    // Parse arguments
    for (i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--codegen") == 0) {
            if (i + 1 < argc) {
                i++; // Consume mode value
                if (strcmp(argv[i], "c_code") == 0) {
                    mode = CODEGEN_MODE_C_CODE;
                } else if (strcmp(argv[i], "lvgl_ui") == 0) {
                    mode = CODEGEN_MODE_LVGL_UI;
                } else if (strcmp(argv[i], "c_code_and_lvgl_ui") == 0) {
                    mode = CODEGEN_MODE_C_CODE_AND_LVGL_UI;
                } else {
                    fprintf(stderr, "Error: Unknown codegen mode '%s'\n", argv[i]);
                    print_usage(argv[0]);
                    return 1;
                }
            } else {
                fprintf(stderr, "Error: --codegen flag requires a mode argument.\n");
                print_usage(argv[0]);
                return 1;
            }
        } else if (!api_spec_path) {
            api_spec_path = argv[i];
        } else if (!ui_spec_path) {
            ui_spec_path = argv[i];
        } else {
            fprintf(stderr, "Error: Too many positional arguments.\n");
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!api_spec_path || !ui_spec_path) {
        fprintf(stderr, "Error: Missing api_spec.json or ui_spec.json path.\n");
        print_usage(argv[0]);
        return 1;
    }

    // --- Load Files ---
    char* api_spec_str = read_file(api_spec_path);
    if (!api_spec_str) return 1;
    char* ui_spec_str = read_file(ui_spec_path);
    if (!ui_spec_str) { free(api_spec_str); return 1; }

    // --- Parse JSON ---
    cJSON* api_json = cJSON_Parse(api_spec_str);
    if (!api_json) {
        fprintf(stderr, "Error parsing API SPEC JSON: %s\n", cJSON_GetErrorPtr());
        goto cleanup_str;
    }
    cJSON* ui_json = cJSON_Parse(ui_spec_str);
    if (!ui_json) {
        fprintf(stderr, "Error parsing UI SPEC JSON: %s\n", cJSON_GetErrorPtr());
        goto cleanup_json; // api_json needs cleanup
    }

    // --- Process ---
    // 1. Parse the API spec into an internal, efficient format
    ApiSpec* api_spec = api_spec_parse(api_json); // api_spec.c should provide this
    if (!api_spec) {
        fprintf(stderr, "Error: Failed to parse API spec.\n");
        goto cleanup_json; // ui_json and api_json need cleanup
    }

    // 2. Generate the Intermediate Representation (IR) from the UI spec
    // The generator will create and use its own registry internally.
    IRRoot* ir = generate_ir_from_ui_spec(ui_json, api_spec); // generator.c provides this
    DEBUG_LOG(LOG_MODULE_MAIN, "IR generation complete. IR Root: %p", (void*)ir);
    if (ir) {
        DEBUG_LOG(LOG_MODULE_MAIN, "IR Root type: %d", ir->base.type); // Should be IR_NODE_ROOT
        if (ir->base.type == IR_NODE_ROOT) {
            int obj_count = 0;
            IRObject* obj_node = ir->root_objects;
            while(obj_node) {
                obj_count++;
                obj_node = obj_node->next;
            }
            DEBUG_LOG(LOG_MODULE_MAIN, "IR Root contains %d top-level objects.", obj_count);
            if (obj_count == 0) {
                 DEBUG_LOG(LOG_MODULE_MAIN, "Warning: IR Root has no objects!");
            }
        } else {
            DEBUG_LOG(LOG_MODULE_MAIN, "Warning: Generated IR is not of type IR_NODE_ROOT!");
        }
    }
    if (!ir) {
        fprintf(stderr, "Error: Failed to generate IR from UI spec.\n");
        api_spec_free(api_spec); // Free api_spec before goto
        goto cleanup_json; // ui_json and api_json need cleanup
    }

    // 3. Perform actions based on codegen mode
    if (mode == CODEGEN_MODE_C_CODE || mode == CODEGEN_MODE_C_CODE_AND_LVGL_UI) {
        printf("// Code generated by LVGL UI Generator\n\n");
        printf("#include \"lvgl.h\"\n");
        printf("#include \"lvgl_dispatch.h\" // For obj_registry_add\n\n");
        printf("// Forward declaration for any registered C pointers you might have from your application.\n");
        printf("// Example: extern const lv_font_t my_custom_font;\n\n");

        printf("void create_ui(lv_obj_t* parent) {\n");

        // Generate the C code for the UI elements based on the IR
        codegen_generate_c(ir, api_spec);

        printf("}\n");
    }

    if (mode == CODEGEN_MODE_LVGL_UI || mode == CODEGEN_MODE_C_CODE_AND_LVGL_UI) {
        if (sdl_viewer_init() != 0) {
            fprintf(stderr, "Error: Failed to initialize SDL viewer.\n");
            ir_free((IRNode*)ir);
            api_spec_free(api_spec);
            cJSON_Delete(ui_json);
            cJSON_Delete(api_json);
            free(ui_spec_str);
            free(api_spec_str);
            return 1;
        }

        lv_obj_t* main_screen = sdl_viewer_create_main_screen();
        if (!main_screen) {
            fprintf(stderr, "Error: Failed to create main LVGL screen.\n");
            sdl_viewer_deinit(); // Clean up SDL parts that were initialized
            ir_free((IRNode*)ir);
            api_spec_free(api_spec);
            cJSON_Delete(ui_json);
            cJSON_Delete(api_json);
            free(ui_spec_str);
            free(api_spec_str);
            return 1;
        }

        // Pass the api_spec to the renderer. The last argument is for an initial context, which is NULL here.
        render_lvgl_ui_from_ir(ir, main_screen, api_spec, NULL);

        sdl_viewer_loop(); // This is an infinite loop
    }

    // --- Cleanup ---
    ir_free((IRNode*)ir);         // Free the entire IR tree
    api_spec_free(api_spec);      // Free the parsed API spec
cleanup_json:
    cJSON_Delete(ui_json);        // Free UI spec JSON object
    cJSON_Delete(api_json);       // Free API spec JSON object
cleanup_str:
    free(ui_spec_str);            // Free UI spec string
    free(api_spec_str);           // Free API spec string

    if (mode == CODEGEN_MODE_LVGL_UI || mode == CODEGEN_MODE_C_CODE_AND_LVGL_UI) {
         // If sdl_viewer_loop was not infinite, this would be the place for sdl_viewer_deinit()
    }


    return 0;
}

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
#include "c_gen/lvgl_dispatch.h"

// For getpid() to create unique temporary filenames
#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif


// --- Function Declarations ---
void print_usage(const char* prog_name);


// --- Main Application ---

void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s <api_spec.json> <ui_spec.json|yaml> [--codegen <backends>]\n", prog_name);
    fprintf(stderr, "  <backends> is a comma-separated list of code generation backends.\n");
    fprintf(stderr, "  Available backends: ir_print, ir_debug_print, c_code, lvgl_render\n");
    fprintf(stderr, "  If --codegen is not specified, 'ir_print' is run by default.\n");
    fprintf(stderr, "  If a .yaml file is provided, 'yq' must be installed to convert it to JSON.\n");
}

void render_abort(const char *msg) {
    fprintf(stderr, "FATAL ERROR: %s\n", msg);
    fflush(stderr);
    exit(1);
}


int main(int argc, char* argv[]) {
    debug_log_init();

    // --- Resource Declarations for robust cleanup ---
    int return_code = 0;
    char* api_spec_content = NULL;
    cJSON* api_spec_json = NULL;
    char* ui_spec_content = NULL;
    cJSON* ui_spec_json = NULL;
    ApiSpec* api_spec = NULL;
    IRRoot* ir_root = NULL;
    char* backend_list_copy = NULL;
    char temp_json_path[256] = {0}; // Will hold path to temporary JSON file

    // --- 1. Argument Parsing ---
    const char* api_spec_path = NULL;
    const char* ui_spec_path = NULL;
    const char* codegen_list_str = "ir_print"; // Default backend

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--codegen") == 0) {
            if (i + 1 < argc) {
                codegen_list_str = argv[++i];
            } else {
                fprintf(stderr, "Error: --codegen option requires an argument.\n");
                print_usage(argv[0]);
                return 1;
            }
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

    if (!api_spec_path || !ui_spec_path) {
        print_usage(argv[0]);
        return 1;
    }

    // --- 1.5. YAML to JSON conversion (if necessary) ---
    const char* ui_spec_to_load = ui_spec_path;
    const char* extension = strrchr(ui_spec_path, '.');

    if (extension && (strcmp(extension, ".yaml") == 0 || strcmp(extension, ".yml") == 0)) {
        printf("YAML file detected: '%s'. Converting with 'yq'...\n", ui_spec_path);

        snprintf(temp_json_path, sizeof(temp_json_path), "__temp_ui_%d.json", getpid());

        char command[1024];
        snprintf(command, sizeof(command), "yq e -o json \"%s\" > \"%s\"", ui_spec_path, temp_json_path);

        printf("Executing: %s\n", command);
        int ret = system(command);

        if (ret != 0) {
            fprintf(stderr, "Error: 'yq' command failed. Is 'yq' installed and in your PATH?\n");
            remove(temp_json_path); // Attempt to clean up potentially empty/partial file
            return_code = 1;
            goto cleanup;
        }
        ui_spec_to_load = temp_json_path;
    }


    // --- 2. File Loading & JSON Parsing ---
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

    ui_spec_content = read_file(ui_spec_to_load);
    if (!ui_spec_content) {
        fprintf(stderr, "Error reading UI spec file: %s\n", ui_spec_to_load);
        return_code = 1;
        goto cleanup;
    }

    // If a temporary file was used for YAML conversion, it has now been read into memory.
    // We can delete the file immediately.
    if (temp_json_path[0] != '\0') {
        printf("Cleaning up temporary file: %s\n", temp_json_path);
        remove(temp_json_path);
        // Mark the path as handled so the final cleanup step doesn't try to remove it again
        // in case of a later error.
        temp_json_path[0] = '\0';
    }

    ui_spec_json = cJSON_Parse(ui_spec_content);
    if (!ui_spec_json) {
        fprintf(stderr, "Error parsing UI spec JSON: %s\n", cJSON_GetErrorPtr());
        return_code = 1;
        goto cleanup;
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

            lv_obj_t* parent_screen = sdl_viewer_create_main_screen();
            if (!parent_screen) {
                fprintf(stderr, "FATAL: Failed to create main screen.\n");
                sdl_viewer_deinit();
                return_code = 1;
                goto cleanup;
            }

            // Render the IR onto the created screen.
            lvgl_render_backend(ir_root, api_spec, parent_screen);

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

cleanup:
    // --- 5. Cleanup ---
    if(backend_list_copy) free(backend_list_copy);
    if(ir_root) ir_free((IRNode*)ir_root);
    if(api_spec) api_spec_free(api_spec);
    if(api_spec_json) cJSON_Delete(api_spec_json);
    if(ui_spec_json) cJSON_Delete(ui_spec_json);
    if(api_spec_content) free(api_spec_content);
    if(ui_spec_content) free(ui_spec_content);

    // Clean up temporary file if it was created but not handled yet (e.g., due to an early error)
    if (temp_json_path[0] != '\0') {
        printf("Cleaning up temporary file (final check): %s\n", temp_json_path);
        remove(temp_json_path);
    }

    return return_code;
}

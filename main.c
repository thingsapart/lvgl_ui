#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cJSON.h>

#include "utils.h"
#include "debug_log.h"
#include "api_spec.h"
#include "generator.h"
#include "ir.h"

// Include headers for available backends
#include "ir_printer.h"
#include "ir_debug_printer.h"


void print_usage(const char* prog_name) {
    fprintf(stderr, "Usage: %s <api_spec.json> <ui_spec.json> [--codegen <backends>]\n", prog_name);
    fprintf(stderr, "  <backends> is a comma-separated list of code generation backends.\n");
    fprintf(stderr, "  Available backends: ir_print, ir_debug_print\n");
    fprintf(stderr, "  If --codegen is not specified, 'ir_print' is run by default.\n");
}

void render_abort(const char *msg) {
    fprintf(stderr, "FATAL ERROR: %s\n", msg);
    fflush(stderr);
    exit(1);
}

int main(int argc, char* argv[]) {
    debug_log_init();

    const char* api_spec_path = NULL;
    const char* ui_spec_path = NULL;
    const char* codegen_list_str = "ir_print"; // Default backend

    // --- 1. Argument Parsing ---
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
            fprintf(stderr, "Error: Unexpected argument '%s'.\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!api_spec_path || !ui_spec_path) {
        fprintf(stderr, "Error: Missing required API spec and UI spec file paths.\n");
        print_usage(argv[0]);
        return 1;
    }

    DEBUG_LOG(LOG_MODULE_MAIN, "API Spec Path: %s", api_spec_path);
    DEBUG_LOG(LOG_MODULE_MAIN, "UI Spec Path: %s", ui_spec_path);
    DEBUG_LOG(LOG_MODULE_MAIN, "Codegen Backends: %s", codegen_list_str);

    // --- 2. File Loading and Parsing ---
    char* api_spec_content = read_file(api_spec_path);
    if (!api_spec_content) render_abort("Could not read API spec file.");
    cJSON* api_json = cJSON_Parse(api_spec_content);
    free(api_spec_content);
    if (!api_json) render_abort("Failed to parse API spec JSON.");

    char* ui_spec_content = read_file(ui_spec_path);
    if (!ui_spec_content) {
        cJSON_Delete(api_json);
        render_abort("Could not read UI spec file.");
    }
    cJSON* ui_json = cJSON_Parse(ui_spec_content);
    free(ui_spec_content);
    if (!ui_json) {
        cJSON_Delete(api_json);
        render_abort("Failed to parse UI spec JSON.");
    }

    // --- 3. IR Generation ---
    DEBUG_LOG(LOG_MODULE_MAIN, "Parsing API spec...");
    ApiSpec* api_spec = api_spec_parse(api_json);
    if (!api_spec) {
        cJSON_Delete(api_json);
        cJSON_Delete(ui_json);
        render_abort("Failed to process the parsed API spec.");
    }

    DEBUG_LOG(LOG_MODULE_MAIN, "Generating IR from UI spec...");
    IRRoot* ir_root = generate_ir_from_ui_spec(ui_json, api_spec);
    if (!ir_root) {
        api_spec_free(api_spec);
        cJSON_Delete(api_json);
        cJSON_Delete(ui_json);
        render_abort("Failed to generate IR from UI spec.");
    }

    // --- 4. Backend Dispatch ---
    char* codegen_list_copy = strdup(codegen_list_str);
    if (!codegen_list_copy) render_abort("strdup failed for codegen list.");

    char* token = strtok(codegen_list_copy, ",");
    while (token) {
        if (strcmp(token, "ir_print") == 0) {
            printf("\n--- Running IR Print Backend ---\n");
            ir_print_backend(ir_root, api_spec);
            printf("--- End IR Print Backend ---\n\n");
        } else if (strcmp(token, "ir_debug_print") == 0) {
            printf("\n--- Running IR Debug Print Backend ---\n");
            ir_debug_print_backend(ir_root, api_spec);
            printf("--- End IR Debug Print Backend ---\n\n");
        } else {
            fprintf(stderr, "Warning: Unknown codegen backend '%s'. Skipping.\n", token);
        }
        token = strtok(NULL, ",");
    }
    free(codegen_list_copy);

    // --- 5. Cleanup ---
    DEBUG_LOG(LOG_MODULE_MAIN, "Cleaning up resources.");
    ir_free((IRNode*)ir_root);
    api_spec_free(api_spec);
    cJSON_Delete(api_json);
    cJSON_Delete(ui_json);

    DEBUG_LOG(LOG_MODULE_MAIN, "Execution finished successfully.");
    return 0;
}

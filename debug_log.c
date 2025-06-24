#include "debug_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h> // For va_list, va_start, va_end
#include <ctype.h>   // For isspace
#include <strings.h> // For strcasecmp

// Array to keep track of enabled modules. Initialized to false.
static bool enabled_modules[LOG_MODULE_COUNT] = {false};

// String names for modules, corresponding to the DebugLogModule enum
static const char* module_names[LOG_MODULE_COUNT] = {
    "NONE",      // LOG_MODULE_NONE
    "MAIN",      // LOG_MODULE_MAIN
    "APISPEC",   // LOG_MODULE_API_SPEC
    "IR",        // LOG_MODULE_IR
    "REGISTRY",  // LOG_MODULE_REGISTRY
    "GENERATOR", // LOG_MODULE_GENERATOR
    "CODEGEN",   // LOG_MODULE_CODEGEN
    "RENDERER",  // LOG_MODULE_RENDERER
    "DISPATCH",  // LOG_MODULE_DISPATCH
    "UTILS",     // LOG_MODULE_UTILS
    "SDLVIEWER", // LOG_MODULE_SDL_VIEWER
    "COUNT"      // LOG_MODULE_COUNT - Placeholder, should not be used as a module name
};

// Helper function to get the string name of a module
const char* debug_log_get_module_name(DebugLogModule module) {
    if (module > LOG_MODULE_NONE && module < LOG_MODULE_COUNT) {
        return module_names[module];
    }
    return "UNKNOWN";
}

void debug_log_init(void) {
    // Ensure all modules are disabled initially
    for (int i = 0; i < LOG_MODULE_COUNT; ++i) {
        enabled_modules[i] = false;
    }

    const char* env_var_value = getenv("LVGL_DEBUG_MODULES");
    if (env_var_value) {
        char* env_val_copy = strdup(env_var_value);
        if (!env_val_copy) {
            perror("debug_log_init: strdup failed");
            return;
        }

        char* mutable_copy = env_val_copy; // strtok_r needs a non-const char*
        char* token;
        char* saveptr; // For strtok_r

        token = strtok_r(mutable_copy, ",", &saveptr);
        while (token) {
            // Trim leading whitespace
            while (isspace((unsigned char)*token)) {
                token++;
            }
            // Trim trailing whitespace
            char *end = token + strlen(token) - 1;
            while (end > token && isspace((unsigned char)*end)) {
                end--;
            }
            *(end + 1) = '\0';

            if (strcasecmp(token, "ALL") == 0) {
                for (int i = LOG_MODULE_MAIN; i < LOG_MODULE_COUNT; ++i) {
                    enabled_modules[i] = true;
                }
                fprintf(stderr, "[DEBUG_LOG] All debug modules enabled via LVGL_DEBUG_MODULES=ALL.\n");
                break; // "ALL" overrides specific modules
            }

            bool found = false;
            for (int i = LOG_MODULE_MAIN; i < LOG_MODULE_COUNT; ++i) {
                if (module_names[i] && strcasecmp(token, module_names[i]) == 0) {
                    enabled_modules[i] = true;
                    found = true;
                    fprintf(stderr, "[DEBUG_LOG] Enabled debug module: %s\n", module_names[i]);
                    break;
                }
            }
            if (!found && strlen(token) > 0) {
                 fprintf(stderr, "[DEBUG_LOG] Unknown debug module specified in LVGL_DEBUG_MODULES: %s\n", token);
            }
            token = strtok_r(NULL, ",", &saveptr);
        }
        free(env_val_copy);
    } else {
        fprintf(stderr, "[DEBUG_LOG] LVGL_DEBUG_MODULES not set. All module logging disabled by default.\n");
    }
}

void debug_log_enable_module(DebugLogModule module) {
    if (module > LOG_MODULE_NONE && module < LOG_MODULE_COUNT) {
        enabled_modules[module] = true;
    }
}

void debug_log_disable_module(DebugLogModule module) {
    if (module > LOG_MODULE_NONE && module < LOG_MODULE_COUNT) {
        enabled_modules[module] = false;
    }
}

bool debug_log_is_module_enabled(DebugLogModule module) {
    if (module > LOG_MODULE_NONE && module < LOG_MODULE_COUNT) {
        return enabled_modules[module];
    }
    return false;
}

void _debug_log_print(DebugLogModule module, const char *file, int line, const char *func, const char *format, ...) {
    const char *filename = strrchr(file, '/');
    if (filename) {
        filename++;
    } else {
        filename = file;
    }

    fprintf(stderr, "[%s] %s:%d:%s(): ", debug_log_get_module_name(module), filename, line, func);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);
}

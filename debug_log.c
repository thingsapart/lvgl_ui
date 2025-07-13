#include "debug_log.h"
#include <string.h>
#include <stdlib.h>
#include <stdarg.h> // For va_list, va_start, va_end
#include <ctype.h>   // For isspace
#include <strings.h> // For strcasecmp

// Array to keep track of enabled modules. Initialized to false.

static bool enabled_modules[LOG_MODULE_COUNT] = {
#ifdef __DEV_MODE__
  true,        // LOG_MODULE_NONE
  false,       // LOG_MODULE_MAIN
  false,       // LOG_MODULE_API_SPEC
  true,        // LOG_MODULE_IR
  false,       // LOG_MODULE_REGISTRY
  true,        // LOG_MODULE_GENERATOR
  true,        // LOG_MODULE_CODEGEN
  true,        // LOG_MODULE_RENDERER
  true,        // LOG_MODULE_DISPATCH
  true,        // LOG_MODULE_UTILS
  true,        // LOG_MODULE_SDL_VIEWER
  true,        // LOG_MODULE_DATABINDING
#else
  false
#endif
};

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
    "DATABINDING", // LOG_MODULE_DATABINDING
};

// Helper function to get the string name of a module
const char* debug_log_get_module_name(DebugLogModule module) {
    if (module > LOG_MODULE_NONE && module < LOG_MODULE_COUNT) {
        return module_names[module];
    }
    return "UNKNOWN";
}

void debug_log_parse_modules_str(const char* modules_str) {
    if (!modules_str) {
        return;
    }

    char* str_copy = strdup(modules_str);
    if (!str_copy) {
        perror("debug_log_parse_modules_str: strdup failed");
        return;
    }

    char* mutable_copy = str_copy; // strtok_r needs a non-const char*
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
            fprintf(stderr, "[DEBUG_LOG] All debug modules enabled.\n");
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
             fprintf(stderr, "[DEBUG_LOG] Unknown debug module specified: '%s'\n", token);
        }
        token = strtok_r(NULL, ",", &saveptr);
    }
    free(str_copy);
}

void debug_log_init(void) {
    // The initial state of enabled modules is defined by the static array at the top.
    // This function now primarily handles configuration from the environment.
    const char* env_var_value = getenv("LVGL_DEBUG_MODULES");
    if (env_var_value) {
        debug_log_parse_modules_str(env_var_value);
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

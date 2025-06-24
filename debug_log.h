#ifndef DEBUG_LOG_H
#define DEBUG_LOG_H

#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>   // For isspace
#include <strings.h> // For strcasecmp
#include <string.h>  // For strdup, strtok, strrchr, strlen

// Enum for different modules
typedef enum {
    LOG_MODULE_NONE = 0, // Should not be used for logging directly
    LOG_MODULE_MAIN,
    LOG_MODULE_API_SPEC,
    LOG_MODULE_IR,
    LOG_MODULE_REGISTRY,
    LOG_MODULE_GENERATOR,
    LOG_MODULE_CODEGEN,
    LOG_MODULE_RENDERER,
    LOG_MODULE_DISPATCH,
    LOG_MODULE_UTILS,
    LOG_MODULE_SDL_VIEWER,
    // Add new modules above this line
    LOG_MODULE_COUNT     // Keep this last for array sizing and iteration bounds
} DebugLogModule;

// Function to initialize the logging system
// Reads LVGL_DEBUG_MODULES environment variable (comma-separated module names, e.g., "CODEGEN,RENDERER", or "ALL")
// If not set, all logging is disabled by default.
void debug_log_init(void);

// Function to enable logging for a specific module
void debug_log_enable_module(DebugLogModule module);

// Function to disable logging for a specific module
void debug_log_disable_module(DebugLogModule module);

// Function to check if a module is enabled
bool debug_log_is_module_enabled(DebugLogModule module);

// The core logging function (implementation detail, typically not called directly by user code)
void _debug_log_print(DebugLogModule module, const char *file, int line, const char *func, const char *format, ...);

// Macro for logging
// Example: DEBUG_LOG(LOG_MODULE_GENERATOR, "Processing item %d", my_item);
#define DEBUG_LOG(module, format, ...) \
    do { \
        if (debug_log_is_module_enabled(module)) { \
            _debug_log_print(module, __FILE__, __LINE__, __func__, format, ##__VA_ARGS__); \
        } \
    } while (0)

#endif // DEBUG_LOG_H

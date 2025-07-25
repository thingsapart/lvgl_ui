/*
 * AUTO-GENERATED by generate_dynamic_lvgl_dispatch.py.
 * DO NOT EDIT MANUALLY.
 */
#ifndef LVGL_DISPATCH_H
#define LVGL_DISPATCH_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

// Forward declarations for required structs
struct IRNode;
struct ApiSpec;

// --- Value Representation for Dynamic Dispatch ---
// A struct to hold a concrete C value from a dispatched function call.
typedef enum {
    RENDER_VAL_TYPE_NULL,
    RENDER_VAL_TYPE_INT,
    RENDER_VAL_TYPE_POINTER,
    RENDER_VAL_TYPE_STRING,
    RENDER_VAL_TYPE_COLOR,
    RENDER_VAL_TYPE_BOOL
} RenderValueType;

typedef struct {
    RenderValueType type;
    union {
        intptr_t i_val;
        void* p_val;
        const char* s_val;
        lv_color_t color_val;
        bool b_val;
    } as;
} RenderValue;

typedef struct _lv_obj_t _lv_obj_t;

// --- Object Registry ---
// A simple dynamic registry to map string IDs to created LVGL objects.
void obj_registry_init(void);
void obj_registry_add(const char* id, void* obj);
char *obj_registry_add_str(const char *s);
void* obj_registry_get(const char* id);
void obj_registry_deinit(void);

// --- Dynamic Dispatcher ---
// Calls an LVGL function by name, with arguments provided as an array of IR nodes.
// Added ApiSpec* spec argument for context-aware parsing (e.g., enums by string name).
RenderValue dynamic_lvgl_call_ir(const char* func_name, void* target_obj, struct IRNode** ir_args, int arg_count, struct ApiSpec* spec);

#ifdef __cplusplus
}
#endif

#endif // LVGL_DISPATCH_H

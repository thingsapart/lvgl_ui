import json
import re
import sys
import argparse
from collections import defaultdict
import os
import shutil
import subprocess
import tempfile

# --- Configuration for Function Wrapping ---
# These lists help filter out functions that are complex, unsafe, or undesirable
# to wrap in a dynamic dispatcher (e.g., those with callbacks, varargs, etc.).

IGNORE_FUNC_ARG_TYPE_SUFFIXES = ["_cb", "_cb_t", "_xcb_t", "void*",
                                 "_lv_display_t*", "ellipsis"]
IGNORE_FUNC_PREFIXES = ["lv_line_set_points"]
IGNORE_FUNC_SUFFIXES = []
IGNORE_ARG_TYPES = [
    "lv_cache_ops_t", "lv_draw_buf_handlers_t", "callback", "cmp_t", "cmp", "va_list",
    "lv_calendar_date_t", "void*", "void", "lv_color16_t", "lv_obj_point_transform_flag_t",
    "lv_style_value_t", "lv_ll_t*", "lv_indev_t*", "ellipsis"
]

# --- Whitelist Configuration ---
# Functions in this set will be wrapped even if they would normally be ignored.
# This is useful for functions with complex arguments (like `void*` or callbacks)
# that we know how to handle manually in the renderer via the object registry.
WHITELIST_FUNCTIONS = {
    'lv_obj_add_event_cb',
    'lv_list_add_button',
    'lv_image_set_src',
    'lv_obj_add_event_cb'
}

# --- Polymorphic Argument Hints ---
# Provides semantic hints for ambiguous argument types like `void*`.
# Key: Function name
# Value: Dictionary mapping { arg_index: hint_string }
# 'arg_index' is the zero-based index of the argument *after* the target object.
POLYMORPHIC_ARG_HINTS = {
    'lv_list_add_button': {0: 'symbol_or_obj'}, # arg 0 is `icon`
    'lv_image_set_src':   {0: 'symbol_or_obj'}, # arg 0 is `src`
    'lv_obj_add_event_cb': {2: 'void_ptr_or_null'}, # arg 2 is `user_data`
}


class CCodeGenerator:
    """
    Generates C header and source files for a dynamic LVGL function dispatcher.
    This dispatcher can call LVGL functions by name, taking arguments from an
    Intermediate Representation (IR) format.
    """
    UNSUPPORTED_RETURN_STRUCTS = {
        'lv_color32_t', 'lv_point_t', 'lv_color_hsv_t', 'lv_style_value_t',
        'lv_point_precise_t', 'lv_span_coords_t'
    }

    def __init__(self, api_spec):
        """Initializes the generator with the parsed API specification."""
        self.spec = api_spec
        self.functions = []
        self.archetypes = defaultdict(list)
        # This now correctly handles a dictionary format for enums.
        self.enum_types = set(self.spec.get('enums', {}).keys())
        self._prepare_functions()

    def _prepare_functions(self):
        """Flattens all functions from the spec into a single list for processing."""
        all_funcs = self.spec.get('functions', {})
        for name, info in all_funcs.items():
            # Ensure info is a dict and add the name to it for easier access
            if isinstance(info, dict):
                info['name'] = name
                self.functions.append(info)
        self.functions.sort(key=lambda f: f['name'])

    def _is_wrappable(self, func_info):
        """Determines if a function can be safely and automatically wrapped."""
        func_name = func_info.get('name', '')
        if not func_name:
            return False

        # First, check if the function is explicitly whitelisted. This overrides all other checks.
        if func_name in WHITELIST_FUNCTIONS:
            return True

        if func_name.startswith('lv_theme_'): return False
        if any(func_name.startswith(p) for p in IGNORE_FUNC_PREFIXES): return False
        if any(func_name.endswith(s) for s in IGNORE_FUNC_SUFFIXES): return False
        if '...' in str(func_info.get('args', [])): return False  # Variadic functions

        # Check argument types against the ignore list
        for arg_type in func_info.get('args', []):
            if not arg_type: continue
            # Whitelist override applies to ignored types as well
            if arg_type in IGNORE_ARG_TYPES and func_name not in WHITELIST_FUNCTIONS: return False
            if any(arg_type.endswith(s) for s in IGNORE_FUNC_ARG_TYPE_SUFFIXES) and func_name not in WHITELIST_FUNCTIONS: return False
            if '(*' in arg_type: return False # Function pointers

        return True

    def _get_archetype_key(self, func_info):
        """
        Generates a key representing a function's signature archetype.
        Functions with the same archetype can share a dispatcher.
        """
        func_name = func_info.get('name', '')
        ret_type = func_info.get('return_type', 'void')
        args = func_info.get('args', [])

        has_target_obj = args and isinstance(args[0], str) and args[0].startswith('lv_') and args[0].endswith('_t*')
        target_c_type = args[0] if has_target_obj else 'void*'
        arg_list = args[1:] if has_target_obj else args

        # ** NEW: Add hints to the archetype key to differentiate polymorphic functions **
        arg_hints = []
        func_hints = POLYMORPHIC_ARG_HINTS.get(func_name, {})
        for i in range(len(arg_list)):
            hint = func_hints.get(i, 'default')
            arg_hints.append(hint)

        return (ret_type, target_c_type, *arg_list, *arg_hints)


    def analyze_archetypes(self):
        """Groups all wrappable functions by their signature archetype."""
        self.archetypes.clear()
        for func in self.functions:
            if self._is_wrappable(func):
                key = self._get_archetype_key(func)
                self.archetypes[key].append(func)

    def _get_parser_for_ir_node(self, c_type, ir_args_var, arg_index, hint):
        """
        Generates the C code snippet to unmarshal an IRNode* into a specific C type,
        now with a hint for polymorphic types.
        """
        c_type_no_const = c_type.replace('const ', '').strip()
        ir_node_accessor = f"{ir_args_var}[{arg_index}]"

        # ** NEW: Check for polymorphic hints first **
        if hint == 'symbol_or_obj':
            return f'({c_type})resolve_symbol_or_obj({ir_node_accessor})'
        if hint == 'void_ptr_or_null':
            # This is for user_data which might be a registered object or just NULL
            return f'({c_type})resolve_symbol_or_obj({ir_node_accessor})'


        if c_type_no_const in self.enum_types:
            return f'({c_type_no_const})ir_node_get_enum_value({ir_node_accessor}, "{c_type_no_const}", spec)'
        if c_type_no_const == 'bool':
            return f'ir_node_get_bool({ir_node_accessor})'
        if c_type == 'const char*':
            return f'ir_node_get_string({ir_node_accessor})'
        if c_type == 'char*':
            return f'obj_registry_add_str(ir_node_get_string({ir_node_accessor}))'
        if c_type_no_const == 'lv_color_t':
            return f'lv_color_hex((uint32_t)ir_node_get_int({ir_node_accessor}))'
        if c_type_no_const == 'lv_color32_t':
            return f'lv_color_to_32(lv_color_hex((uint32_t)ir_node_get_int({ir_node_accessor})), LV_OPA_COVER)'
        if c_type.endswith('*'):
            # **MODIFIED**: Use the new robust pointer unmarshaler
            return f'({c_type})unmarshal_ir_pointer({ir_node_accessor})'

        return f'({c_type_no_const})ir_node_get_int({ir_node_accessor})'

    def generate_files(self, header_path, source_path):
        """Writes the generated header and source files to the specified paths."""
        self._write_header_file(header_path)
        self._write_source_file(source_path)

    def _write_header_file(self, path):
        """Generates the C header file."""
        with open(path, 'w', encoding='utf-8') as f:
            f.write(
"""/*
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
""")

    def _write_source_file(self, path):
        """Generates the C source file."""
        with open(path, 'w', encoding='utf-8') as f:
            f.write(
"""/*
 * AUTO-GENERATED by generate_dynamic_lvgl_dispatch.py.
 * DO NOT EDIT MANUALLY.
 */
#include "lvgl_dispatch.h"
#include "ir.h"
#include "utils.h" // For ir_node_get_... helpers
#include "api_spec.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// --- Typedefs for Dispatcher Mechanism ---
typedef void (*generic_lvgl_func_t)(void);
typedef RenderValue (*lvgl_ir_dispatcher_t)(generic_lvgl_func_t fn, void* target, struct IRNode** ir_args, int arg_count, struct ApiSpec* spec);
""")

            f.write("\n// --- Forward Declarations for Archetype Dispatchers ---\n")
            self.archetype_map = {}
            for i, key in enumerate(self.archetypes.keys()):
                dispatcher_name = f"dispatch_ir_archetype_{i}"
                self.archetype_map[key] = dispatcher_name
                f.write(f"static RenderValue {dispatcher_name}(generic_lvgl_func_t fn, void* target, struct IRNode** ir_args, int arg_count, struct ApiSpec* spec);\n")

            f.write(
"""
// --- C Helpers for Argument Unmarshaling ---

// Converts an IRNode into a pointer. It handles three cases:
// 1. A raw pointer from an intermediate function call result (IR_EXPR_RAW_POINTER).
// 2. A string ID for a registered object (IR_EXPR_LITERAL with is_string=true).
// 3. A direct reference to a registered object (IR_EXPR_REGISTRY_REF).
static void* unmarshal_ir_pointer(struct IRNode* node) {
    if (!node) return NULL;
    if (node->type == IR_EXPR_RAW_POINTER) {
        return ((IRExprRawPointer*)node)->ptr;
    }
    const char* id = ir_node_get_string(node);
    if (!id) return NULL;
    return obj_registry_get(id);
}

static void* resolve_symbol_or_obj(struct IRNode* node) {
    if (!node) return NULL;
    // First, check if it's a raw pointer (e.g., from a 'user_data' that's already a pointer)
    if (node->type == IR_EXPR_RAW_POINTER) {
        return ((IRExprRawPointer*)node)->ptr;
    }

    const char* s = ir_node_get_string(node);
    if (!s) return NULL;

    // If it's a registered object (e.g., '@my_image'), get it from the registry.
    if (s[0] == '@') {
        return obj_registry_get(s);
    }
    // Otherwise, treat it as a string literal (e.g., LV_SYMBOL_AUDIO).
    return (void*)s;
}
"""
            )

            f.write("\n// --- Archetype Dispatcher Implementations ---\n")
            for i, (key, func_list) in enumerate(self.archetypes.items()):
                dispatcher_name = self.archetype_map[key]
                
                # Deconstruct the archetype key, which now includes hints
                num_non_hint_items = 2 + (len(key) - 2) // 2
                ret_type, target_c_type, *rest = key
                arg_types = rest[:num_non_hint_items - 2]
                arg_hints = rest[num_non_hint_items - 2:]

                first_func = func_list[0]

                f.write(f"// Archetype for {len(func_list)} functions like: {first_func['name']}\n")
                f.write(f"static RenderValue {dispatcher_name}(generic_lvgl_func_t fn, void* target, struct IRNode** ir_args, int arg_count, struct ApiSpec* spec) {{\n")
                f.write(f"    RenderValue result; result.type = RENDER_VAL_TYPE_NULL; result.as.p_val = NULL;\n")

                if target_c_type != 'void*':
                    f.write(f'    if (target == NULL) {{ print_warning("Argument 0 (target) for %s is NULL - not allowed", "{first_func["name"]}"); return result; }}\n')
                num_expected_args = len(arg_types)
                f.write(f"    if (arg_count != {num_expected_args}) {{\n")
                f.write(f"        print_warning(\"IR call to {first_func['name']}-like function: expected {num_expected_args} args, got %d\", arg_count);\n")
                f.write(f"        return result;\n")
                f.write(f"    }}\n\n")

                for j, c_type in enumerate(arg_types):
                    hint = arg_hints[j]
                    parser_code = self._get_parser_for_ir_node(c_type, 'ir_args', j, hint)
                    f.write(f"    {c_type} arg{j} = {parser_code};\n")
                    if c_type == 'lv_style_t*' or c_type == 'lv_obj_t*':
                        f.write(f'    if (arg{j} == NULL) {{ /* print_warning("Argument {j} ({c_type}) for {first_func["name"]} is NULL"); */ }}\n')

                c_call_arg_types = [target_c_type] if target_c_type != 'void*' else []
                c_call_arg_types.extend(arg_types)
                f.write(f"    typedef {ret_type} (*specific_func_t)({', '.join(c_call_arg_types) if c_call_arg_types else 'void'});\n")

                call_params = [f"({target_c_type})target"] if target_c_type != 'void*' else []
                call_params.extend(f"arg{j}" for j in range(len(arg_types)))
                call_str = f"((specific_func_t)fn)({', '.join(call_params)})"

                ret_type_no_const = ret_type.replace('const ', '').strip()

                if ret_type == "void":
                    f.write(f"    {call_str};\n")
                elif ret_type_no_const in self.UNSUPPORTED_RETURN_STRUCTS:
                    f.write(f"    (void){call_str}; // Return type '{ret_type_no_const}' is a struct and not supported. Value ignored.\n")
                elif ret_type_no_const == "bool":
                    f.write(f"    result.type = RENDER_VAL_TYPE_BOOL;\n")
                    f.write(f"    result.as.b_val = {call_str};\n")
                elif ret_type.endswith('*'): # Pointer return
                    f.write(f"    result.type = RENDER_VAL_TYPE_POINTER;\n")
                    f.write(f"    result.as.p_val = {call_str};\n")
                elif ret_type_no_const == "lv_color_t":
                    f.write(f"    result.type = RENDER_VAL_TYPE_COLOR;\n")
                    f.write(f"    result.as.color_val = {call_str};\n")
                else: # Non-pointer, non-void return (assume integer-like)
                    f.write(f"    result.type = RENDER_VAL_TYPE_INT;\n")
                    f.write(f"    result.as.i_val = (intptr_t)({call_str});\n")

                f.write("    return result;\n")
                f.write("}\n\n")

            f.write(
"""
typedef struct {
    const char* name;
    lvgl_ir_dispatcher_t ir_dispatcher;
    generic_lvgl_func_t func_ptr;
} FunctionMapping;

// This array is sorted by name for bsearch
static const FunctionMapping function_registry[] = {
""")
            registry_entries = []
            for key, funcs in self.archetypes.items():
                dispatcher_name = self.archetype_map[key]
                for func in funcs:
                    registry_entries.append(
                        f'    {{"{func["name"]}", {dispatcher_name}, (generic_lvgl_func_t){func["name"]}}}'
                    )

            registry_entries.sort()
            f.write(",\n".join(registry_entries))
            f.write("\n};\n\n")

            f.write(
"""
static int compare_func_mappings(const void* a, const void* b) {
    return strcmp((const char*)a, ((const FunctionMapping*)b)->name);
}

RenderValue dynamic_lvgl_call_ir(const char* func_name, void* target_obj, struct IRNode** ir_args, int arg_count, struct ApiSpec* spec) {
    RenderValue result; result.type = RENDER_VAL_TYPE_NULL; result.as.p_val = NULL;
    if (!func_name) return result;
    const FunctionMapping* mapping = (const FunctionMapping*)bsearch(
        func_name, function_registry,
        sizeof(function_registry) / sizeof(FunctionMapping),
        sizeof(FunctionMapping), compare_func_mappings
    );
    if (mapping && mapping->ir_dispatcher) {
        return mapping->ir_dispatcher(mapping->func_ptr, target_obj, ir_args, arg_count, spec);
    }
    print_warning("Dynamic LVGL IR call failed: function '%s' not found or dispatcher missing.", func_name);
    return result;
}

// --- Simple Object Registry Implementation ---
#ifndef DYNAMIC_LVGL_MAX_OBJECTS
#define DYNAMIC_LVGL_MAX_OBJECTS 256
#endif

typedef struct {
    char* id;
    void* obj;
} ObjectEntry;

static ObjectEntry obj_registry[DYNAMIC_LVGL_MAX_OBJECTS];
static int obj_registry_count = 0;

void obj_registry_init(void) {
    obj_registry_count = 0;
    memset(obj_registry, 0, sizeof(obj_registry));
}

char* obj_registry_add_str(const char *s) {
    if (!s) return NULL;
    if (obj_registry_count >= DYNAMIC_LVGL_MAX_OBJECTS) {
        print_warning("Cannot add string to registry: registry full");
        return (char*)s;
    }
    size_t slen = strlen(s);
    char* id_buf = malloc(slen + 6);
    if (!id_buf) return (char*)s;
    sprintf(id_buf, "str::%s", s);

    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, id_buf) == 0) {
            free(id_buf);
            return (char*)obj_registry[i].obj;
        }
    }

    obj_registry[obj_registry_count].id = id_buf;
    obj_registry[obj_registry_count].obj = strdup(s);
    return (char*)obj_registry[obj_registry_count++].obj;
}

void obj_registry_add(const char* id, void* obj) {
    if (!id) return;
    if (obj_registry_count >= DYNAMIC_LVGL_MAX_OBJECTS) {
        print_warning("Cannot add object to registry: full or null ID");
        return;
    }
    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, id) == 0) {
            obj_registry[i].obj = obj;
            return;
        }
    }
    obj_registry[obj_registry_count].id = strdup(id);
    obj_registry[obj_registry_count].obj = obj;
    obj_registry_count++;
}

void* obj_registry_get(const char* id) {
    if (!id) return NULL;
    if (strcmp(id, "SCREEN_ACTIVE") == 0) return (void*)lv_screen_active();
    if (strcmp(id, "NULL") == 0) return NULL;

    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, id) == 0) {
            return obj_registry[i].obj;
        }
    }
    const char* key = (id[0] == '@') ? id + 1 : id;
    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, key) == 0) {
            return obj_registry[i].obj;
        }
    }

    print_warning("Object with ID '%s' not found in registry.", id);
    return NULL;
}

void obj_registry_deinit(void) {
    for (int i = 0; i < obj_registry_count; i++) {
        if(obj_registry[i].id) free(obj_registry[i].id);
        if(obj_registry[i].obj && strncmp(obj_registry[i].id, "str::", 5) == 0) {
            free(obj_registry[i].obj);
        }
    }
    obj_registry_init();
}
""")

def main():
    """Main entry point for the script."""
    arg_parser = argparse.ArgumentParser(description="Generate a dynamic LVGL dispatcher from an API specification.")
    arg_parser.add_argument("api_spec_path", help="Path to the processed api_spec.json file.")
    arg_parser.add_argument("--header-out", default="lvgl_dispatch.h", help="Output path for the generated C header file.")
    arg_parser.add_argument("--source-out", default="lvgl_dispatch.c", help="Output path for the generated C source file.")
    args = arg_parser.parse_args()

    print(f"--- C Code Generation for Dynamic Dispatcher ---", file=sys.stderr)

    try:
        with open(args.api_spec_path, 'r', encoding='utf-8') as f:
            print(f"Loading API spec from: {args.api_spec_path}", file=sys.stderr)
            api_spec = json.load(f)
    except FileNotFoundError:
        print(f"Error: API spec file not found at '{args.api_spec_path}'", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: Could not decode JSON from '{args.api_spec_path}': {e}", file=sys.stderr)
        sys.exit(1)

    generator = CCodeGenerator(api_spec)

    print("Analyzing function archetypes...", file=sys.stderr)
    generator.analyze_archetypes()
    print(f"Found {len(generator.archetypes)} unique, wrappable function archetypes.", file=sys.stderr)

    print(f"Generating {args.header_out} and {args.source_out}...", file=sys.stderr)
    generator.generate_files(args.header_out, args.source_out)

    print("Done.", file=sys.stderr)

if __name__ == '__main__':
    main()

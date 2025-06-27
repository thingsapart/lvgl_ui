import json
import re
import sys
import argparse
from collections import defaultdict

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

class LVGLApiSpecTransformer:
    """
    Transforms the raw lv_def.json spec into a more convenient format
    for the CCodeGenerator, primarily by converting the 'enums' list
    into a dictionary.
    """

    def __init__(self, raw_spec):
        self.raw_spec = raw_spec
        self.transformed_spec = {}

    def _get_type_str(self, type_info):
        """Recursively builds a string representation of a C type from lv_def.json format."""
        if not type_info: return "void*"
        if type_info.get('json_type') == 'ret_type':
            return self._get_type_str(type_info.get('type'))
        if type_info.get('name') == 'void' and 'pointer' not in type_info.get('json_type', ''):
            return "void"

        suffix = ""
        current = type_info
        while current.get('json_type') == 'pointer':
            suffix += '*'
            current = current.get('type', {})

        base_name = current.get('name') or current.get('type', {}).get('name') or "void"
        if base_name == "anonymous": base_name = "void"

        base_name = base_name.replace(" const", "").strip()
        if "const " in base_name and suffix:
            base_name = base_name.replace("const ", "")
            return f"const {base_name}{suffix}".strip()

        return f"{base_name}{suffix}".strip()

    def transform(self):
        """Performs the transformation."""
        # Copy over sections that don't need transformation
        self.transformed_spec['constants'] = self.raw_spec.get('constants', {})
        self.transformed_spec['objects'] = self.raw_spec.get('objects', {})
        self.transformed_spec['widgets'] = self.raw_spec.get('widgets', {})

        # Transform 'enums' from a list of objects to a dictionary
        enums_dict = {}
        for enum_obj in self.raw_spec.get('enums', []):
            if isinstance(enum_obj, dict) and 'name' in enum_obj and 'members' in enum_obj:
                enum_name = enum_obj['name']
                members_dict = {
                    member['name']: member['value']
                    for member in enum_obj.get('members', [])
                    if 'name' in member and 'value' in member
                }
                enums_dict[enum_name] = members_dict
        self.transformed_spec['enums'] = enums_dict

        # Transform 'functions' from list to dict and simplify signatures
        functions_dict = {}
        for func_obj in self.raw_spec.get('functions', []):
            if not isinstance(func_obj, dict): continue
            func_name = func_obj.get('name')
            if not func_name: continue

            ret_type = self._get_type_str(func_obj.get('type'))

            args_list = []
            args_spec = func_obj.get('args')
            if args_spec:
                is_void_arg = len(args_spec) == 1 and self._get_type_str(args_spec[0].get('type')) == 'void'
                if not is_void_arg:
                    args_list = [self._get_type_str(arg.get('type')) for arg in args_spec]

            functions_dict[func_name] = {"return_type": ret_type, "args": args_list}

        # Handle aliases like "lv_btn_create": "lv_button_create"
        # This is a simplification; a full implementation might need to copy arg data.
        # For now, we assume the CCodeGenerator will look up the final function name.
        raw_funcs = self.raw_spec.get('functions', [])
        for func_obj in raw_funcs:
             if isinstance(func_obj, dict):
                 func_name = func_obj.get('name')
                 if func_name and func_obj.get(func_name) and isinstance(func_obj[func_name], str):
                     alias_target = func_obj[func_name]
                     if alias_target in functions_dict:
                         functions_dict[func_name] = functions_dict[alias_target]


        self.transformed_spec['functions'] = functions_dict

        return self.transformed_spec


class CCodeGenerator:
    """
    Generates C header and source files for a dynamic LVGL function dispatcher.
    This dispatcher can call LVGL functions by name, taking arguments from an
    Intermediate Representation (IR) format.
    """

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

        if func_name.startswith('lv_theme_'): return False
        if any(func_name.startswith(p) for p in IGNORE_FUNC_PREFIXES): return False
        if any(func_name.endswith(s) for s in IGNORE_FUNC_SUFFIXES): return False
        if '...' in str(func_info.get('args', [])): return False  # Variadic functions

        # Check argument types against the ignore list
        for arg_type in func_info.get('args', []):
            if not arg_type: continue
            if arg_type in IGNORE_ARG_TYPES: return False
            if any(arg_type.endswith(s) for s in IGNORE_FUNC_ARG_TYPE_SUFFIXES): return False
            if '(*' in arg_type: return False # Function pointers
            if arg_type in ['void*', 'const void*']: return False

        return True

    def _get_archetype_key(self, func_info):
        """
        Generates a key representing a function's signature archetype.
        Functions with the same archetype can share a dispatcher.
        """
        ret_type = func_info.get('return_type', 'void')
        args = func_info.get('args', [])

        # The first argument is the target object if it's an LVGL pointer type
        has_target_obj = args and isinstance(args[0], str) and args[0].startswith('lv_') and args[0].endswith('_t*')

        target_c_type = args[0] if has_target_obj else 'void*'
        arg_list = args[1:] if has_target_obj else args

        # An archetype is defined by: (return_type, target_obj_type, arg1_type, arg2_type, ...)
        return (ret_type, target_c_type, *arg_list)

    def analyze_archetypes(self):
        """Groups all wrappable functions by their signature archetype."""
        self.archetypes.clear()
        for func in self.functions:
            if self._is_wrappable(func):
                key = self._get_archetype_key(func)
                self.archetypes[key].append(func)

    def _get_parser_for_ir_node(self, c_type, ir_args_var, arg_index):
        """
        Generates the C code snippet to unmarshal an IRNode* into a specific C type.
        This now uses the C helper functions like `ir_node_get_int`, etc.
        """
        c_type_no_const = c_type.replace('const ', '').strip()
        ir_node_accessor = f"{ir_args_var}[{arg_index}]"

        # For enums, use the dedicated helper that can look up string symbols
        if c_type_no_const in self.enum_types:
            return f'({c_type_no_const})ir_node_get_enum_value({ir_node_accessor}, "{c_type_no_const}", spec)'

        if c_type_no_const == 'bool':
            return f'ir_node_get_bool({ir_node_accessor})'

        if c_type == 'const char*':
            return f'ir_node_get_string({ir_node_accessor})'

        # For mutable strings, create a copy in the registry
        if c_type == 'char*':
            return f'obj_registry_add_str(ir_node_get_string({ir_node_accessor}))'

        # For colors, convert from the integer value in the IR
        if c_type_no_const == 'lv_color_t':
            return f'lv_color_hex((uint32_t)ir_node_get_int({ir_node_accessor}))'

        if c_type_no_const == 'lv_color32_t':
            return f'lv_color_to_32(lv_color_hex((uint32_t)ir_node_get_int({ir_node_accessor})), LV_OPA_COVER)'

        # For any other pointer, assume it's a reference in the object registry
        if c_type.endswith('*'):
            return f'({c_type})obj_registry_get(ir_node_get_string({ir_node_accessor}))'

        # Default case for numeric types (int, lv_coord_t, uint32_t, etc.)
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

// Forward declarations for required structs
struct IRNode;
struct ApiSpec;

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
lv_obj_t* dynamic_lvgl_call_ir(const char* func_name, void* target_obj, struct IRNode** ir_args, int arg_count, struct ApiSpec* spec);

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

// --- Typedefs for Dispatcher Mechanism ---
typedef void (*generic_lvgl_func_t)(void);
typedef lv_obj_t* (*lvgl_ir_dispatcher_t)(generic_lvgl_func_t fn, void* target, struct IRNode** ir_args, int arg_count, struct ApiSpec* spec);
""")

            self.archetype_map = {}
            # Generate forward declarations for all archetype dispatchers
            f.write("\n// --- Forward Declarations for Archetype Dispatchers ---\n")
            for i, key in enumerate(self.archetypes.keys()):
                dispatcher_name = f"dispatch_ir_archetype_{i}"
                self.archetype_map[key] = dispatcher_name
                f.write(f"static lv_obj_t* {dispatcher_name}(generic_lvgl_func_t fn, void* target, struct IRNode** ir_args, int arg_count, struct ApiSpec* spec);\n")

            f.write("\n// --- Archetype Dispatcher Implementations ---\n")

            # Generate implementation for each archetype dispatcher
            for i, (key, func_list) in enumerate(self.archetypes.items()):
                dispatcher_name = self.archetype_map[key]
                ret_type, target_c_type, *arg_types = key
                first_func = func_list[0]

                f.write(f"// Archetype for {len(func_list)} functions like: {first_func['name']}\n")
                f.write(f"static lv_obj_t* {dispatcher_name}(generic_lvgl_func_t fn, void* target, struct IRNode** ir_args, int arg_count, struct ApiSpec* spec) {{\n")

                num_expected_args = len(arg_types)
                f.write(f"    if (arg_count != {num_expected_args}) {{\n")
                f.write(f"        LV_LOG_WARN(\"IR call to {first_func['name']}-like function: expected {num_expected_args} args, got %d\", arg_count);\n")
                f.write(f"        return NULL;\n")
                f.write(f"    }}\n\n")

                # Argument parsing using the new IR helpers
                for j, c_type in enumerate(arg_types):
                    parser_code = self._get_parser_for_ir_node(c_type, 'ir_args', j)
                    f.write(f"    {c_type} arg{j} = {parser_code};\n")

                # Typedef for the specific function pointer
                c_call_arg_types = [target_c_type] if target_c_type != 'void*' else []
                c_call_arg_types.extend(arg_types)
                f.write(f"    typedef {ret_type} (*specific_func_t)({', '.join(c_call_arg_types) if c_call_arg_types else 'void'});\n")

                # The actual function call
                call_params = [f"({target_c_type})target"] if target_c_type != 'void*' else []
                call_params.extend(f"arg{j}" for j in range(len(arg_types)))
                call_str = f"((specific_func_t)fn)({', '.join(call_params)})"

                if ret_type == "void":
                    f.write(f"    {call_str};\n    return NULL;\n")
                elif ret_type.endswith('*'): # Assume any pointer return can be a lv_obj_t* for chaining
                    f.write(f"    return (lv_obj_t*){call_str};\n")
                else: # Non-pointer return, not chainable
                    f.write(f"    (void){call_str};\n    return NULL;\n")

                f.write("}\n\n")

            # --- Function Registry and Main Dispatcher ---
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
            # Generate registry entries
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

lv_obj_t* dynamic_lvgl_call_ir(const char* func_name, void* target_obj, struct IRNode** ir_args, int arg_count, struct ApiSpec* spec) {
    if (!func_name) return NULL;
    const FunctionMapping* mapping = (const FunctionMapping*)bsearch(
        func_name, function_registry,
        sizeof(function_registry) / sizeof(FunctionMapping),
        sizeof(FunctionMapping), compare_func_mappings
    );
    if (mapping && mapping->ir_dispatcher) {
        // Pass the spec pointer to the archetype dispatcher for context
        return mapping->ir_dispatcher(mapping->func_ptr, target_obj, ir_args, arg_count, spec);
    }
    LV_LOG_WARN("Dynamic LVGL IR call failed: function '%s' not found or dispatcher missing.", func_name);
    return NULL;
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

// Makes a copy of a string and stores it in the registry to manage its lifetime.
char* obj_registry_add_str(const char *s) {
    if (!s) return NULL;
    if (obj_registry_count >= DYNAMIC_LVGL_MAX_OBJECTS) {
        LV_LOG_WARN("Cannot add string to registry: registry full");
        return (char*)s; // Fallback: return original pointer
    }
    // We use a prefix to avoid ID collisions with objects.
    size_t slen = strlen(s);
    char* id_buf = malloc(slen + 6); // "str::" + s + null terminator
    if (!id_buf) return (char*)s;
    sprintf(id_buf, "str::%s", s);

    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, id_buf) == 0) {
            free(id_buf);
            return (char*)obj_registry[i].obj; // Return existing copy
        }
    }

    obj_registry[obj_registry_count].id = id_buf;
    obj_registry[obj_registry_count].obj = strdup(s);
    return (char*)obj_registry[obj_registry_count++].obj;
}

void obj_registry_add(const char* id, void* obj) {
    if (!id) return;
    if (obj_registry_count >= DYNAMIC_LVGL_MAX_OBJECTS) {
        LV_LOG_WARN("Cannot add object to registry: full or null ID");
        return;
    }
    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, id) == 0) {
            obj_registry[i].obj = obj; // Update existing entry
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
    LV_LOG_WARN("Object with ID '%s' not found in registry.", id);
    return NULL;
}

void obj_registry_deinit(void) {
    for (int i = 0; i < obj_registry_count; i++) {
        if(obj_registry[i].id) free(obj_registry[i].id);
        if(obj_registry[i].obj && strncmp(obj_registry[i].id, "str::", 5) == 0) {
            free(obj_registry[i].obj); // Free strings copied by obj_registry_add_str
        }
    }
    obj_registry_init();
}
""")

def main():
    """Main entry point for the script."""
    arg_parser = argparse.ArgumentParser(description="Generate a dynamic LVGL dispatcher from an API specification.")
    arg_parser.add_argument("api_spec_path", help="Path to the raw lv_def.json API spec file.")
    arg_parser.add_argument("--header-out", default="lvgl_dispatch.h", help="Output path for the generated C header file.")
    arg_parser.add_argument("--source-out", default="lvgl_dispatch.c", help="Output path for the generated C source file.")
    args = arg_parser.parse_args()

    try:
        with open(args.api_spec_path, 'r', encoding='utf-8') as f:
            raw_spec = json.load(f)
    except FileNotFoundError:
        print(f"Error: API spec file not found at '{args.api_spec_path}'", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: Could not decode JSON from '{args.api_spec_path}': {e}", file=sys.stderr)
        sys.exit(1)

    print("--- C Code Generation for Dynamic Dispatcher ---", file=sys.stderr)

    # Transform the raw spec into the format expected by the CCodeGenerator
    transformer = LVGLApiSpecTransformer(raw_spec)
    transformed_spec = transformer.transform()

    generator = CCodeGenerator(transformed_spec)

    print("Analyzing function archetypes...", file=sys.stderr)
    generator.analyze_archetypes()
    print(f"Found {len(generator.archetypes)} unique, wrappable function archetypes.", file=sys.stderr)

    print(f"Generating {args.header_out} and {args.source_out}...", file=sys.stderr)
    generator.generate_files(args.header_out, args.source_out)

    print("Done.", file=sys.stderr)

if __name__ == '__main__':
    main()

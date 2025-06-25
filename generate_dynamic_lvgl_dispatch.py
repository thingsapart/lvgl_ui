import json
import re
import sys
import argparse # For command-line arguments
from collections import defaultdict

# Global lists for ignoring functions
IGNORE_FUNC_ARG_TYPE_SUFFIXES = ["_cb", "_cb_t", "_xcb_t", "void*",
                                 "_lv_display_t*"]
IGNORE_FUNC_PREFIXES = ["lv_line_set_points"]
IGNORE_FUNC_SUFFIXES = []
IGNORE_ARG_TYPES = ["lv_cache_ops_t", "lv_draw_buf_handlers_t", "callback",
                    "cmp_t", "cmp", "va_list", "lv_calendar_date_t", "void*",
                    "void", "lv_color16_t", "lv_obj_point_transform_flag_t",
                    "lv_style_value_t", "lv_ll_t*", "lv_indev_t*"]

class LVGLApiParser:
    """
    Parses the LVGL API specification JSON and translates it into a more
    structured, hierarchical format suitable for code generators or documentation.
    This part of the script is primarily for generating the intermediate JSON
    that the CCodeGenerator uses.
    """

    def __init__(self, api_spec):
        """Initializes the parser with the raw API specification."""
        self.spec = api_spec
        self.result = {
            "constants": {},
            "enums": {},
            "functions": {},
            "widgets": defaultdict(lambda: {"properties": {}, "methods": {}}),
            "objects": defaultdict(lambda: {"properties": {}, "methods": {}}),
        }
        # Pre-populate with known types
        self.widget_types = {'obj'}
        self.object_types = {'style', 'anim', 'timer'}
        self.enum_type_names = set()

    def _get_type_str(self, type_info):
        """Recursively builds a string representation of a C type."""
        if not type_info:
            return "void*" # Default to void* if type info is missing
        if type_info.get('json_type') == 'ret_type':
            return self._get_type_str(type_info.get('type'))
        if type_info.get('name') == 'void' and 'pointer' not in type_info.get('json_type', ''):
            return "void"

        suffix = ""
        current = type_info
        while current.get('json_type') == 'pointer':
            suffix += '*'
            current = current.get('type', {})

        base_name = current.get('name') or current.get('type', {}).get('name') or "void" # Default to void
        if base_name == "anonymous": # Replace anonymous with a usable type
            base_name = "void"

        base_name = base_name.replace(" const", "").strip()
        if "const " in base_name and suffix:
             base_name = base_name.replace("const ", "")
             return f"const {base_name}{suffix}".strip()

        return f"{base_name}{suffix}".strip()

    def _get_property_type(self, func_info):
        """Determines a simplified property type from a setter function's signature."""
        args = func_info.get('args', [])
        if len(args) < 2:
            return 'unknown'
        val_arg = args[1]
        return self._get_type_str(val_arg.get('type'))

    def _discover_types(self):
        """First pass to identify all enum, widget, and object types."""
        for enum in self.spec.get('enums', []):
            if enum.get('name'):
                self.enum_type_names.add(enum['name'])

        for func in self.spec.get('functions', []):
            name = func.get('name', '')
            if name.endswith('_create'):
                match = re.match(r"lv_(\w+)_create", name)
                if match:
                    self.widget_types.add(match.group(1))
            elif name.endswith('_init'):
                match = re.match(r"lv_(\w+)_init", name)
                if match and match.group(1) and match.group(1) not in ['mem']:
                    self.object_types.add(match.group(1))

    def _translate_primitives(self):
        """Translate enums, constants, and raw function signatures."""
        for enum in self.spec.get('enums', []):
            members = {member['name']: member['value'] for member in enum.get('members', [])}
            if enum.get('name'):
                self.result['enums'][enum['name']] = members
            else:
                self.result['constants'].update(members)

        for macro in self.spec.get('macros', []):
            if macro.get('params') is None and macro.get('initializer'):
                initializer = macro['initializer'].strip().replace("ULL", "").replace("UL", "").replace("U", "")
                initializer = initializer.replace("L", "").replace("LL", "")
                self.result['constants'][macro['name']] = initializer

        for func in self.spec.get('functions', []):
            func_name = func.get('name', '')
            if not func_name: continue
            ret_type_str = self._get_type_str(func.get('type'))
            args_list = []
            if func.get('args'):
                is_void_arg = len(func['args']) == 1 and self._get_type_str(func['args'][0].get('type')) == 'void'
                if not is_void_arg:
                    args_list = [
                        {"name": arg.get("name"), "type": self._get_type_str(arg.get('type'))}
                        for arg in func['args']
                    ]
            self.result['functions'][func_name] = {"return_type": ret_type_str, "args": args_list}

    def _structure_api(self):
        """Second pass: Organize functions into widget/object properties and methods."""
        self.result['widgets']['obj']['inherits'] = None
        for w_name in self.widget_types:
            if w_name != 'obj':
                self.result['widgets'][w_name]['inherits'] = 'obj'
                self.result['widgets'][w_name]['create'] = f'lv_{w_name}_create'

        for o_name in self.object_types:
            self.result['objects'][o_name]['c_type'] = f'lv_{o_name}_t'
            self.result['objects'][o_name]['init'] = f'lv_{o_name}_init'

        for func in self.spec.get('functions', []):
            func_name = func.get('name', '')
            match = re.match(r"lv_([a-zA-Z0-9]+)_(.*)", func_name)
            if not match: continue
            target_name, action = match.groups()

            target_collection = None
            if target_name in self.widget_types: target_collection = self.result['widgets']
            elif target_name in self.object_types: target_collection = self.result['objects']

            if target_collection:
                target_collection[target_name]['methods'][func_name] = self.result['functions'][func_name]
                prop_name = None
                if action.startswith('set_'): prop_name = action[4:]
                elif action.startswith(('add_', 'clear_')): prop_name = action
                if prop_name:
                    prop_type = self._get_property_type(func)
                    target_collection[target_name]['properties'][prop_name] = {"setter": func_name, "type": prop_type}

    def _finalize_and_sort(self):
        """Converts defaultdicts and sorts all keys for consistent output."""
        self.result['widgets'] = dict(sorted(self.result['widgets'].items()))
        self.result['objects'] = dict(sorted(self.result['objects'].items()))
        self.result = dict(sorted(self.result.items()))
        for key in ['constants', 'enums', 'functions', 'widgets', 'objects']:
            if key in self.result: self.result[key] = dict(sorted(self.result[key].items()))
        for w_name, w_data in self.result.get('widgets', {}).items():
            if 'properties' in w_data: w_data['properties'] = dict(sorted(w_data['properties'].items()))
            if 'methods' in w_data: w_data['methods'] = dict(sorted(w_data['methods'].items()))
            self.result['widgets'][w_name] = dict(sorted(w_data.items()))
        for o_name, o_data in self.result.get('objects', {}).items():
            if 'properties' in o_data: o_data['properties'] = dict(sorted(o_data['properties'].items()))
            if 'methods' in o_data: o_data['methods'] = dict(sorted(o_data['methods'].items()))
            self.result['objects'][o_name] = dict(sorted(o_data.items()))

    def parse(self):
        """Run all translation steps and return the final structure."""
        self._discover_types()
        self._translate_primitives()
        self._structure_api()
        self._finalize_and_sort()
        return self.result

# ----------------------------------------------------------------------
# ---- C file generator for dynamic LVGL function calls ----
# ----------------------------------------------------------------------

class CCodeGenerator:
    """Generates C header and source files for a dynamic LVGL function dispatcher."""

    def __init__(self, translated_spec):
        self.spec = translated_spec
        self.functions = []
        self.archetypes = defaultdict(list)
        self.enum_types = set(self.spec['enums'].keys())
        self._prepare_functions()

    def _prepare_functions(self):
        """Flattens all functions into a single list for processing."""
        all_funcs = self.spec['functions']
        for name, info in all_funcs.items():
            info['name'] = name
            self.functions.append(info)
        self.functions.sort(key=lambda f: f['name'])

    def _is_wrappable(self, func_info):
        """Determines if a function can be automatically wrapped."""
        func_name = func_info.get('name', '')
        if not func_name: return False

        if func_name.startswith('lv_theme_'): return False
        if any(func_name.startswith(p) for p in IGNORE_FUNC_PREFIXES): return False
        if any(func_name.endswith(s) for s in IGNORE_FUNC_SUFFIXES): return False
        if '...' in str(func_info.get('args', [])): return False # Variadic

        for arg in func_info.get('args', []):
            arg_type = arg.get('type', '')
            if not arg_type: continue
            if arg_type in IGNORE_ARG_TYPES: return False
            if any(arg_type.endswith(s) for s in IGNORE_FUNC_ARG_TYPE_SUFFIXES): return False
            if '(*' in arg_type: return False
            if arg_type == 'void*' or arg_type == 'const void*': return False

        return True

    def _get_archetype_key(self, func_info):
        """Generates a key representing the function's signature archetype."""
        ret_type = func_info['return_type']
        args = func_info.get('args', [])
        # Treat any lv_..._t* as a potential target object.
        has_target_obj = args and args[0]['type'].startswith('lv_') and args[0]['type'].endswith('_t*')

        target_c_type = args[0]['type'] if has_target_obj else 'void*'
        arg_list = args[1:] if has_target_obj else args

        # Key: (return_type, target_obj_type, arg1_type, arg2_type, ...)
        return (ret_type, target_c_type, *(arg['type'] for arg in arg_list))

    def analyze_archetypes(self):
        """Groups all wrappable functions by their archetype key."""
        self.archetypes.clear()
        for func in self.functions:
            if self._is_wrappable(func):
                key = self._get_archetype_key(func)
                self.archetypes[key].append(func)

    def _get_parser_for_ir_node_type(self, c_type, ir_args_var, arg_index):
        """Gets the C code snippet to parse an IRNode* into a C type, improving type safety."""
        c_type_no_const = c_type.replace('const ', '').strip()
        ir_node_accessor = f"{ir_args_var}[{arg_index}]"

        if c_type_no_const in self.enum_types:
            return f"({c_type_no_const})ir_node_get_int({ir_node_accessor})"

        if c_type_no_const == 'bool':
            return f"ir_node_get_bool({ir_node_accessor})"

        if c_type == 'const char*':
            return f"ir_node_get_string({ir_node_accessor})"

        # Handle color structs. Assumes IR provides a number (e.g., from #RRGGBB being converted to 0xRRGGBB).
        # if c_type_no_const in ['lv_color_t', 'lv_color16_t', 'lv_color32_t']:
        # Need to break up lv_color_t, lv_color32_t - no conversion found for
        # lv_color16_t, skipping for now.
        if c_type_no_const in ['lv_color_t']:
            return f"lv_color_hex((uint32_t)ir_node_get_int({ir_node_accessor}))"
        if c_type_no_const in ['lv_color32_t']:
            return f"lv_color_to_32(lv_color_hex((uint32_t)ir_node_get_int({ir_node_accessor})), LV_OPA_COVER)"

        if c_type.endswith('*'):
            # Any other pointer type is assumed to be in the object registry.
            return f"({c_type})obj_registry_get(ir_node_get_string({ir_node_accessor}))"

        # Default for int, lv_coord_t, uint32_t, etc.
        return f"({c_type_no_const})ir_node_get_int({ir_node_accessor})"


    def generate_files(self, header_path, source_path):
        self._write_header_file(header_path)
        self._write_source_file(source_path)

    def _write_header_file(self, path):
        with open(path, 'w', encoding='utf-8') as f:
            f.write(
"""/*
 * AUTO-GENERATED by generate_dynamic_lvgl_dispatch.py.
 * DO NOT EDIT MANUALLY.
 */
#ifndef DYNAMIC_LVGL_H
#define DYNAMIC_LVGL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>

typedef struct IRNode IRNode;

// --- Object Registry ---
// A simple dynamic registry to map string IDs to created LVGL objects (widgets, styles, etc.).
void obj_registry_init(void);
void obj_registry_add(const char* id, void* obj);
void* obj_registry_get(const char* id);
void obj_registry_deinit(void);

// --- Dynamic Dispatcher ---
// Calls an LVGL function by name, with arguments provided as an array of IR nodes.
// The renderer is responsible for resolving complex IR expressions (like context vars)
// into simpler, self-contained IR nodes (literals, registry refs) before calling.
lv_obj_t* dynamic_lvgl_call_ir(const char* func_name, void* target_obj, IRNode** ir_args, int arg_count);

#ifdef __cplusplus
}
#endif

#endif // DYNAMIC_LVGL_H
""")

    def _write_source_file(self, path):
        with open(path, 'w', encoding='utf-8') as f:
            f.write(
"""/*
 * AUTO-GENERATED by generate_dynamic_lvgl_dispatch.py.
 * DO NOT EDIT MANUALLY.
 */
#include "lvgl_dispatch.h"
#include "ir.h"
#include "utils.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct _lv_obj_t _lv_obj_t;
typedef void (*generic_lvgl_func_t)(void);
typedef lv_obj_t* (*lvgl_ir_dispatcher_t)(generic_lvgl_func_t fn, void* target, IRNode** ir_args, int arg_count);
""")

            self.archetype_map = {}
            # Generate forward declarations for all archetype dispatchers
            for i, key in enumerate(self.archetypes.keys()):
                dispatcher_name_ir = f"dispatch_ir_archetype_{i}"
                self.archetype_map[key] = dispatcher_name_ir
                f.write(f"static lv_obj_t* {dispatcher_name_ir}(generic_lvgl_func_t fn, void* target, IRNode** ir_args, int arg_count);\n")

            f.write("\n// --- Archetype Dispatcher Implementations ---\n")

            # Generate implementation for each archetype dispatcher
            for i, (key, func_list) in enumerate(self.archetypes.items()):
                dispatcher_name_ir = self.archetype_map[key]
                ret_type, target_c_type, *arg_types = key
                first_func = func_list[0]

                f.write(f"// Archetype for {len(func_list)} functions like: {first_func['name']}\n")
                f.write(f"static lv_obj_t* {dispatcher_name_ir}(generic_lvgl_func_t fn, void* target, IRNode** ir_args, int arg_count) {{\n")

                num_expected_args = len(arg_types)
                f.write(f"    if (arg_count != {num_expected_args}) {{\n")
                f.write(f"        LV_LOG_WARN(\"IR call to {first_func['name']}-like function: expected {num_expected_args} args, got %d\", arg_count);\n")
                f.write(f"        return NULL;\n")
                f.write(f"    }}\n\n")

                # Argument parsing
                for j, c_type in enumerate(arg_types):
                    parser_code = self._get_parser_for_ir_node_type(c_type, 'ir_args', j)
                    f.write(f"    {c_type} arg{j} = {parser_code};\n")

                # Typedef for the specific function pointer
                c_call_arg_types = [target_c_type] if target_c_type != 'void*' else []
                c_call_arg_types.extend(arg_types)
                f.write(f"    typedef {ret_type} (*specific_func_t)({', '.join(c_call_arg_types) if c_call_arg_types else 'void'});\n")

                # Function call
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

lv_obj_t* dynamic_lvgl_call_ir(const char* func_name, void* target_obj, IRNode** ir_args, int arg_count) {
    if (!func_name) return NULL;
    const FunctionMapping* mapping = (const FunctionMapping*)bsearch(
        func_name, function_registry,
        sizeof(function_registry) / sizeof(FunctionMapping),
        sizeof(FunctionMapping), compare_func_mappings
    );
    if (mapping && mapping->ir_dispatcher) {
        return mapping->ir_dispatcher(mapping->func_ptr, target_obj, ir_args, arg_count);
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

void obj_registry_add(const char* id, void* obj) {
    if (obj_registry_count >= DYNAMIC_LVGL_MAX_OBJECTS || !id) {
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
    }
    obj_registry_init();
}
""")

def main():
    arg_parser = argparse.ArgumentParser(description="Generate dynamic LVGL dispatcher C code.")
    arg_parser.add_argument("api_spec_path", help="Path to the LVGL API spec JSON file (e.g., api_spec.json)")
    arg_parser.add_argument("--header-out", default="lvgl_dispatch.h", help="Output path for the generated C header file.")
    arg_parser.add_argument("--source-out", default="lvgl_dispatch.c", help="Output path for the generated C source file.")

    args = arg_parser.parse_args()

    try:
        with open(args.api_spec_path, 'r', encoding='utf-8') as f:
            api_spec_json = json.load(f)
    except FileNotFoundError:
        print(f"Error: File not found at '{args.api_spec_path}'", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: Could not decode JSON from '{args.api_spec_path}': {e}", file=sys.stderr)
        sys.exit(1)

    parser = LVGLApiParser(api_spec_json)
    translated_spec = parser.parse()

    print(f"--- C Code Generation ---", file=sys.stderr)
    generator = CCodeGenerator(translated_spec)

    print(f"Analyzing function archetypes...", file=sys.stderr)
    generator.analyze_archetypes()
    print(f"Found {len(generator.archetypes)} unique function archetypes.", file=sys.stderr)

    print(f"Generating {args.header_out} and {args.source_out}...", file=sys.stderr)
    generator.generate_files(args.header_out, args.source_out)
    print("Done.", file=sys.stderr)

if __name__ == '__main__':
    main()

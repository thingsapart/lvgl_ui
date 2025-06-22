import json
import re
import sys
from collections import defaultdict

class LVGLApiParser:
    """
    Parses the LVGL API specification JSON and translates it into a more
    structured, hierarchical format suitable for code generators or documentation.
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
        self.object_types = {'style'}
        self.enum_type_names = set()

    def _get_type_str(self, type_info):
        """Recursively builds a string representation of a C type."""
        if not type_info:
            return "unknown"
        if type_info.get('json_type') == 'ret_type':
            return self._get_type_str(type_info.get('type'))
        if type_info.get('name') == 'void' and 'pointer' not in type_info.get('json_type', ''):
            return "void"

        suffix = ""
        current = type_info
        while current.get('json_type') == 'pointer':
            suffix += '*'
            current = current.get('type', {})

        base_name = current.get('name') or current.get('type', {}).get('name') or "anonymous"
        # Sanitize to create a valid C type string
        base_name = base_name.replace(" const", "").strip()
        if "const " in base_name and suffix:
             base_name = base_name.replace("const ", "")
             return f"const {base_name}{suffix}".strip()

        return f"{base_name}{suffix}".strip()

    def _get_property_type(self, func_info):
        """Determines a simplified property type from a setter function's signature."""
        func_name = func_info.get('name', '')
        args = func_info.get('args', [])

        if len(args) < 2:
            return 'unknown'

        # The second argument is usually the value being set.
        val_arg = args[1]
        val_arg_type_str = self._get_type_str(val_arg.get('type'))

        return val_arg_type_str


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
                if match and match.group(1):
                    # Avoid adding 'mem' as a user-facing object
                    if match.group(1) not in ['mem']:
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
                        {
                            "name": arg.get("name"),
                            "type": self._get_type_str(arg.get('type'))
                        } for arg in func['args']
                    ]

            self.result['functions'][func_name] = {
                "return_type": ret_type_str,
                "args": args_list
            }

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

            if target_name in self.widget_types:
                self.result['widgets'][target_name]['methods'][func_name] = self.result['functions'][func_name]
                prop_name = None
                if action.startswith('set_'): prop_name = action[4:]
                elif action.startswith(('add_', 'clear_')): prop_name = action
                if prop_name:
                    prop_type = self._get_property_type(func)
                    self.result['widgets'][target_name]['properties'][prop_name] = {"setter": func_name, "type": prop_type}

            elif target_name in self.object_types:
                self.result['objects'][target_name]['methods'][func_name] = self.result['functions'][func_name]
                prop_name = None
                if action.startswith('set_'): prop_name = action[4:]
                elif action.startswith(('add_', 'clear_')): prop_name = action
                if prop_name:
                    prop_type = self._get_property_type(func)
                    self.result['objects'][target_name]['properties'][prop_name] = {"setter": func_name, "type": prop_type}

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
        """Run all translation steps in order and return the final structure."""
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
        if '...' in str(func_info['args']): return False # Variadic functions
        if not func_info['name']: return False
        for arg in func_info['args']:
            arg_type = arg['type']
            if '(*' in arg_type: return False # Function pointers
            if arg_type == 'void*' or arg_type == 'const void*': return False # Generic pointers
        return True

    def _get_generalized_type(self, c_type):
        """Reduces a C type to a broad category for aggressive grouping."""
        if c_type == "void": return "VOID"
        c_type_no_const = c_type.replace('const ', '').strip()
        if c_type_no_const in self.enum_types: return 'INT_LIKE'
        if c_type_no_const.endswith('_t') and 'int' in c_type_no_const: return 'INT_LIKE'
        if c_type_no_const in ['bool', 'char', 'short', 'int', 'long']: return 'INT_LIKE'
        if c_type == 'const char*': return 'STRING'
        if c_type.startswith('lv_') and c_type.endswith('_t*'): return 'OBJ_PTR' # Broader category for any LVGL object/struct pointer
        if c_type == 'lv_color_t': return 'COLOR'
        if c_type.endswith('*'): return 'OTHER_PTR'
        return 'INT_LIKE' # Default assumption for unknown simple structs

    def _get_archetype_key(self, func_info):
        """Creates a tuple based on GENERALIZED types for maximum grouping."""
        generalized_ret_type = self._get_generalized_type(func_info['return_type'])

        args = func_info['args']

        # A "target" exists if the first argument is any kind of pointer.
        has_target = bool(args and args[0]['type'].endswith('*'))

        arg_list = args[1:] if has_target else args
        generalized_arg_types = [self._get_generalized_type(arg['type']) for arg in arg_list]

        key_tuple = (generalized_ret_type, has_target, *generalized_arg_types)
        return key_tuple

    def analyze_archetypes(self):
        """Groups all wrappable functions by their signature archetype."""
        for func in self.functions:
            if self._is_wrappable(func):
                key = self._get_archetype_key(func)
                self.archetypes[key].append(func)

    def _get_generalized_c_type(self, generalized_type):
        """Maps our category back to a C type for the dispatcher's function signature."""
        return {
            'VOID': 'void',
            'INT_LIKE': 'intptr_t',
            'STRING': 'const char*',
            'OBJ_PTR': 'lv_obj_t*', # Return obj_t* for simplicity, C will handle the cast.
            'COLOR': 'lv_color_t',
            'OTHER_PTR': 'void*',
        }.get(generalized_type, 'intptr_t')

    def _get_parser_for_type(self, c_type, json_var, arg_index):
        """Gets the C code snippet to parse a cJSON value into a C type."""
        c_type_no_const = c_type.replace('const ', '').strip()

        if c_type_no_const in self.enum_types:
            return f'unmarshal_value(cJSON_GetArrayItem({json_var}, {arg_index}), "{c_type_no_const}")'
        if self._get_generalized_type(c_type) == 'INT_LIKE':
             return f'(intptr_t)cJSON_GetNumberValue(cJSON_GetArrayItem({json_var}, {arg_index}))'
        if c_type == 'const char*':
            return f'cJSON_GetStringValue(cJSON_GetArrayItem({json_var}, {arg_index}))'
        # Any LVGL pointer is assumed to be in the object registry
        if c_type.startswith('lv_') and c_type.endswith('_t*'):
            return f'({c_type})obj_registry_get(cJSON_GetStringValue(cJSON_GetArrayItem({json_var}, {arg_index})))'
        if c_type == 'lv_color_t':
             return f'lv_color_hex((uint32_t)cJSON_GetNumberValue(cJSON_GetArrayItem({json_var}, {arg_index})))'
        if c_type.endswith('*'):
             return f'({c_type})obj_registry_get(cJSON_GetStringValue(cJSON_GetArrayItem({json_var}, {arg_index})))'

        return f'(intptr_t)cJSON_GetNumberValue(cJSON_GetArrayItem({json_var}, {arg_index}))'

    def generate_files(self, header_path, source_path):
        """Generates and writes both the .h and .c files."""
        self._write_header_file(header_path)
        self._write_source_file(source_path)

    def _write_header_file(self, path):
        with open(path, 'w', encoding='utf-8') as f:
            f.write(
"""/*
 * AUTO-GENERATED by the LVGL API parser script.
 * DO NOT EDIT MANUALLY.
 */
#ifndef DYNAMIC_LVGL_H
#define DYNAMIC_LVGL_H

#ifdef __cplusplus
extern "C" {
#endif

#include "lvgl.h"
#include "cJSON.h"

/**
 * @brief Initializes the object registry. Call this once.
 */
void obj_registry_init(void);

/**
 * @brief Adds a named object to the registry.
 * @param id The string ID for the object.
 * @param obj The lv_obj_t pointer.
 */
void obj_registry_add(const char* id, void* obj);

/**
 * @brief Retrieves an object from the registry by its ID.
 * @param id The string ID of the object.
 * @return The pointer or NULL if not found.
 */
void* obj_registry_get(const char* id);

/**
 * @brief Cleans up the object registry.
 */
void obj_registry_deinit(void);


/**
 * @brief The main entry point to dynamically call an LVGL function.
 *
 * @param func_name The name of the LVGL function to call (e.g., "lv_obj_set_width").
 * @param target_obj The target object for the function. For creation functions (e.g., lv_label_create),
 *                   this is the parent. For methods, this is the object instance. Can be NULL for
 *                   functions without a target object (e.g., lv_pct).
 * @param args A cJSON array containing the arguments for the function. For a function like
 *             lv_obj_set_width(obj, 100), 'args' would be a cJSON array with one number: [100].
 * @return A new lv_obj_t* if the called function was an object creator, otherwise NULL.
 *         Note: For non-object returns, the value is discarded and NULL is returned.
 */
lv_obj_t* dynamic_lvgl_call(const char* func_name, void* target_obj, cJSON* args);

/**
 * @brief Unmarshals a JSON value to an enum. This function must be implemented by the user.
 *        It should contain a series of if-else checks or a map to convert the string
 *        `expected_enum_type_for_arg` into a call to a specific enum parser.
 *
 * @param value The cJSON value (should be a string or a number).
 * @param expected_enum_type_for_arg A string naming the enum type, e.g., "lv_align_t".
 * @return The integer value of the enum.
 */
int unmarshal_value(cJSON* value, const char* expected_enum_type_for_arg);

#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif // DYNAMIC_LVGL_H
""")

    def _write_source_file(self, path):
        with open(path, 'w', encoding='utf-8') as f:
            f.write(
"""/*
 * AUTO-GENERATED by the LVGL API parser script.
 * DO NOT EDIT MANUALLY.
 */
#include "dynamic_lvgl.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

// Generic function pointer and dispatcher signature
typedef void (*generic_lvgl_func_t)(void);
typedef lv_obj_t* (*lvgl_dispatcher_t)(generic_lvgl_func_t fn, void* target, cJSON* args);

// Forward declarations of all archetype dispatchers
""")
            # Generate forward declarations for dispatchers
            self.archetype_map = {}
            for i, key in enumerate(self.archetypes.keys()):
                dispatcher_name = f"dispatch_archetype_{i}"
                self.archetype_map[key] = dispatcher_name
                f.write(f"static lv_obj_t* {dispatcher_name}(generic_lvgl_func_t fn, void* target, cJSON* args);\n")

            f.write("\n// --- Archetype Dispatcher Implementations ---\n")

            for i, (key, func_list) in enumerate(self.archetypes.items()):
                dispatcher_name = self.archetype_map[key]
                generalized_ret_type, has_target, *generalized_arg_types = key

                first_func = func_list[0]
                args_for_comment = ", ".join(generalized_arg_types)
                f.write(f"// Archetype for {len(func_list)} funcs like: {first_func['return_type']} {first_func['name']}({args_for_comment})\n")
                f.write(f"static lv_obj_t* {dispatcher_name}(generic_lvgl_func_t fn, void* target, cJSON* args) {{\n")

                arg_list = first_func['args'][1:] if has_target else first_func['args']

                for j, arg_info in enumerate(arg_list):
                    c_type = arg_info['type']
                    f.write(f"    {self._get_generalized_c_type(generalized_arg_types[j])} arg{j} = {self._get_parser_for_type(c_type, 'args', j)};\n")

                c_ret_type_for_typedef = self._get_generalized_c_type(generalized_ret_type)

                c_call_arg_types = []
                if has_target:
                    c_call_arg_types.append("void*")
                c_call_arg_types.extend([self._get_generalized_c_type(t) for t in generalized_arg_types])

                f.write(f"    typedef {c_ret_type_for_typedef} (*specific_func_t)({', '.join(c_call_arg_types) if c_call_arg_types else 'void'});\n")

                call_params = []
                if has_target:
                    call_params.append("target")
                call_params.extend([f"arg{j}" for j in range(len(arg_list))])
                call_str = f"((specific_func_t)fn)({', '.join(call_params)})"

                if generalized_ret_type == "VOID":
                    f.write(f"    {call_str};\n    return NULL;\n")
                elif generalized_ret_type == "OBJ_PTR":
                    f.write(f"    return (lv_obj_t*){call_str};\n")
                else:
                    f.write(f"    (void){call_str};\n    return NULL;\n")
                f.write("}\n\n")

            # --- Registry ---
            f.write(
"""
// --- Function Registry ---
typedef struct {
    const char* name;
    lvgl_dispatcher_t dispatcher;
    generic_lvgl_func_t func_ptr;
} FunctionMapping;
""")

            registry_entries = []
            for key, func_list in self.archetypes.items():
                dispatcher_name = self.archetype_map[key]
                for func_info in func_list:
                    registry_entries.append(
                        f'{{"{func_info["name"]}", {dispatcher_name}, (generic_lvgl_func_t){func_info["name"]}}}'
                    )
            registry_entries.sort()

            f.write("\nstatic const FunctionMapping function_registry[] = {\n    ")
            f.write(",\n    ".join(registry_entries))
            f.write("\n};\n\n")

            # --- Dispatch Logic & Object Registry ---
            f.write(
"""
// --- Dispatch Logic ---
static int compare_func_mappings(const void* a, const void* b) {
    return strcmp((const char*)a, ((const FunctionMapping*)b)->name);
}

lv_obj_t* dynamic_lvgl_call(const char* func_name, void* target_obj, cJSON* args) {
    if (!func_name) {
        return NULL;
    }

    const FunctionMapping* mapping = (const FunctionMapping*)bsearch(
        func_name,
        function_registry,
        sizeof(function_registry) / sizeof(FunctionMapping),
        sizeof(FunctionMapping),
        compare_func_mappings
    );

    if (mapping) {
        return mapping->dispatcher(mapping->func_ptr, target_obj, args);
    }

    LV_LOG_WARN("Dynamic LVGL call failed: function '%s' not found in registry.", func_name);
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
    if (strcmp(id, "SCREEN_ACTIVE") == 0) return lv_screen_active();
    if (strcmp(id, "NULL") == 0) return NULL;

    for (int i = 0; i < obj_registry_count; i++) {
        if (strcmp(obj_registry[i].id, id) == 0) {
            return obj_registry[i].obj;
        }
    }
    LV_LOG_WARN("Object with ID '%s' not found.", id);
    return NULL;
}

void obj_registry_deinit(void) {
    for (int i = 0; i < obj_registry_count; i++) {
        free(obj_registry[i].id);
    }
    obj_registry_init();
}
""")

def main(filepath):
    """
    Loads the API spec file, runs the parser, and prints the result to stdout.
    Also generates C header and source files for the dynamic dispatcher.
    """
    try:
        with open(filepath, 'r', encoding='utf-8') as f:
            api_spec = json.load(f)
    except FileNotFoundError:
        print(f"Error: File not found at '{filepath}'", file=sys.stderr)
        sys.exit(1)
    except json.JSONDecodeError as e:
        print(f"Error: Could not decode JSON from '{filepath}': {e}", file=sys.stderr)
        sys.exit(1)

    # --- Step 1: Run the original parser and print its JSON output ---
    parser = LVGLApiParser(api_spec)
    translated_spec = parser.parse()
    json.dump(translated_spec, sys.stdout, indent=2)

    # --- Step 2: Run the C code generator ---
    print(f"\n--- C Code Generation ---", file=sys.stderr)
    generator = CCodeGenerator(translated_spec)

    print("Analyzing function archetypes with aggressive grouping...", file=sys.stderr)
    generator.analyze_archetypes()
    print(f"Found {len(generator.archetypes)} unique function archetypes.", file=sys.stderr)

    header_path = "dynamic_lvgl.h"
    source_path = "dynamic_lvgl.c"
    print(f"Generating {header_path} and {source_path}...", file=sys.stderr)
    generator.generate_files(header_path, source_path)
    print("Done.", file=sys.stderr)


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: python {sys.argv[0]} <path_to_api_spec.json>", file=sys.stderr)
        sys.exit(1)

    main(sys.argv[1])

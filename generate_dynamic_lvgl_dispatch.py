import json
import re
import sys
import argparse # For command-line arguments
from collections import defaultdict

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

    def __init__(self, translated_spec, consolidation_mode="typesafe"):
        self.spec = translated_spec
        self.consolidation_mode = consolidation_mode
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

    def _is_obj_ptr(self, type_str):
        # Helper for typesafe mode
        return type_str.startswith('lv_') and type_str.endswith('_t*') and type_str != 'lv_event_t*'

    def _is_wrappable(self, func_info):
        """Determines if a function can be automatically wrapped."""
        if '...' in str(func_info['args']): return False # Variadic functions
        if not func_info['name']: return False
        for arg in func_info['args']:
            arg_type = arg['type']
            if '(*' in arg_type: return False # Function pointers
            # Allow void* only in aggressive mode, where it becomes OTHER_PTR
            if (arg_type == 'void*' or arg_type == 'const void*') and self.consolidation_mode == "typesafe":
                return False
        return True

    def _get_generalized_type_typesafe(self, c_type):
        c_type_no_const = c_type.replace('const ', '').strip()
        if c_type_no_const in self.enum_types: return 'INT_LIKE'
        if c_type_no_const.endswith('_t') and 'int' in c_type_no_const: return 'INT_LIKE'
        if c_type_no_const in ['bool', 'char', 'short', 'int', 'long']: return 'INT_LIKE'
        if c_type == 'const char*': return 'STRING'
        if self._is_obj_ptr(c_type): return 'OBJ_PTR' # Specific LVGL object pointers
        if c_type == 'lv_color_t': return 'COLOR'
        if c_type.endswith('*'): return 'OTHER_PTR' # Other non-LVGL pointers
        return 'INT_LIKE'

    def _get_generalized_type_aggressive(self, c_type):
        if c_type == "void": return "VOID" # Needed for return types
        c_type_no_const = c_type.replace('const ', '').strip()
        if c_type_no_const in self.enum_types: return 'INT_LIKE'
        if c_type_no_const.endswith('_t') and 'int' in c_type_no_const: return 'INT_LIKE'
        if c_type_no_const in ['bool', 'char', 'short', 'int', 'long']: return 'INT_LIKE'
        if c_type == 'const char*': return 'STRING'
        # Broader category for ANY LVGL object/struct pointer or generic void*
        if (c_type.startswith('lv_') and c_type.endswith('_t*')) or c_type.endswith('*'): # Catches lv_obj_t*, other lv_..._t*, and void*
            return 'ANY_POINTER'
        if c_type == 'lv_color_t': return 'COLOR'
        # Non-pointer structs/types are treated as INT_LIKE
        return 'INT_LIKE'


    def _get_generalized_type(self, c_type):
        if self.consolidation_mode == "aggressive":
            return self._get_generalized_type_aggressive(c_type)
        else: # typesafe
            return self._get_generalized_type_typesafe(c_type)

    def _get_archetype_key_typesafe(self, func_info):
        ret_type = func_info['return_type']
        args = func_info['args']
        # Target object is specifically an lv_obj_t* or similar known widget type
        has_target_obj = args and self._is_obj_ptr(args[0]['type'])
        target_c_type = args[0]['type'] if has_target_obj else 'void'
        arg_list = args[1:] if has_target_obj else args
        generalized_arg_types = [self._get_generalized_type(arg['type']) for arg in arg_list]
        return (ret_type, target_c_type, *generalized_arg_types)

    def _get_archetype_key_aggressive(self, func_info):
        generalized_ret_type = self._get_generalized_type(func_info['return_type'])
        args = func_info['args']
        # Target object is any pointer for aggressive mode
        has_target = bool(args and args[0]['type'].endswith('*'))
        arg_list = args[1:] if has_target else args
        generalized_arg_types = [self._get_generalized_type(arg['type']) for arg in arg_list]
        # The 'target_c_type' equivalent for aggressive is just 'bool has_target'
        return (generalized_ret_type, has_target, *generalized_arg_types)

    def _get_archetype_key(self, func_info):
        if self.consolidation_mode == "aggressive":
            return self._get_archetype_key_aggressive(func_info)
        else: # typesafe
            return self._get_archetype_key_typesafe(func_info)

    def analyze_archetypes(self):
        """Groups all wrappable functions by their signature archetype."""
        self.archetypes.clear()
        for func in self.functions:
            if self._is_wrappable(func):
                key = self._get_archetype_key(func)
                self.archetypes[key].append(func)

    def _get_generalized_c_type(self, generalized_type):
        # This mapping needs to be compatible with both modes,
        # or have conditional logic if truly necessary.
        # For now, 'ANY_POINTER' maps to void* for function signatures.
        # OBJ_PTR maps to lv_obj_t* for typesafe.
        if self.consolidation_mode == "aggressive":
            return {
                'VOID': 'void',
                'INT_LIKE': 'intptr_t',
                'STRING': 'const char*',
                'ANY_POINTER': 'void*', # All pointers are void* in aggressive dispatcher signatures
                'COLOR': 'lv_color_t',
            }.get(generalized_type, 'intptr_t')
        else: # typesafe
            return {
                'INT_LIKE': 'intptr_t',
                'STRING': 'const char*',
                'OBJ_PTR': 'lv_obj_t*',
                'COLOR': 'lv_color_t',
                'OTHER_PTR': 'void*',
            }.get(generalized_type, 'intptr_t')


    def _get_parser_for_type(self, c_type, json_var, arg_index):
        """Gets the C code snippet to parse a cJSON value into a C type."""
        c_type_no_const = c_type.replace('const ', '').strip()

        if c_type_no_const in self.enum_types:
            return f'unmarshal_value(cJSON_GetArrayItem({json_var}, {arg_index}), "{c_type_no_const}")'

        # Generalized type helps decide the cJSON function and potential cast
        gen_type = self._get_generalized_type(c_type)

        if gen_type == 'INT_LIKE':
             return f'({c_type_no_const if c_type_no_const != "bool" else "int"})cJSON_GetNumberValue(cJSON_GetArrayItem({json_var}, {arg_index}))' # Cast to original type for enums/small ints
        if gen_type == 'STRING': # const char*
            return f'cJSON_GetStringValue(cJSON_GetArrayItem({json_var}, {arg_index}))'

        # Pointer types:
        if self.consolidation_mode == "typesafe":
            if self._is_obj_ptr(c_type): # Specific lv_obj_t* etc.
                return f'({c_type})obj_registry_get(cJSON_GetStringValue(cJSON_GetArrayItem({json_var}, {arg_index})))'
            elif c_type.endswith('*'): # Other pointers (e.g. lv_font_t*)
                 return f'({c_type})obj_registry_get(cJSON_GetStringValue(cJSON_GetArrayItem({json_var}, {arg_index})))' # Assume they are also in registry for now
        else: # aggressive
            if gen_type == 'ANY_POINTER': # All pointers are looked up
                 return f'({c_type})obj_registry_get(cJSON_GetStringValue(cJSON_GetArrayItem({json_var}, {arg_index})))'


        if c_type == 'lv_color_t': # lv_color_t
             return f'lv_color_hex((uint32_t)cJSON_GetNumberValue(cJSON_GetArrayItem({json_var}, {arg_index})))'

        # Default for simple types not explicitly handled, or structs passed by value (treated as int-like)
        return f'({c_type})cJSON_GetNumberValue(cJSON_GetArrayItem({json_var}, {arg_index}))'


    def generate_files(self, header_path, source_path):
        """Generates and writes both the .h and .c files."""
        self._write_header_file(header_path)
        self._write_source_file(source_path)

    def _write_header_file(self, path):
        with open(path, 'w', encoding='utf-8') as f:
            f.write(
"""/*
 * AUTO-GENERATED by generate_dynamic_lvgl_dispatch.py.
 * Consolidation: {mode}
 * DO NOT EDIT MANUALLY.
 */
#ifndef DYNAMIC_LVGL_H
#define DYNAMIC_LVGL_H

#ifdef __cplusplus
extern "C" {{
#endif

#include "lvgl.h"
#include "cJSON.h" // For cJSON based calls

// Forward declare IR node type if IR input is enabled
#if defined(ENABLE_IR_INPUTS)
// We need the full definition if we are passing IR** args,
// but ir.h might not be included by the user of dynamic_lvgl.h.
// For now, assume dynamic_lvgl.c will include ir.h.
// Users of dynamic_lvgl_call_ir will need to include ir.h themselves.
typedef struct IRNode IRNode; // Changed from typedef struct IR IR;
#endif

void obj_registry_init(void);
""".format(mode=self.consolidation_mode))

            # Object registry functions are typed based on mode
            obj_ptr_type = "lv_obj_t*" if self.consolidation_mode == "typesafe" else "void*"
            f.write(f"void obj_registry_add(const char* id, {obj_ptr_type} obj);\n")
            f.write(f"{obj_ptr_type} obj_registry_get(const char* id);\n")
            f.write("void obj_registry_deinit(void);\n\n")

            f.write("#if defined(ENABLE_CJSON_INPUTS)\n")
            f.write(
"""
lv_obj_t* dynamic_lvgl_call_json(const char* func_name, {target_obj_type} target_obj, cJSON* args);

int unmarshal_value(cJSON* value, const char* expected_enum_type_for_arg);
""".format(target_obj_type=obj_ptr_type))
            f.write("#endif // ENABLE_CJSON_INPUTS\n\n")

            f.write("#if defined(ENABLE_IR_INPUTS)\n")
            f.write(
"""
lv_obj_t* dynamic_lvgl_call_ir(const char* func_name, {target_obj_type} target_obj, IRNode** ir_args, int arg_count); // Changed IR** to IRNode**
// Note: IR based calls will require ir.h to be included by the caller.
// Enum unmarshalling from IR nodes will be handled directly or via a new helper if needed.
""".format(target_obj_type=obj_ptr_type))
            f.write("#endif // ENABLE_IR_INPUTS\n\n")

            f.write(
"""
#ifdef __cplusplus
} /*extern "C"*/
#endif

#endif // DYNAMIC_LVGL_H
""")

    def _write_source_file(self, path):
        with open(path, 'w', encoding='utf-8') as f:
            f.write(
"""/*
 * AUTO-GENERATED by generate_dynamic_lvgl_dispatch.py.
 * Consolidation: {mode}
 * DO NOT EDIT MANUALLY.
 */
#include "dynamic_lvgl.h"
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#if defined(ENABLE_IR_INPUTS)
#include "ir.h" // For IR struct definition
#endif

// Generic function pointer
typedef void (*generic_lvgl_func_t)(void);

// Dispatcher signatures will differ for cJSON and IR
#if defined(ENABLE_CJSON_INPUTS)
typedef lv_obj_t* (*lvgl_json_dispatcher_t)(generic_lvgl_func_t fn, {target_obj_type} target, cJSON* args);
#endif
#if defined(ENABLE_IR_INPUTS)
typedef lv_obj_t* (*lvgl_ir_dispatcher_t)(generic_lvgl_func_t fn, {target_obj_type} target, IRNode** ir_args, int arg_count); // Changed IR** to IRNode**
#endif

// Forward declarations of all archetype dispatchers
""".format(mode=self.consolidation_mode,
           target_obj_type="lv_obj_t*" if self.consolidation_mode == "typesafe" else "void*"))

            self.archetype_map = {} # Reset for current mode
            for i, key in enumerate(self.archetypes.keys()):
                dispatcher_name_json = f"dispatch_json_archetype_{i}"
                dispatcher_name_ir = f"dispatch_ir_archetype_{i}"
                self.archetype_map[key] = (dispatcher_name_json, dispatcher_name_ir)
                f.write(f"#if defined(ENABLE_CJSON_INPUTS)\n")
                f.write(f"static lv_obj_t* {dispatcher_name_json}(generic_lvgl_func_t fn, {'lv_obj_t*' if self.consolidation_mode == 'typesafe' else 'void*'} target, cJSON* args);\n")
                f.write(f"#endif\n")
                f.write(f"#if defined(ENABLE_IR_INPUTS)\n")
                f.write(f"static lv_obj_t* {dispatcher_name_ir}(generic_lvgl_func_t fn, {'lv_obj_t*' if self.consolidation_mode == 'typesafe' else 'void*'} target, IRNode** ir_args, int arg_count);\n") # Changed IR** to IRNode**
                f.write(f"#endif\n\n")


            f.write("\n// --- Archetype Dispatcher Implementations ---\n")

            for i, (key, func_list) in enumerate(self.archetypes.items()):
                dispatcher_name_json, dispatcher_name_ir = self.archetype_map[key]
                first_func = func_list[0]

                # Common parts for the archetype
                if self.consolidation_mode == "typesafe":
                    ret_type, target_c_type_key, *generalized_arg_types_key = key
                    original_args = (first_func['args'][1:] if target_c_type_key != 'void' else first_func['args'])
                    comment_args_str = ", ".join(f"{self._get_generalized_c_type(t)} ({t})" for t in generalized_arg_types_key)
                    target_param_type_c = target_c_type_key
                    target_param_name = "target"
                else: # aggressive
                    generalized_ret_type_key, has_target_key, *generalized_arg_types_key = key
                    ret_type = first_func['return_type'] # Use actual return type for typedef
                    original_args = (first_func['args'][1:] if has_target_key else first_func['args'])
                    comment_args_str = ", ".join(f"{self._get_generalized_c_type(t)} ({t})" for t in generalized_arg_types_key)
                    target_param_type_c = "void*" # Target is always void* for aggressive dispatcher
                    target_param_name = "target_voidp"


                f.write(f"// Archetype for {len(func_list)} funcs like {first_func['return_type']} {first_func['name']}({comment_args_str})\n")

                # --- JSON Dispatcher ---
                f.write(f"#if defined(ENABLE_CJSON_INPUTS)\n")
                f.write(f"static lv_obj_t* {dispatcher_name_json}(generic_lvgl_func_t fn, {target_param_type_c} {target_param_name}, cJSON* args) {{\n")

                # Argument parsing for JSON
                for j, arg_info in enumerate(original_args):
                    actual_c_type = arg_info['type']
                    # Use generalized C type for the variable, but parse for actual C type
                    var_c_type = self._get_generalized_c_type(generalized_arg_types_key[j])
                    f.write(f"    {var_c_type} arg{j} = {self._get_parser_for_type(actual_c_type, 'args', j)};\n")

                # Typedef for the specific function call for JSON
                c_call_arg_types = [first_func['args'][0]['type']] if (original_args != first_func['args'] and first_func['args']) else [] # Actual type of target
                c_call_arg_types.extend([arg['type'] for arg in original_args])
                f.write(f"    typedef {ret_type} (*specific_func_t)({', '.join(c_call_arg_types) if c_call_arg_types else 'void'});\n")

                # Generate the call for JSON
                call_params = [target_param_name] if (original_args != first_func['args'] and first_func['args']) else []
                # Cast each arg to its specific original type for the call
                for j, arg_info in enumerate(original_args):
                    call_params.append(f"({arg_info['type']})arg{j}")

                call_str = f"((specific_func_t)fn)({', '.join(call_params)})"

                if ret_type == "void":
                    f.write(f"    {call_str};\n    return NULL;\n")
                elif self._is_obj_ptr(ret_type) or (self.consolidation_mode == "aggressive" and self._get_generalized_type(ret_type) == "ANY_POINTER"):
                    f.write(f"    return (lv_obj_t*){call_str};\n") # Assume obj_ptr returns are lv_obj_t* for the dynamic_call return
                else:
                    f.write(f"    (void){call_str};\n    return NULL;\n")
                f.write("}\n")
                f.write(f"#endif // ENABLE_CJSON_INPUTS\n\n")

                # --- IR Dispatcher ---
                f.write(f"#if defined(ENABLE_IR_INPUTS)\n")
                f.write(f"static lv_obj_t* {dispatcher_name_ir}(generic_lvgl_func_t fn, {target_param_type_c} {target_param_name}, IRNode** ir_args, int arg_count) {{\n") # Changed IR** to IRNode**

                # Argument parsing for IR
                # Check arg_count against expected number of args
                f.write(f"    if (arg_count != {len(original_args)}) {{\n")
                f.write(f"        LV_LOG_WARN(\"IR call to {first_func['name']}: expected {len(original_args)} args, got %d\", arg_count);\n")
                f.write(f"        return NULL;\n")
                f.write(f"    }}\n\n")

                for j, arg_info in enumerate(original_args):
                    actual_c_type = arg_info['type']
                    var_c_type = self._get_generalized_c_type(generalized_arg_types_key[j])
                    parser_code = self._get_parser_for_ir_node_type(actual_c_type, 'ir_args', j)
                    f.write(f"    {var_c_type} arg{j}; // Default init\n")
                    f.write(f"    IRNode* current_ir_arg{j} = ir_args[{j}];\n")
                    f.write(f"    if (!current_ir_arg{j}) {{\n")
                    f.write(f"        LV_LOG_WARN(\"IR call to {first_func['name']}: arg {j} is NULL\");\n")
                    f.write(f"        return NULL; // Or handle error appropriately\n")
                    f.write(f"    }}\n")
                    f.write(f"    arg{j} = {parser_code};\n\n")


                # Typedef for the specific function call for IR (same as JSON)
                c_call_arg_types_ir = [first_func['args'][0]['type']] if (original_args != first_func['args'] and first_func['args']) else []
                c_call_arg_types_ir.extend([arg['type'] for arg in original_args])
                f.write(f"    typedef {ret_type} (*specific_func_t)({', '.join(c_call_arg_types_ir) if c_call_arg_types_ir else 'void'});\n")

                # Generate the call for IR (same as JSON, but with IR-parsed args)
                call_params_ir = [target_param_name] if (original_args != first_func['args'] and first_func['args']) else []
                for j, arg_info in enumerate(original_args):
                    call_params_ir.append(f"({arg_info['type']})arg{j}") # Cast to specific type
                call_str_ir = f"((specific_func_t)fn)({', '.join(call_params_ir)})"

                if ret_type == "void":
                    f.write(f"    {call_str_ir};\n    return NULL;\n")
                elif self._is_obj_ptr(ret_type) or \
                     (self.consolidation_mode == "aggressive" and self._get_generalized_type(ret_type) == "ANY_POINTER"):
                    f.write(f"    return (lv_obj_t*){call_str_ir};\n")
                else: # Other return types
                    f.write(f"    // For non-pointer, non-void return types, the value is returned as intptr_t\n")
                    f.write(f"    // but dynamic_lvgl_call_ir returns lv_obj_t*, so we cast to void and return NULL.\n")
                    f.write(f"    // If specific return values are needed, dynamic_lvgl_call_ir signature must change.\n")
                    f.write(f"    (void){call_str_ir};\n")
                    f.write(f"    return NULL;\n")
                f.write("}\n")
                f.write(f"#endif // ENABLE_IR_INPUTS\n\n")

            # --- Helper function for IR enum/define parsing (Python side) ---
    def _get_parser_for_ir_node_type(self, c_type, ir_args_var, arg_index):
        """Gets the C code snippet to parse an IRNode* into a C type."""
        c_type_no_const = c_type.replace('const ', '').strip()
        ir_node_accessor = f"{ir_args_var}[{arg_index}]"

        # Check node type first
        # This is a simplified version. A more robust solution would check ir_node->base.type
        # and then access the correct field of the specific IRExpr* type.
        # For now, we assume literals are handled by value_str/value_int like in older IR.
        # This part will need significant refinement based on the actual IR structure in use.

        # Enums: Assume they are passed as string literals of the enum member name
        # or as integer literals.
        if c_type_no_const in self.enum_types:
            # This requires a C helper function `unmarshal_ir_enum_value`
            # similar to `unmarshal_value` for cJSON, but taking `IRNode*`
            # and resolving defines/enum members.
            # For now, let's assume a simpler direct mapping or that the IR carries the int value.
            # Simplistic: if it's a string, look it up. If int, use it.
            # This will need a proper C helper: resolve_enum_from_ir_node(ir_node, "lv_align_t")
            return f"({c_type_no_const})ir_node_get_int_robust({ir_node_accessor}, \"{c_type_no_const}\")"


        gen_type = self._get_generalized_type(c_type)

        if gen_type == 'INT_LIKE':
            # bool, char, short, int, long, and int-like typedefs (like lv_coord_t)
            # also other enums not caught above (if they are passed as numbers)
            if c_type_no_const == "bool":
                return f"(bool)ir_node_get_int({ir_node_accessor})" # Assuming ir_node_get_int handles "true"/"false" or 0/1
            else:
                return f"({c_type_no_const})ir_node_get_int({ir_node_accessor})"

        if gen_type == 'STRING': # const char*
            # Assumes IRNode has a value_str or similar for string literals
            return f"({c_type})ir_node_get_string({ir_node_accessor})"

        # Pointer types:
        if self.consolidation_mode == "typesafe":
            if self._is_obj_ptr(c_type): # lv_obj_t*, lv_img_t* etc.
                return f"({c_type})obj_registry_get(ir_node_get_string({ir_node_accessor}))"
            elif c_type.endswith('*'): # Other pointers (e.g. lv_font_t*, void*)
                 return f"({c_type})obj_registry_get(ir_node_get_string({ir_node_accessor}))"
        else: # aggressive
            if gen_type == 'ANY_POINTER': # All pointers including void*
                 return f"({c_type})obj_registry_get(ir_node_get_string({ir_node_accessor}))"

        if c_type == 'lv_color_t':
            # Assuming color is passed as an integer (0xRRGGBB) in the IR node
            return f"lv_color_hex((uint32_t)ir_node_get_int({ir_node_accessor}))"

        # Default for simple types or structs passed by value (treated as int-like)
        # This is a fallback and might not be correct for all cases (e.g. structs by value)
        return f"({c_type})ir_node_get_int({ir_node_accessor})"

            # --- Function Registry and Dispatch Logic ---


            # --- Function Registry and Dispatch Logic ---
            obj_ptr_type_c = "lv_obj_t*" if self.consolidation_mode == "typesafe" else "void*"

            f.write(
"""
// --- Function Registry ---
typedef struct {
    const char* name;
#if defined(ENABLE_CJSON_INPUTS)
    lvgl_json_dispatcher_t json_dispatcher;
#endif
#if defined(ENABLE_IR_INPUTS)
    lvgl_ir_dispatcher_t ir_dispatcher;
#endif
    generic_lvgl_func_t func_ptr;
} FunctionMapping;
""")

            registry_entries = []
            for key, func_list in self.archetypes.items():
                dispatcher_name_json, dispatcher_name_ir = self.archetype_map[key]
                for func_info in func_list:
                    entry = f'{{"{func_info["name"]}", '
                    entry += f"{dispatcher_name_json}, " if "ENABLE_CJSON_INPUTS" else ""
                    entry += f"{dispatcher_name_ir}, " if "ENABLE_IR_INPUTS" else ""
                    entry += f'(generic_lvgl_func_t){func_info["name"]}}}'
                    registry_entries.append(entry)
            registry_entries.sort()

            f.write("\nstatic const FunctionMapping function_registry[] = {\n    ")
            f.write(",\n    ".join(registry_entries))
            f.write("\n};\n\n")

            f.write(
"""
// --- Dispatch Logic ---
static int compare_func_mappings(const void* a, const void* b) {
    return strcmp((const char*)a, ((const FunctionMapping*)b)->name);
}

#if defined(ENABLE_CJSON_INPUTS)
lv_obj_t* dynamic_lvgl_call_json(const char* func_name, {target_obj_type} target_obj, cJSON* args) {{
    if (!func_name) return NULL;
    const FunctionMapping* mapping = (const FunctionMapping*)bsearch(
        func_name, function_registry,
        sizeof(function_registry) / sizeof(FunctionMapping),
        sizeof(FunctionMapping), compare_func_mappings
    );
    if (mapping && mapping->json_dispatcher) {{
        return mapping->json_dispatcher(mapping->func_ptr, target_obj, args);
    }}
    LV_LOG_WARN("Dynamic LVGL JSON call failed: function '%s' not found or dispatcher missing.", func_name);
    return NULL;
}}
#endif // ENABLE_CJSON_INPUTS

#if defined(ENABLE_IR_INPUTS)
lv_obj_t* dynamic_lvgl_call_ir(const char* func_name, {target_obj_type} target_obj, IRNode** ir_args, int arg_count) {{ # Changed IR** to IRNode**
    if (!func_name) return NULL;
    const FunctionMapping* mapping = (const FunctionMapping*)bsearch(
        func_name, function_registry,
        sizeof(function_registry) / sizeof(FunctionMapping),
        sizeof(FunctionMapping), compare_func_mappings
    );
    if (mapping && mapping->ir_dispatcher) {{
        return mapping->ir_dispatcher(mapping->func_ptr, target_obj, ir_args, arg_count);
    }}
    LV_LOG_WARN("Dynamic LVGL IR call failed: function '%s' not found or dispatcher missing.", func_name);
    return NULL;
}}
#endif // ENABLE_IR_INPUTS

// --- Simple Object Registry Implementation ---
#ifndef DYNAMIC_LVGL_MAX_OBJECTS
#define DYNAMIC_LVGL_MAX_OBJECTS 256
#endif

typedef struct {{
    char* id;
    {target_obj_type} obj;
}} ObjectEntry;

static ObjectEntry obj_registry[DYNAMIC_LVGL_MAX_OBJECTS];
static int obj_registry_count = 0;

void obj_registry_init(void) {{
    obj_registry_count = 0;
    memset(obj_registry, 0, sizeof(obj_registry));
}}

void obj_registry_add(const char* id, {target_obj_type} obj) {{
    if (obj_registry_count >= DYNAMIC_LVGL_MAX_OBJECTS || !id) {{
        LV_LOG_WARN("Cannot add object to registry: full or null ID");
        return;
    }}
    for (int i = 0; i < obj_registry_count; i++) {{
        if (strcmp(obj_registry[i].id, id) == 0) {{
            obj_registry[i].obj = obj;
            return;
        }}
    }}
    obj_registry[obj_registry_count].id = strdup(id);
    obj_registry[obj_registry_count].obj = obj;
    obj_registry_count++;
}}

{target_obj_type} obj_registry_get(const char* id) {{
    if (!id) return NULL;
    // Special case for SCREEN_ACTIVE, always returns lv_obj_t*
    if (strcmp(id, "SCREEN_ACTIVE") == 0) return ({target_obj_type})lv_screen_active();
    if (strcmp(id, "NULL") == 0) return NULL;

    for (int i = 0; i < obj_registry_count; i++) {{
        if (strcmp(obj_registry[i].id, id) == 0) {{
            return obj_registry[i].obj;
        }}
    }}
    LV_LOG_WARN("Object with ID '%s' not found.", id);
    return NULL;
}}

void obj_registry_deinit(void) {{
    for (int i = 0; i < obj_registry_count; i++) {{
        if(obj_registry[i].id) free(obj_registry[i].id);
    }}
    obj_registry_init();
}}

#if defined(ENABLE_IR_INPUTS)
// Helper functions to extract values from IRNodes robustly.
// These are simplified and assume IRNode has value_str and value_int.
// A more complete implementation would check IRNode->base.type (e.g. IR_EXPR_LITERAL)
// and then access the appropriate fields of the specific IRExprLiteral struct.

static intptr_t ir_node_get_int(IRNode* node) {
    if (!node) return 0;
    // This is a placeholder. Actual implementation depends on how int literals are stored in IRNode.
    // Assuming IRExprLiteral with a 'value' string that needs parsing.
    // Or, if IRNode directly stores int for IR_EXPR_LITERAL of type int.
    if (node->type == IR_EXPR_LITERAL) {
        IRExprLiteral* lit = (IRExprLiteral*)node;
        if (lit->value) {
            // Check for boolean strings
            if (strcmp(lit->value, "true") == 0) return 1;
            if (strcmp(lit->value, "false") == 0) return 0;
            return (intptr_t)strtol(lit->value, NULL, 0); // Supports decimal, hex (0x), octal (0)
        }
    }
    LV_LOG_WARN("ir_node_get_int: Could not parse int from IRNode type %d", node->type);
    return 0;
}

static const char* ir_node_get_string(IRNode* node) {
    if (!node) return NULL;
    // Placeholder:
    if (node->type == IR_EXPR_LITERAL) {
        IRExprLiteral* lit = (IRExprLiteral*)node;
        // Assuming lit->value for strings includes quotes, e.g. "\"text\""
        // This helper should ideally strip them if the registry expects "text"
        // For now, pass it as is.
        return lit->value;
    } else if (node->type == IR_EXPR_VARIABLE) { // If ID is stored as a variable name
        IRExprVariable* var = (IRExprVariable*)node;
        return var->name;
    }
    LV_LOG_WARN("ir_node_get_string: Could not parse string from IRNode type %d", node->type);
    return NULL;
}

// More robust version for enums/defines that might be strings or ints in IR
static intptr_t ir_node_get_int_robust(IRNode* node, const char* enum_type_name) {
    if (!node) return 0;

    if (node->type == IR_EXPR_LITERAL) {
        IRExprLiteral* lit = (IRExprLiteral*)node;
        if (!lit->value) {
             LV_LOG_WARN("ir_node_get_int_robust: Literal node has NULL value for enum %s", enum_type_name);
             return 0;
        }
        // Try to parse as number first
        char* endptr;
        long val = strtol(lit->value, &endptr, 0);
        if (*endptr == '\\0') { // Successfully parsed as a number
            return (intptr_t)val;
        }
        // If not a number, assume it's a string to be looked up by a user-defined function
        // This part needs a proper solution, like calling a user-provided lookup function.
        // For now, it's a placeholder, as `unmarshal_value` is for cJSON.
        // We'd need something like: `int resolve_ir_enum(const char* enum_name_str, const char* expected_enum_type)`
        // LV_LOG_INFO("Looking up enum %s string value '%s'", enum_type_name, lit->value);
        // This is where you'd call out to a function that can resolve LV_ALIGN_CENTER etc.
        // For demonstration, if it's a known enum, it should be handled by a more specific lookup.
        // This generic path is insufficient for string-based enum/define names.
        LV_LOG_WARN("ir_node_get_int_robust: Cannot resolve string literal '%s' for enum type '%s' without a dedicated resolver. Returning 0.", lit->value, enum_type_name);

    } else if (node->type == IR_EXPR_VARIABLE) { // An enum/define used as a variable name in IR
        // This also requires a lookup mechanism.
        IRExprVariable* var = (IRExprVariable*)node;
         LV_LOG_WARN("ir_node_get_int_robust: Cannot resolve variable '%s' for enum type '%s' without a dedicated resolver. Returning 0.", var->name, enum_type_name);
    } else {
        LV_LOG_WARN("ir_node_get_int_robust: Unexpected IRNode type %d for enum %s", node->type, enum_type_name);
    }
    return 0; // Fallback
}


#endif // ENABLE_IR_INPUTS

""".format(target_obj_type=obj_ptr_type_c))

def main():
    arg_parser = argparse.ArgumentParser(description="Generate dynamic LVGL dispatcher C code.")
    arg_parser.add_argument("api_spec_path", help="Path to the LVGL API spec JSON file (e.g., api_spec.json)")
    arg_parser.add_argument("--consolidation-mode", choices=["typesafe", "aggressive"],
                            default="typesafe", help="Function consolidation strategy.")
    arg_parser.add_argument("--output-json", action="store_true", help="Output the parsed API spec as JSON to stdout instead of generating C code.")
    arg_parser.add_argument("--header-out", default="dynamic_lvgl.h", help="Output path for the generated C header file.")
    arg_parser.add_argument("--source-out", default="dynamic_lvgl.c", help="Output path for the generated C source file.")

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

    if args.output_json:
        json.dump(translated_spec, sys.stdout, indent=2)
        sys.exit(0)

    print(f"--- C Code Generation (Mode: {args.consolidation_mode}) ---", file=sys.stderr)
    generator = CCodeGenerator(translated_spec, consolidation_mode=args.consolidation_mode)

    print(f"Analyzing function archetypes with {args.consolidation_mode} grouping...", file=sys.stderr)
    generator.analyze_archetypes()
    print(f"Found {len(generator.archetypes)} unique function archetypes.", file=sys.stderr)

    print(f"Generating {args.header_out} and {args.source_out}...", file=sys.stderr)
    generator.generate_files(args.header_out, args.source_out)
    print("Done.", file=sys.stderr)


if __name__ == '__main__':
    main()

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include "api_spec.h"
#include "generator.h"
#include "ir.h"
#include "registry.h"
#include "utils.h" // For get_obj_type_from_c_type if used

// --- Context for Generation ---
typedef struct {
    Registry* registry;
    const ApiSpec* api_spec;
    int var_counter;
    IRStmtBlock* current_global_block;
    // No direct ui_context here, it's passed around
} GenContext;

// Forward declarations
static void process_node_internal(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context, const char* forced_c_var_name);
static void process_node(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context); // Wrapper
static void process_properties(GenContext* ctx, cJSON* props_json, const char* target_c_var_name, IRStmtBlock* current_block, const char* obj_type_for_api_lookup, cJSON* ui_context);
static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context, const char* explicit_target_var_name);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context, const char* expected_c_type, const char* expected_enum_type_name);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);
// static const char* guess_expected_enum_name_from_c_type(const char* c_type, const ApiSpec* api_spec); // Declaration if used

// --- Utility Functions ---
static char* sanitize_c_identifier(const char* input_name) {
    if (!input_name || input_name[0] == '\0') {
        return strdup("unnamed_var");
    }
    char* sanitized_name = (char*)malloc(strlen(input_name) + 2);
    if (!sanitized_name) {
        perror("Failed to allocate memory for sanitized_name");
        return strdup("sanitize_fail");
    }
    int current_pos = 0;
    const char* p_in = input_name;
    if (p_in[0] == '@') {
        p_in++;
    }
    if (*p_in == '\0') { // Handle "@" as input
        free(sanitized_name);
        return strdup("unnamed_after_at");
    }
    if (isdigit((unsigned char)p_in[0])) {
        sanitized_name[current_pos++] = '_';
    }
    while (*p_in) {
        if (isalnum((unsigned char)*p_in)) {
            sanitized_name[current_pos++] = *p_in;
        } else {
            if (current_pos == 0 || sanitized_name[current_pos - 1] != '_') {
                 sanitized_name[current_pos++] = '_';
            }
        }
        p_in++;
    }
    if (current_pos > 0 && sanitized_name[current_pos - 1] == '_') {
         // Avoid trailing underscore unless it's the only char
        if (current_pos > 1 || sanitized_name[0] != '_') {
            sanitized_name[current_pos -1] = '\0';
        } else {
             sanitized_name[current_pos] = '\0';
        }
    } else {
        sanitized_name[current_pos] = '\0';
    }

    if (sanitized_name[0] == '\0' ) {
        free(sanitized_name);
        return strdup("unnamed_sanitized_var");
    }
    return sanitized_name;
}

static char* generate_unique_var_name(GenContext* ctx, const char* base_type) {
    char buf[128];
    char* sanitized_base = sanitize_c_identifier(base_type ? base_type : "var");
    snprintf(buf, sizeof(buf), "%s_%d", sanitized_base, ctx->var_counter++);
    free(sanitized_base);
    return strdup(buf);
}

// --- Core Processing Functions ---

// New helper for enum validation in unmarshal_value
static bool is_valid_enum_member(const ApiSpec* api_spec, const char* enum_type_name, const char* value_str) {
    if (!api_spec || !enum_type_name || !value_str) return false;
    const cJSON* enum_def = api_spec_get_enum(api_spec, enum_type_name);
    if (!enum_def) {
        // This might be too noisy if enums are not always in api_spec.json
        // fprintf(stderr, "DEBUG: Enum type '%s' not found in api_spec for validation.\n", enum_type_name);
        return false; // Or true, to be permissive if enum type not found
    }
    if (cJSON_GetObjectItem(enum_def, value_str)) {
        return true;
    }
    return false;
}


static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context, const char* expected_c_type, const char* expected_enum_type_name) {
    if (!value) return ir_new_literal("NULL");

    if (cJSON_IsString(value)) {
        const char* s_orig = value->valuestring;
        if (s_orig == NULL) return ir_new_literal("NULL");

        // Enum validation:
        if (expected_enum_type_name && expected_enum_type_name[0] != '\0') {
            if (!is_valid_enum_member(ctx->api_spec, expected_enum_type_name, s_orig)) {
                fprintf(stderr, "Warning: Value '%s' is not a valid member of enum '%s'. Expected C type: '%s'. Using literal '%s' as fallback.\n",
                        s_orig, expected_enum_type_name, expected_c_type ? expected_c_type : "unknown", s_orig);
                // Fallback to literal, but could also be ir_new_literal("NULL") or a default enum value if known.
                // For now, let it pass as a literal, type checking later might catch it if it's used as an int.
            }
        }


        if (s_orig[0] == '$' && s_orig[1] != '\0') { // Context variable
            if (ui_context) {
                cJSON* ctx_val = cJSON_GetObjectItem(ui_context, s_orig + 1);
                if (ctx_val) {
                    // When resolving context variables, we don't have an immediate expected type from the current property/arg.
                    return unmarshal_value(ctx, ctx_val, ui_context, NULL, NULL);
                } else {
                    fprintf(stderr, "Warning: Context variable '%s' not found. Using NULL.\n", s_orig + 1);
                    return ir_new_literal("NULL");
                }
            } else {
                 fprintf(stderr, "Warning: Attempted to access context variable '%s' with NULL ui_context. Using NULL.\n", s_orig + 1);
                 return ir_new_literal("NULL");
            }
        }
        if (s_orig[0] == '@' && s_orig[1] != '\0') { // ID reference
            const char* registered_c_var = registry_get_generated_var(ctx->registry, s_orig + 1);
            if (registered_c_var) {
                const char* type_of_id = NULL;
                registry_get_pointer_by_id(ctx->registry, s_orig + 1, &type_of_id); // Get its C-type from registry

                if (type_of_id && (strcmp(type_of_id, "lv_style_t") == 0 || strcmp(type_of_id, "lv_style_t*") == 0 )) {
                     return ir_new_address_of(ir_new_variable(registered_c_var));
                }
                // Check expected C type for other address-of cases (e.g. lv_font_t*)
                if (expected_c_type && strstr(expected_c_type, "*") && !strstr(expected_c_type, "char*") && type_of_id && strcmp(expected_c_type, type_of_id) != 0 && strcmp(expected_c_type, strcat(strdup(type_of_id),"*")) != 0) {
                    // This is a heuristic: if expected is a pointer and actual is not (or vice-versa for non-char*), could be an issue.
                    // More robustly, if expected is T* and actual is T, take address.
                    // For now, rely on explicit style check and direct variable use.
                }
                return ir_new_variable(registered_c_var);
            }
            // If not in generated_var registry, it might be an ID for runtime lookup.
            // The registry_get_pointer_by_id handles this.
            // This path is tricky: is it a C variable name or an ID for runtime?
            // For now, assume if not in registry_get_generated_var, it's a runtime ID string literal.
            // This means properties expecting variable names directly (not IDs) won't work with @.
             _dprintf(stderr, "DEBUG: ID '%s' not found in compile-time registry, treating as literal ID string for runtime lookup.\n", s_orig + 1);
             return ir_new_literal_string(s_orig + 1); // Pass the ID itself as a string
        }
        if (s_orig[0] == '#' && strlen(s_orig) > 1) { // Color hex
            long hex_val = strtol(s_orig + 1, NULL, 16);
            char hex_str_arg[32];
            snprintf(hex_str_arg, sizeof(hex_str_arg), "0x%06lX", hex_val);
            return ir_new_func_call_expr("lv_color_hex", ir_new_expr_node(ir_new_literal(hex_str_arg)));
        }
        if (s_orig[0] == '!' && s_orig[1] != '\0') { // String for global registry
            const char* registered_string = registry_add_str(ctx->registry, s_orig + 1);
            if (registered_string) {
                return ir_new_literal_string(registered_string); // Returns the string itself, quotes will be added by codegen
            } else {
                fprintf(stderr, "Warning: registry_add_str failed for value: %s. Using NULL literal.\n", s_orig + 1);
                return ir_new_literal("NULL");
            }
        }

        size_t len = strlen(s_orig);
        if (len > 0 && s_orig[len - 1] == '%') { // Percentage
            char* temp_s = strdup(s_orig);
            temp_s[len - 1] = '\0';
            char* endptr;
            long num_val = strtol(temp_s, &endptr, 10);
            if (*endptr == '\0' && endptr != temp_s) { // Valid integer before '%'
                char num_str_arg[32];
                snprintf(num_str_arg, sizeof(num_str_arg), "%ld", num_val);
                free(temp_s);
                return ir_new_func_call_expr("lv_pct", ir_new_expr_node(ir_new_literal(num_str_arg)));
            }
            free(temp_s);
        }

        // Check if it's a known constant or enum (already handled if expected_enum_type_name was set)
        if (!expected_enum_type_name && api_spec_is_enum_value(ctx->api_spec, s_orig)) {
             return ir_new_literal(s_orig);
        }
        if (api_spec_is_constant(ctx->api_spec, s_orig)) {
             return ir_new_literal(s_orig);
        }
        // Default to string literal if none of the above
        return ir_new_literal_string(s_orig);

    } else if (cJSON_IsNumber(value)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", (int)value->valuedouble);
        return ir_new_literal(buf);
    } else if (cJSON_IsBool(value)) {
        return ir_new_literal(cJSON_IsTrue(value) ? "true" : "false");
    } else if (cJSON_IsArray(value)) {
        IRExprNode* elements = NULL;
        cJSON* elem_json;
        // For arrays, individual elements might have different expected types if we could know them.
        // For now, pass NULL, NULL for expected types for array elements.
        cJSON_ArrayForEach(elem_json, value) {
            ir_expr_list_add(&elements, unmarshal_value(ctx, elem_json, ui_context, NULL, NULL));
        }
        return ir_new_array(elements);
    } else if (cJSON_IsObject(value)) {
        cJSON* call_item = cJSON_GetObjectItem(value, "call");
        cJSON* args_item = cJSON_GetObjectItem(value, "args");

        if (cJSON_IsString(call_item)) { // It's a function call object
            const char* call_name = call_item->valuestring;
            IRExprNode* ir_args_list = NULL;
            const FunctionDefinition* func_def = api_spec_get_function(ctx->api_spec, call_name);

            if (!func_def) {
                fprintf(stderr, "Warning: Function '%s' not found in API spec. Cannot perform type checking for arguments.\n", call_name);
            }

            FunctionArg* expected_arg_def = func_def ? func_def->args : NULL;
            int arg_idx = 0;

            if (cJSON_IsArray(args_item)) {
                cJSON* current_json_arg;
                cJSON_ArrayForEach(current_json_arg, args_item) {
                    const char* expected_arg_c_type = NULL;
                    const char* expected_arg_enum_name = NULL;
                    if (expected_arg_def) {
                        expected_arg_c_type = expected_arg_def->type;
                        expected_arg_enum_name = expected_arg_def->enum_type_name;
                    } else if (func_def) {
                        fprintf(stderr, "Warning: Too many arguments provided for function '%s'. Expected %d arguments. Ignoring extra.\n", call_name, func_def->num_args);
                    }

                    IRExpr* arg_expr = unmarshal_value(ctx, current_json_arg, ui_context, expected_arg_c_type, expected_arg_enum_name);
                    ir_expr_list_add(&ir_args_list, arg_expr);

                    if (func_def && expected_arg_def && arg_expr) {
                        const char* actual_arg_type_str = ir_expr_get_type(arg_expr, ctx->api_spec, ctx->registry);
                        if (actual_arg_type_str && expected_arg_c_type && strcmp(actual_arg_type_str, expected_arg_c_type) != 0) {
                            // More sophisticated check needed: e.g. int vs enum, pointer compatibility
                            bool type_compatible = false;
                            if (strcmp(expected_arg_c_type, "void*") == 0) type_compatible = true; // void* accepts any pointer
                            else if (strstr(expected_arg_c_type, "*") && strcmp(actual_arg_type_str, "NULL_t") == 0) type_compatible = true; // Pointer type expected, NULL given
                            else if (api_spec_is_enum_type(ctx->api_spec, expected_arg_enum_name) && strcmp(actual_arg_type_str,"int")==0 ) type_compatible = true; // Enum expected, int given
                            else if (expected_arg_enum_name && strcmp(actual_arg_type_str, expected_arg_enum_name) == 0) type_compatible = true;


                            if (!type_compatible) {
                                 fprintf(stderr, "Warning: Type mismatch for argument %d ('%s') of function '%s'. Expected C-type '%s', but got '%s'.\n",
                                    arg_idx + 1, expected_arg_def->name ? expected_arg_def->name : "", call_name, expected_arg_c_type, actual_arg_type_str);
                            }
                        }
                    }
                    if (expected_arg_def) expected_arg_def = expected_arg_def->next;
                    arg_idx++;
                }
            } else if (args_item != NULL) { // Single argument not in an array
                const char* expected_arg_c_type = NULL;
                const char* expected_arg_enum_name = NULL;
                if (expected_arg_def) {
                    expected_arg_c_type = expected_arg_def->type;
                    expected_arg_enum_name = expected_arg_def->enum_type_name;
                } else if (func_def && func_def->num_args > 0) {
                     fprintf(stderr, "Warning: Too many arguments provided for function '%s'. Expected %d arguments. Ignoring extra.\n", call_name, func_def->num_args);
                }


                IRExpr* arg_expr = unmarshal_value(ctx, args_item, ui_context, expected_arg_c_type, expected_arg_enum_name);
                ir_expr_list_add(&ir_args_list, arg_expr);

                if (func_def && expected_arg_def && arg_expr) {
                    const char* actual_arg_type_str = ir_expr_get_type(arg_expr, ctx->api_spec, ctx->registry);
                     if (actual_arg_type_str && expected_arg_c_type && strcmp(actual_arg_type_str, expected_arg_c_type) != 0) {
                        bool type_compatible = false;
                        if (strcmp(expected_arg_c_type, "void*") == 0) type_compatible = true;
                        else if (strstr(expected_arg_c_type, "*") && strcmp(actual_arg_type_str, "NULL_t") == 0) type_compatible = true;
                        else if (api_spec_is_enum_type(ctx->api_spec, expected_arg_enum_name) && strcmp(actual_arg_type_str,"int")==0 ) type_compatible = true;
                        else if (expected_arg_enum_name && strcmp(actual_arg_type_str, expected_arg_enum_name) == 0) type_compatible = true;


                        if (!type_compatible) {
                            fprintf(stderr, "Warning: Type mismatch for argument %d ('%s') of function '%s'. Expected C-type '%s', but got '%s'.\n",
                                arg_idx + 1, expected_arg_def->name ? expected_arg_def->name : "",call_name, expected_arg_c_type, actual_arg_type_str);
                        }
                    }
                }
                if (expected_arg_def) expected_arg_def = expected_arg_def->next;
                arg_idx++;
            }

            if (func_def && expected_arg_def) { // Check if all expected args were provided
                 fprintf(stderr, "Warning: Too few arguments provided for function '%s'. Expected more arguments starting with '%s'.\n",
                    call_name, expected_arg_def->name ? expected_arg_def->name : "unknown");
            }
             if (func_def && arg_idx < func_def->min_args) {
                fprintf(stderr, "Warning: Too few arguments for function %s. Expected at least %d, got %d.\n", call_name, func_def->min_args, arg_idx);
            }


            return ir_new_func_call_expr(call_name, ir_args_list);
        }
        // If not a 'call' object, it's an unhandled object structure
        fprintf(stderr, "Warning: Unhandled JSON object structure in unmarshal_value (not a 'call'): %s. Using NULL.\n", cJSON_PrintUnformatted(value));
        return ir_new_literal("NULL");
    } else if (cJSON_IsNull(value)) {
        return ir_new_literal("NULL");
    }

    fprintf(stderr, "Warning: Unhandled JSON type (%d) in unmarshal_value. Using NULL.\n", value->type);
    return ir_new_literal("NULL");
}


static void process_properties(GenContext* ctx, cJSON* node_json_containing_properties, const char* target_c_var_name, IRStmtBlock* current_block, const char* obj_type_for_api_lookup, cJSON* ui_context) {
    _dprintf(stderr, "DEBUG: process_properties: START. Target C var: %s, Obj type: %s\n", target_c_var_name, obj_type_for_api_lookup);
    if (!node_json_containing_properties) return;

    cJSON* source_of_props = node_json_containing_properties;
    cJSON* properties_sub_object = cJSON_GetObjectItem(node_json_containing_properties, "properties");
    if (properties_sub_object && cJSON_IsObject(properties_sub_object)) {
        source_of_props = properties_sub_object;
    }

    cJSON* prop = NULL;
    for (prop = source_of_props->child; prop != NULL; prop = prop->next) {
        const char* prop_name = prop->string;
        if (!prop_name) continue;

        if (strncmp(prop_name, "//", 2) == 0) { /* Comment */
            if (cJSON_IsString(prop)) ir_block_add_stmt(current_block, ir_new_comment(prop->valuestring));
            continue;
        }

        if (strcmp(prop_name, "style") == 0) {
            if (cJSON_IsString(prop) && prop->valuestring != NULL && prop->valuestring[0] == '@') {
                IRExpr* style_expr = unmarshal_value(ctx, prop, ui_context, "lv_style_t*", NULL); // Expecting a style object
                if (style_expr) {
                    IRExprNode* args_list = ir_new_expr_node(ir_new_variable(target_c_var_name));
                    ir_expr_list_add(&args_list, style_expr); // style_expr should be address_of if style obj
                    ir_expr_list_add(&args_list, ir_new_literal("0")); // Selector = 0 for base style
                    ir_block_add_stmt(current_block, ir_new_func_call_stmt("lv_obj_add_style", args_list));
                    continue;
                }
            }
        }

        // Skip structural/reserved keywords
        if (strcmp(prop_name, "type") == 0 || strcmp(prop_name, "id") == 0 || strcmp(prop_name, "named") == 0 ||
            strcmp(prop_name, "context") == 0 || strcmp(prop_name, "children") == 0 ||
            strcmp(prop_name, "view_id") == 0 || strcmp(prop_name, "inherits") == 0 ||
            strcmp(prop_name, "use-view") == 0 || strcmp(prop_name, "style") == 0 || /* style already handled or is not @ref */
            strcmp(prop_name, "create") == 0 || strcmp(prop_name, "c_type") == 0 || strcmp(prop_name, "init_func") == 0 ||
            strcmp(prop_name, "with") == 0 || strcmp(prop_name, "properties") == 0) {
            continue;
        }

        const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, obj_type_for_api_lookup, prop_name);
        if (!prop_def) {
            if (strcmp(obj_type_for_api_lookup, "obj") != 0 && strcmp(obj_type_for_api_lookup, "style") != 0) {
                prop_def = api_spec_find_property(ctx->api_spec, "obj", prop_name); // Fallback to common "obj" properties
            }
            if (!prop_def) {
                fprintf(stderr, "Warning: Property '%s' for object type '%s' (C var '%s') not found in API spec. Skipping.\n", prop_name, obj_type_for_api_lookup, target_c_var_name);
                continue;
            }
        }

        const char* expected_c_type_for_prop_val = prop_def->c_type;
        const char* expected_enum_name_for_prop_val = prop_def->enum_type_name;

        const char* actual_setter_name_const = prop_def->setter;
        char* actual_setter_name_allocated = NULL;
        if (!actual_setter_name_const || actual_setter_name_const[0] == '\0') {
            char constructed_setter[128];
            // Construction logic based on obj_type_for_api_lookup and prop_name
            // (e.g., lv_obj_set_width, lv_style_set_bg_color)
            // This is a simplified placeholder; actual construction might be more complex.
            const char* type_prefix = prop_def->widget_type_hint ? prop_def->widget_type_hint : obj_type_for_api_lookup;
            if (strcmp(obj_type_for_api_lookup, "style")==0) type_prefix = "style"; // normalize
            else if (strcmp(obj_type_for_api_lookup, "obj")==0 && prop_def->widget_type_hint == NULL) type_prefix = "obj";


            snprintf(constructed_setter, sizeof(constructed_setter), "lv_%s_set_%s", type_prefix, prop_name);
            actual_setter_name_allocated = strdup(constructed_setter);
            actual_setter_name_const = actual_setter_name_allocated;
        }

        IRExprNode* args_list = NULL;
        if (prop_def->func_args != NULL) { // Signature-based argument generation
            FunctionArg* current_func_arg_def = prop_def->func_args;
            if (current_func_arg_def && strstr(current_func_arg_def->type, "_t*") != NULL) { // First arg is target obj
                ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
                current_func_arg_def = current_func_arg_def->next;
            }

            if (cJSON_IsArray(prop)) {
                cJSON* val_item_json;
                cJSON_ArrayForEach(val_item_json, prop) {
                    if (!current_func_arg_def) { fprintf(stderr, "Warning: Too many values in JSON array for func %s (prop %s)\n", actual_setter_name_const, prop_name); break; }
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, val_item_json, ui_context, current_func_arg_def->type, current_func_arg_def->enum_type_name));
                    current_func_arg_def = current_func_arg_def->next;
                }
            } else if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value")) { // {"value": X, "part": Y, "state": Z}
                cJSON* value_json = cJSON_GetObjectItem(prop, "value");
                cJSON* part_json = cJSON_GetObjectItem(prop, "part");
                cJSON* state_json = cJSON_GetObjectItem(prop, "state");
                // This needs careful mapping to current_func_arg_def based on names or fixed order
                if (value_json && current_func_arg_def) {
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, value_json, ui_context, current_func_arg_def->type, current_func_arg_def->enum_type_name));
                    current_func_arg_def = current_func_arg_def->next;
                }
                if (part_json && current_func_arg_def) { // Assuming 'part' is next if present
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, part_json, ui_context, current_func_arg_def->type, current_func_arg_def->enum_type_name));
                    current_func_arg_def = current_func_arg_def->next;
                }
                if (state_json && current_func_arg_def) { // Assuming 'state' is next if present
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, state_json, ui_context, current_func_arg_def->type, current_func_arg_def->enum_type_name));
                    current_func_arg_def = current_func_arg_def->next;
                }
            } else { // Simple value: "prop_name": "simple_value"
                if (current_func_arg_def) {
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, prop, ui_context, current_func_arg_def->type, current_func_arg_def->enum_type_name));
                    current_func_arg_def = current_func_arg_def->next;
                } else if (!args_list && prop_def->num_style_args == 0 && strcmp(obj_type_for_api_lookup, "style") != 0 ) {
                    // If no args consumed yet, and it's a simple setter (not style-like), assume this is the value.
                     ir_expr_list_add(&args_list, unmarshal_value(ctx, prop, ui_context, expected_c_type_for_prop_val, expected_enum_name_for_prop_val));
                }
            }
            if (current_func_arg_def) { fprintf(stderr, "Warning: Not all args for func %s (prop %s) provided by JSON.\n", actual_setter_name_const, prop_name); }
        } else { // Fallback to old logic (target, value, optional part/state)
            ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
            cJSON* value_to_unmarshal = prop;
            const char* part_str = prop_def->style_part_default ? prop_def->style_part_default : "LV_PART_MAIN";
            const char* state_str = prop_def->style_state_default ? prop_def->style_state_default : "LV_STATE_DEFAULT";

            if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value")) {
                value_to_unmarshal = cJSON_GetObjectItem(prop, "value");
                cJSON* p = cJSON_GetObjectItem(prop, "part"); if(p) part_str = p->valuestring;
                cJSON* s = cJSON_GetObjectItem(prop, "state"); if(s) state_str = s->valuestring;
            }
            IRExpr* val_expr = unmarshal_value(ctx, value_to_unmarshal, ui_context, expected_c_type_for_prop_val, expected_enum_name_for_prop_val);

            if (prop_def->num_style_args == -1 && strcmp(obj_type_for_api_lookup, "style") != 0) { // lv_obj_set_style_..._sel
                ir_expr_list_add(&args_list, val_expr);
                char selector_str[128]; snprintf(selector_str, sizeof(selector_str), "%s | %s", part_str, state_str);
                ir_expr_list_add(&args_list, ir_new_literal(selector_str));
            } else if (prop_def->num_style_args > 0 && strcmp(obj_type_for_api_lookup, "style") == 0) { // lv_style_set_...
                 if(prop_def->num_style_args == 1) { // e.g. lv_style_set_text_opa(style, LV_STATE_DEFAULT, opa_val)
                    ir_expr_list_add(&args_list, ir_new_literal((char*)state_str)); // state first
                 } else { // e.g. lv_style_set_text_decor(style, LV_PART_MAIN, LV_STATE_DEFAULT, decor_val)
                    ir_expr_list_add(&args_list, ir_new_literal((char*)part_str));  // part first
                    ir_expr_list_add(&args_list, ir_new_literal((char*)state_str)); // then state
                 }
                 ir_expr_list_add(&args_list, val_expr);
            } else { // Standard setter lv_..._set_...(obj, value) or style setter with 0 extra args
                ir_expr_list_add(&args_list, val_expr);
            }
        }
        ir_block_add_stmt(current_block, ir_new_func_call_stmt(actual_setter_name_const, args_list));
        if (actual_setter_name_allocated) free(actual_setter_name_allocated);
    }
    _dprintf(stderr, "DEBUG: process_properties: END. Target C var: %s, Obj type: %s\n", target_c_var_name, obj_type_for_api_lookup);
}


// Wrapper function
static void process_node(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context) {
    process_node_internal(ctx, node_json, parent_block, parent_c_var, default_obj_type, ui_context, NULL);
}

static void process_node_internal(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context, const char* forced_c_var_name) {
    if (!cJSON_IsObject(node_json)) return;

    IRStmtBlock* current_node_ir_block = ir_new_block();
    ir_block_add_stmt(parent_block, (IRStmt*)current_node_ir_block);

    cJSON* node_specific_context = cJSON_GetObjectItem(node_json, "context");
    cJSON* effective_context = ui_context; // Default to inherited context
    bool own_effective_context = false;

    if (node_specific_context) { // Node has its own context definition
        if (ui_context) { // And there's an inherited context, so merge them
            effective_context = cJSON_Duplicate(ui_context, true);
            own_effective_context = true;
            cJSON* item_ctx_iter;
            for (item_ctx_iter = node_specific_context->child; item_ctx_iter != NULL; item_ctx_iter = item_ctx_iter->next) {
                cJSON_DeleteItemFromObjectCaseSensitive(effective_context, item_ctx_iter->string); // Remove old if present
                cJSON_AddItemToObject(effective_context, item_ctx_iter->string, cJSON_Duplicate(item_ctx_iter, true));
            }
        } else { // No inherited context, just use the node's specific one
            effective_context = node_specific_context; // No duplication needed if not merging
        }
    }


    char* c_var_name_for_node = NULL;
    char* allocated_c_var_name = NULL;
    const char* type_str = NULL;
    const WidgetDefinition* widget_def = NULL;
    const char* id_for_runtime_registry = NULL; // Store the string like "my_button_id"
    const char* c_type_for_gen_registry = NULL; // Store the C type string like "lv_obj_t*" or "my_custom_type_t"


    if (forced_c_var_name) {
        c_var_name_for_node = (char*)forced_c_var_name;
        // Determine type_str and widget_def for forced_c_var_name based on default_obj_type
        type_str = default_obj_type;
        if (type_str) widget_def = api_spec_find_widget(ctx->api_spec, type_str);
         _dprintf(stderr, "DEBUG: process_node_internal (forced_c_var_name path for DO block): C_VAR_NAME: %s, Type for props: %s\n", c_var_name_for_node, type_str ? type_str : "NULL");

    } else {
        cJSON* named_item = cJSON_GetObjectItem(node_json, "named");
        cJSON* id_item = cJSON_GetObjectItem(node_json, "id");
        const char* c_name_source = NULL;

        if (id_item && cJSON_IsString(id_item) && id_item->valuestring[0] == '@') {
            id_for_runtime_registry = id_item->valuestring + 1;
        }

        if (named_item && cJSON_IsString(named_item) && named_item->valuestring[0] != '\0') {
            c_name_source = named_item->valuestring;
        } else if (id_for_runtime_registry) {
            c_name_source = id_for_runtime_registry; // Use @id as base for C var name if "named" is missing
        }

        if (c_name_source) {
            allocated_c_var_name = sanitize_c_identifier(c_name_source);
            c_var_name_for_node = allocated_c_var_name;
            // Register C var name for @id or named (if not starting with @)
            const char* key_for_gen_reg = id_for_runtime_registry ? id_for_runtime_registry : (c_name_source[0] != '@' ? c_name_source : NULL);
            if (key_for_gen_reg) {
                 registry_add_generated_var(ctx->registry, key_for_gen_reg, c_var_name_for_node);
                  _dprintf(stderr, "DEBUG: Registered ID/Name '%s' to C-var '%s'\n", key_for_gen_reg, c_var_name_for_node);
            }
        } else {
            // Auto-generate var name if no "named" or "@id"
            const char* temp_type_str_for_name = default_obj_type;
            cJSON* type_item_for_name = cJSON_GetObjectItem(node_json, "type");
            if (type_item_for_name && cJSON_IsString(type_item_for_name)) {
                 temp_type_str_for_name = type_item_for_name->valuestring;
            }
            allocated_c_var_name = generate_unique_var_name(ctx, temp_type_str_for_name && temp_type_str_for_name[0] != '@' ? temp_type_str_for_name : "obj");
            c_var_name_for_node = allocated_c_var_name;
        }
    }


    bool is_with_assignment_node = false; // TODO: Review this logic
    // ... (is_with_assignment_node logic as before) ...

    bool object_successfully_created = false;

    // Determine type_str and widget_def if not already set (i.e. not forced_c_var_name path)
    if (!forced_c_var_name) {
        cJSON* type_item_local = cJSON_GetObjectItem(node_json, "type");
        type_str = type_item_local ? cJSON_GetStringValue(type_item_local) : default_obj_type;
        if (!type_str || type_str[0] == '\0') {
             fprintf(stderr, "Error: Node missing valid 'type'. C var: %s. Skipping.\n", c_var_name_for_node);
             goto cleanup_node;
        }
        if (type_str[0] == '@') { // Component type
            const cJSON* component_root_json = registry_get_component(ctx->registry, type_str + 1);
            if (!component_root_json) {
                fprintf(stderr, "Error: Component definition '%s' not found. C var: %s. Skipping.\n", type_str, c_var_name_for_node);
                goto cleanup_node;
            }
            // Process the component's root node instead of this one.
            // Pass current c_var_name_for_node as forced_c_var_name for the component's root.
            // The component's root type becomes the new default_obj_type.
            cJSON* comp_root_type_item = cJSON_GetObjectItem(component_root_json, "type");
            const char* comp_root_type_str = comp_root_type_item ? cJSON_GetStringValue(comp_root_type_item) : "obj";
            process_node_internal(ctx, (cJSON*)component_root_json, current_node_ir_block, parent_c_var, comp_root_type_str, effective_context, c_var_name_for_node);
            // After component expansion, apply any overriding properties from the instance
            process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, comp_root_type_str, effective_context);
            goto cleanup_node; // Current node processing is complete after component expansion.
        }
        widget_def = api_spec_find_widget(ctx->api_spec, type_str);
    }
    // At this point, for non-component types (or forced_c_var_name path),
    // type_str, widget_def, and c_var_name_for_node should be set.

    // Determine c_type_for_gen_registry
    if (widget_def && widget_def->c_type && widget_def->c_type[0] != '\0') {
        c_type_for_gen_registry = widget_def->c_type;
    } else if (strcmp(type_str, "style") == 0) {
        c_type_for_gen_registry = "lv_style_t"; // Styles are by value in registry context
    } else {
        c_type_for_gen_registry = "lv_obj_t*"; // Default for LVGL objects
    }


    if (is_with_assignment_node) {
        // ...
    } else if (forced_c_var_name) { // Operating on an existing var (e.g. "do" block)
        process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, type_str, effective_context);
        cJSON* children_json_in_do = cJSON_GetObjectItem(node_json, "children");
        if (cJSON_IsArray(children_json_in_do)) {
            cJSON* child_node_json_in_do;
            cJSON_ArrayForEach(child_node_json_in_do, children_json_in_do) {
                process_node_internal(ctx, child_node_json_in_do, current_node_ir_block, c_var_name_for_node, "obj", effective_context, NULL);
            }
        }
    } else { // Create new object/widget
        if (widget_def && widget_def->create && widget_def->create[0] != '\0') { // Standard widget (e.g. button)
            IRExpr* parent_expr = (parent_c_var && parent_c_var[0] != '\0') ? ir_new_variable(parent_c_var) : ir_new_literal("NULL");
            ir_block_add_stmt(current_node_ir_block, ir_new_widget_allocate_stmt(c_var_name_for_node, c_type_for_gen_registry, widget_def->create, parent_expr));
            object_successfully_created = true;
        } else if (widget_def && widget_def->init_func && widget_def->init_func[0] != '\0') { // Object with init func (e.g. style)
             if (!c_type_for_gen_registry) {
                fprintf(stderr, "Error: Object type '%s' (var %s) has init_func but no c_type. Skipping.\n", type_str, c_var_name_for_node);
                goto cleanup_node;
            }
            ir_block_add_stmt(current_node_ir_block, ir_new_object_allocate_stmt(c_var_name_for_node, c_type_for_gen_registry, widget_def->init_func));
            object_successfully_created = true;
        } else if (strcmp(type_str, "obj") == 0) { // Generic "obj"
            IRExpr* parent_expr = (parent_c_var && parent_c_var[0] != '\0') ? ir_new_variable(parent_c_var) : ir_new_literal("NULL");
            ir_block_add_stmt(current_node_ir_block, ir_new_widget_allocate_stmt(c_var_name_for_node, "lv_obj_t*", "lv_obj_create", parent_expr));
            object_successfully_created = true;
        } else {
            fprintf(stderr, "Warning: Type '%s' (var %s) is not a known widget, object with init_func, or generic 'obj'. Cannot create. Applying props/children may fail.\n", type_str, c_var_name_for_node);
            // object_successfully_created remains false
        }

        if (object_successfully_created) {
            process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, type_str, effective_context);
            cJSON* children_json = cJSON_GetObjectItem(node_json, "children");
            if (cJSON_IsArray(children_json)) {
                cJSON* child_node_json;
                cJSON_ArrayForEach(child_node_json, children_json) {
                    process_node_internal(ctx, child_node_json, current_node_ir_block, c_var_name_for_node, "obj", effective_context, NULL);
                }
            }
        }
    }


    // Add to pointer registry (for runtime lookup by ID)
    if (object_successfully_created && id_for_runtime_registry && c_var_name_for_node) {
        // Determine type for runtime registry_add_pointer call.
        // This should be the JSON type ("button", "label", "style", etc.)
        const char* type_for_runtime_registry_arg = type_str;
        if (widget_def && widget_def->json_type_override && widget_def->json_type_override[0] != '\0') {
            type_for_runtime_registry_arg = widget_def->json_type_override;
        }


        IRExprNode* reg_args = NULL;
        ir_expr_list_add(&reg_args, ir_new_variable("ui_registry")); // Assumes global "ui_registry"
        // If it's a style, pass the variable directly (it's lv_style_t, not lv_style_t*)
        // Otherwise, pass the variable (which is already a pointer like lv_obj_t*)
        if (c_type_for_gen_registry && strcmp(c_type_for_gen_registry, "lv_style_t") == 0) {
             // This case is tricky. registry_add_pointer expects void*.
             // If we store lv_style_t by value, we'd need to register its address.
             // For now, let's assume all registered pointers are actual pointers.
             // This means styles should probably be lv_style_t* in c_type_for_gen_registry if registered this way.
             // OR, registry_add_pointer needs to handle values if type indicates it's not a pointer.
             // For simplicity, let's assume c_var_name_for_node is always a pointer type if it's registered.
             // This implies styles might need to be heap allocated if we want to register them via ID.
             // Let's assume for now styles are not added to this runtime ID registry, only UI objects.
             // This needs clarification in requirements for style ID lookup.
             // However, the original plan was to add c_type_for_gen_registry to registry.
             // Let's assume if it's lv_style_t, we register its address.

            // This part of the original plan seems to have a slight conflict.
            // registry_add_pointer takes (Registry*, void* ptr, char* id, char* type)
            // If c_var_name_for_node is lv_style_t my_style; we need &my_style.
            // If c_var_name_for_node is lv_obj_t* my_obj = ...; we pass my_obj.

            // Let's add to the *generator's context registry* (compile-time)
            // This was the purpose of c_type_for_gen_registry.
            registry_add_pointer(ctx->registry, (void*)c_var_name_for_node, id_for_runtime_registry, c_type_for_gen_registry);
             _dprintf(stderr, "DEBUG: Added to GenContext registry: ID '%s', C-var '%s', C-Type '%s'\n", id_for_runtime_registry, c_var_name_for_node, c_type_for_gen_registry);


            // For the *generated code's* call to registry_add_pointer (runtime):
            // We need to decide what C-type string to pass. It should be the type of the object itself.
            // 'type_for_runtime_registry_arg' is the JSON type ("button", "label").
            // This is what we want for the runtime registry.
            IRExpr* ptr_arg_expr;
            if (c_type_for_gen_registry && strcmp(c_type_for_gen_registry, "lv_style_t") == 0) {
                ptr_arg_expr = ir_new_address_of(ir_new_variable(c_var_name_for_node));
            } else {
                ptr_arg_expr = ir_new_variable(c_var_name_for_node);
            }
            ir_expr_list_add(&reg_args, ptr_arg_expr);
            ir_expr_list_add(&reg_args, ir_new_literal_string(id_for_runtime_registry));
            ir_expr_list_add(&reg_args, type_for_runtime_registry_arg ? ir_new_literal_string(type_for_runtime_registry_arg) : ir_new_literal("NULL"));
            ir_block_add_stmt(current_node_ir_block, ir_new_func_call_stmt("registry_add_pointer", reg_args));
             _dprintf(stderr, "DEBUG: Emitted runtime registry_add_pointer for ID '%s' (C-var: %s, JSON Type: %s)\n", id_for_runtime_registry, c_var_name_for_node, type_for_runtime_registry_arg ? type_for_runtime_registry_arg : "NULL");

        } else if (c_type_for_gen_registry && strstr(c_type_for_gen_registry, "*") != NULL) { // It's already a pointer type
            registry_add_pointer(ctx->registry, (void*)c_var_name_for_node, id_for_runtime_registry, c_type_for_gen_registry);
             _dprintf(stderr, "DEBUG: Added to GenContext registry: ID '%s', C-var '%s', C-Type '%s'\n", id_for_runtime_registry, c_var_name_for_node, c_type_for_gen_registry);

            ir_expr_list_add(&reg_args, ir_new_variable(c_var_name_for_node));
            ir_expr_list_add(&reg_args, ir_new_literal_string(id_for_runtime_registry));
            ir_expr_list_add(&reg_args, type_for_runtime_registry_arg ? ir_new_literal_string(type_for_runtime_registry_arg) : ir_new_literal("NULL"));
            ir_block_add_stmt(current_node_ir_block, ir_new_func_call_stmt("registry_add_pointer", reg_args));
            _dprintf(stderr, "DEBUG: Emitted runtime registry_add_pointer for ID '%s' (C-var: %s, JSON Type: %s)\n", id_for_runtime_registry, c_var_name_for_node, type_for_runtime_registry_arg ? type_for_runtime_registry_arg : "NULL");
        }
    }


    // Process "with" blocks if any (only for non-assignment, non-forced nodes)
    if (!is_with_assignment_node && !forced_c_var_name && object_successfully_created) {
        cJSON* item_w_regular = NULL;
        for (item_w_regular = node_json->child; item_w_regular != NULL; item_w_regular = item_w_regular->next) {
            if (item_w_regular->string && strcmp(item_w_regular->string, "with") == 0) {
                // Pass NULL for explicit_target_var_name, process_single_with_block will create temp vars.
                process_single_with_block(ctx, item_w_regular, current_node_ir_block, effective_context, NULL);
            }
        }
    }

cleanup_node:
    if (allocated_c_var_name) free(allocated_c_var_name);
    if (own_effective_context) cJSON_Delete(effective_context);
}


static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context, const char* explicit_target_var_name) {
    if (!cJSON_IsObject(with_node)) return; // Should be array of objects or single object

    cJSON* current_with_item = NULL;
    if (cJSON_IsArray(with_node)){
        // Iterate if "with" holds an array of operations
        cJSON_ArrayForEach(current_with_item, with_node){
            process_single_with_block(ctx, current_with_item, parent_ir_block, ui_context, explicit_target_var_name);
        }
        return;
    }
    // If not an array, current_with_item is with_node itself
    current_with_item = with_node;


    cJSON* obj_json = cJSON_GetObjectItem(current_with_item, "obj");
    if (!obj_json) {
        fprintf(stderr, "Error: 'with' block item missing 'obj' key.\n");
        return;
    }

    // When unmarshalling the 'obj' part of a 'with' block, we generally don't have a specific expected C type yet from a property.
    IRExpr* obj_expr = unmarshal_value(ctx, obj_json, ui_context, NULL, NULL);
    if (!obj_expr) {
        fprintf(stderr, "Error: Failed to unmarshal 'obj' in 'with' block.\n");
        return;
    }

    const char* target_c_var_name_for_do = NULL;
    char* generated_var_name_to_free = NULL;
    const char* obj_type_for_do_props = "obj"; // Default type for properties in "do"
    const char* c_type_of_obj_expr_val = "lv_obj_t*"; // Default C type of the obj_expr's result

    // Determine c_type_of_obj_expr_val and obj_type_for_do_props from obj_expr
    const char* actual_type_of_obj_expr = ir_expr_get_type(obj_expr, ctx->api_spec, ctx->registry);
    if (actual_type_of_obj_expr) {
        c_type_of_obj_expr_val = actual_type_of_obj_expr; // This is the C type string
        obj_type_for_do_props = get_obj_type_from_c_type(c_type_of_obj_expr_val); // Convert C type to JSON type for prop lookup
    }


    if (explicit_target_var_name) { // "with" is part of a "named" node: `named_var = with.obj; do ...`
        target_c_var_name_for_do = explicit_target_var_name;
        ir_block_add_stmt(parent_ir_block, ir_new_var_decl(c_type_of_obj_expr_val, target_c_var_name_for_do, obj_expr));
        // obj_expr is now owned by var_decl
    } else { // "with" is standalone, create temp var or use direct var name
        if (obj_expr->type == IR_EXPR_VARIABLE && !cJSON_GetObjectItem(current_with_item, "do")) {
            // If obj_expr is just a variable AND there's no "do" block,
            // this 'with' might be a simple assignment to a variable that's not explicitly named.
            // This case is ambiguous and potentially problematic.
            // For now, assume if no "do", it's an expression whose value might be used elsewhere or implicitly.
            // Let's proceed as if a temporary variable is created.
            generated_var_name_to_free = generate_unique_var_name(ctx, obj_type_for_do_props);
            target_c_var_name_for_do = generated_var_name_to_free;
            ir_block_add_stmt(parent_ir_block, ir_new_var_decl(c_type_of_obj_expr_val, target_c_var_name_for_do, obj_expr));
        } else if (obj_expr->type == IR_EXPR_VARIABLE) {
            // If there IS a "do" block, and obj_expr is a variable, we operate directly on that variable.
            target_c_var_name_for_do = strdup(((IRExprVariable*)obj_expr)->name);
            generated_var_name_to_free = (char*)target_c_var_name_for_do;
            ir_free((IRNode*)obj_expr); // We copied the name, free the expression.
        } else { // obj_expr is a function call or other complex expression, requires a temp var.
            generated_var_name_to_free = generate_unique_var_name(ctx, obj_type_for_do_props);
            target_c_var_name_for_do = generated_var_name_to_free;
            ir_block_add_stmt(parent_ir_block, ir_new_var_decl(c_type_of_obj_expr_val, target_c_var_name_for_do, obj_expr));
        }
    }

    cJSON* do_json = cJSON_GetObjectItem(current_with_item, "do");
    if (cJSON_IsObject(do_json)) {
        // Use obj_type_for_do_props for looking up properties inside the "do" block
        process_node_internal(ctx, do_json, parent_ir_block, NULL /*parent_c_var for children in "do"*/, obj_type_for_do_props, ui_context, target_c_var_name_for_do /*forced_c_var_name for "do"*/);
    }

    if (generated_var_name_to_free) {
        free(generated_var_name_to_free);
    }
}


// This function is simplified as component definitions are pre-processed.
// Top-level "style" definitions are also typically handled by specific calls if needed,
// or fall into the general process_node if structured like other UI elements.
static void generate_ir_from_ui_spec_internal_logic(GenContext* ctx, const cJSON* ui_spec_root) {
    cJSON* item_json = NULL;

    // Pre-process and register all component definitions
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        cJSON* type_node = cJSON_GetObjectItemCaseSensitive(item_json, "type");
        if (type_node && cJSON_IsString(type_node) && strcmp(type_node->valuestring, "component") == 0) {
            cJSON* id_json = cJSON_GetObjectItemCaseSensitive(item_json, "id");
            cJSON* root_json = cJSON_GetObjectItemCaseSensitive(item_json, "root");
            if (!root_json) root_json = cJSON_GetObjectItemCaseSensitive(item_json, "content");

            if (id_json && cJSON_IsString(id_json) && id_json->valuestring[0] == '@' && root_json) {
                registry_add_component(ctx->registry, id_json->valuestring + 1, root_json);
                 _dprintf(stderr, "DEBUG: Registered component '%s'\n", id_json->valuestring + 1);
            } else {
                fprintf(stderr, "Warning: Invalid component definition. Missing @id or root/content. Node: %s\n", cJSON_PrintUnformatted(item_json));
            }
        }
    }

    // Process all top-level nodes (screens, non-component objects, style definitions if any at top level)
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        cJSON* type_node = cJSON_GetObjectItem(item_json, "type");
        const char* type_str = type_node ? cJSON_GetStringValue(type_node) : NULL;

        if (type_str && strcmp(type_str, "component") == 0) {
            continue; // Skip component definitions themselves, they are expanded on use.
        }
        // For top-level elements, parent_c_var is NULL or a default like "lv_scr_act()" if applicable.
        // Here, using "NULL" as a generic parent, specific create functions handle NULL parent.
        process_node(ctx, item_json, ctx->current_global_block, NULL, type_str ? type_str : "obj", NULL);
    }
}


IRStmtBlock* generate_ir_from_ui_spec_with_registry(
    const cJSON* ui_spec_root,
    const ApiSpec* api_spec,
    Registry* existing_registry) { // Renamed for clarity

    if (!ui_spec_root || !api_spec || !cJSON_IsArray(ui_spec_root)) {
        fprintf(stderr, "Error: Invalid arguments to generate_ir_from_ui_spec_with_registry.\n");
        return NULL;
    }

    GenContext ctx;
    ctx.api_spec = api_spec;
    ctx.var_counter = 0;

    bool own_registry = false;
    if (existing_registry) {
        ctx.registry = existing_registry;
    } else {
        ctx.registry = registry_create();
        if (!ctx.registry) {
            fprintf(stderr, "Error: Failed to create registry.\n");
            return NULL;
        }
        own_registry = true;
    }

    ctx.current_global_block = ir_new_block();
    if (!ctx.current_global_block) {
        fprintf(stderr, "Error: Failed to create root IR block.\n");
        if (own_registry) registry_free(ctx.registry);
        return NULL;
    }

    generate_ir_from_ui_spec_internal_logic(&ctx, ui_spec_root);

    // The string entries in the registry are typically written out to a separate C file by the caller,
    // so we don't free the string registry part here if it was passed in.
    // If we owned this registry (i.e. created it here), we should free it.
    if (own_registry) {
        registry_free(ctx.registry); // This will free generated_vars, components, and strings.
    }

    return ctx.current_global_block;
}

// Original wrapper, kept for API compatibility if anything calls it without a registry.
IRStmtBlock* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
    return generate_ir_from_ui_spec_with_registry(ui_spec_root, api_spec, NULL);
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include "api_spec.h"
#include "generator.h"
#include "ir.h"
#include "registry.h"
#include "utils.h"

// --- Context for Generation ---
typedef struct {
    Registry* registry;
    const ApiSpec* api_spec;
    int var_counter;
    IRStmtBlock* current_global_block;
} GenContext;

// Forward declarations
static void process_node_internal(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context, const char* forced_c_var_name);
static void process_node(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context); // Wrapper
static void process_properties(GenContext* ctx, cJSON* props_json, const char* target_c_var_name, IRStmtBlock* current_block, const char* obj_type_for_api_lookup, cJSON* ui_context);
static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context, const char* explicit_target_var_name);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context, const char* expected_enum_type_for_arg);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);

// --- Helper to count expected arguments from FunctionArg list (excluding initial object pointer) ---
static int count_expected_json_args(const FunctionArg* head) {
    if (!head) return 0;
    int count = 0;
    const FunctionArg* current = head;
    // Check if the first argument is the object pointer itself (common LVGL pattern)
    if (current && current->type && (strstr(current->type, "lv_obj_t*") || strstr(current->type, "lv_style_t*"))) {
        current = current->next; // Skip it for counting purposes if it's the object type
    }
    while (current) {
        count++;
        current = current->next;
    }
    return count;
}


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
    if (*p_in == '\0') {
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
    sanitized_name[current_pos] = '\0';
    if (sanitized_name[0] == '\0' || (sanitized_name[0] == '_' && sanitized_name[1] == '\0') ) {
        if (sanitized_name[0] == '\0') {
             free(sanitized_name);
             return strdup("unnamed_sanitized_var");
        }
    }
    return sanitized_name;
}

static char* generate_unique_var_name(GenContext* ctx, const char* base_type) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s_%d", base_type ? base_type : "var", ctx->var_counter++);
    return strdup(buf);
}

// --- Core Processing Functions ---

static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context, const char* expected_enum_type_for_arg) {
    if (!value) return ir_new_literal("NULL");

    if (cJSON_IsString(value)) {
        const char* s_orig = value->valuestring;

        if (s_orig == NULL) return ir_new_literal("NULL");

        if (s_orig[0] == '$' && s_orig[1] != '\0') {
            if (ui_context) {
                cJSON* ctx_val = cJSON_GetObjectItem(ui_context, s_orig + 1);
                if (ctx_val) {
                    return unmarshal_value(ctx, ctx_val, ui_context, expected_enum_type_for_arg);
                } else {
                    fprintf(stderr, "Warning: Context variable '%s' not found.\n", s_orig + 1);
                    return ir_new_literal("NULL");
                }
            } else {
                 fprintf(stderr, "Warning: Attempted to access context variable '%s' with NULL context.\n", s_orig + 1);
                 return ir_new_literal("NULL");
            }
        }
        if (s_orig[0] == '@' && s_orig[1] != '\0') {
            const char* registered_c_var = registry_get_generated_var(ctx->registry, s_orig + 1);
            if (registered_c_var) {
                if (strstr(s_orig + 1, "style") != NULL || strstr(registered_c_var, "style") != NULL || strncmp(registered_c_var, "s_", 2) == 0 ) {
                    return ir_new_address_of(ir_new_variable(registered_c_var));
                }
                return ir_new_variable(registered_c_var);
            }
            fprintf(stderr, "Info: ID '%s' not found in registry, treating as direct variable name.\n", s_orig + 1);
            return ir_new_variable(s_orig + 1);
        }
        if (s_orig[0] == '#' && s_orig[1] != '\0') {
            long hex_val = strtol(s_orig + 1, NULL, 16);
            char hex_str_arg[32];
            snprintf(hex_str_arg, sizeof(hex_str_arg), "0x%06lX", hex_val);
            return ir_new_func_call_expr("lv_color_hex", ir_new_expr_node(ir_new_literal(hex_str_arg)));
        }

        if (s_orig[0] == '!' && s_orig[1] != '\0') {
            const char* registered_string = registry_add_str(ctx->registry, s_orig + 1);
            if (registered_string) {
                return ir_new_literal_string(registered_string);
            } else {
                fprintf(stderr, "Warning: registry_add_str failed for value: %s. Returning NULL literal.\n", s_orig + 1);
                return ir_new_literal("NULL");
            }
        }

        size_t len = strlen(s_orig);
        if (len > 0 && s_orig[len - 1] == '%') {
            char* temp_s = strdup(s_orig);
            if (!temp_s) { perror("Failed to strdup for percentage processing"); return ir_new_literal_string(s_orig); }
            temp_s[len - 1] = '\0';
            char* endptr;
            long num_val = strtol(temp_s, &endptr, 10);
            if (*endptr == '\0' && endptr != temp_s) {
                char num_str_arg[32];
                snprintf(num_str_arg, sizeof(num_str_arg), "%ld", num_val);
                free(temp_s);
                return ir_new_func_call_expr("lv_pct", ir_new_expr_node(ir_new_literal(num_str_arg)));
            }
            free(temp_s);
        }

        // This is a plain string, not prefixed. Apply enum logic.
        if (expected_enum_type_for_arg && expected_enum_type_for_arg[0] != '\0') {
            const cJSON* all_enums = api_spec_get_enums(ctx->api_spec);
            if (all_enums) {
                cJSON* specific_enum_type_json = cJSON_GetObjectItem(all_enums, expected_enum_type_for_arg);
                if (specific_enum_type_json && cJSON_IsObject(specific_enum_type_json)) {
                    bool found_in_specific = cJSON_GetObjectItem(specific_enum_type_json, s_orig) != NULL;
                    if (found_in_specific) {
                        return ir_new_literal(s_orig); // Found in specific expected enum
                    } else {
                        // Value not found in the expected enum type.
                        // Now, search for s_orig in *all* enums to find its actual type.
                        const char* actual_enum_type_name = NULL;
                        cJSON* current_enum_type_json = NULL;
                        for (current_enum_type_json = all_enums->child; current_enum_type_json != NULL; current_enum_type_json = current_enum_type_json->next) {
                            if (cJSON_IsObject(current_enum_type_json) && current_enum_type_json->string) {
                                if (cJSON_GetObjectItem(current_enum_type_json, s_orig)) {
                                    actual_enum_type_name = current_enum_type_json->string;
                                    break;
                                }
                            }
                        }

                        if (actual_enum_type_name) {
                            // This is the new error message for mismatched enum types
                            fprintf(stderr, "ERROR: \"%s\" (enum %s) is not of expected enum type %s.\n", s_orig, actual_enum_type_name, expected_enum_type_for_arg);
                        }
                        // Continue to global search as per requirement
                    }
                } else {
                     fprintf(stderr, "Warning: Expected enum type '%s' not found in API spec. Falling back to global search for '%s'.\n", expected_enum_type_for_arg, s_orig);
                }
            }
        }

        // Fallback: global constant and enum search
        const cJSON* constants = api_spec_get_constants(ctx->api_spec);
        if (constants && cJSON_GetObjectItem(constants, s_orig)) {
            return ir_new_literal(s_orig);
        }

        const cJSON* all_enum_types_json_fallback = api_spec_get_enums(ctx->api_spec);
        if (all_enum_types_json_fallback && cJSON_IsObject(all_enum_types_json_fallback)) {
            cJSON* enum_type_definition_json = NULL;
            for (enum_type_definition_json = all_enum_types_json_fallback->child; enum_type_definition_json != NULL; enum_type_definition_json = enum_type_definition_json->next) {
                if (cJSON_IsObject(enum_type_definition_json)) {
                    if (cJSON_GetObjectItem(enum_type_definition_json, s_orig)) {
                        return ir_new_literal(s_orig);
                    }
                }
            }
        }
        // If s_orig wasn't an enum or constant, and not a special prefixed string,
        // it's treated as a literal string to be passed to a C function directly,
        // or used as a format string, etc.
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
        cJSON_ArrayForEach(elem_json, value) {
            ir_expr_list_add(&elements, unmarshal_value(ctx, elem_json, ui_context, NULL));
        }
        return ir_new_array(elements);
    } else if (cJSON_IsObject(value)) {
        cJSON* call_item = cJSON_GetObjectItem(value, "call");
        cJSON* args_item = cJSON_GetObjectItem(value, "args");
        if (cJSON_IsString(call_item)) {
            const char* call_name = call_item->valuestring;
            IRExprNode* args_list = NULL;
            if (cJSON_IsArray(args_item)) {
                 cJSON* arg_json;
                 cJSON_ArrayForEach(arg_json, args_item) {
                    IRExpr* arg_expr = unmarshal_value(ctx, arg_json, ui_context, NULL);
                    ir_expr_list_add(&args_list, arg_expr);
                }
            } else if (args_item != NULL) {
                 IRExpr* arg_expr = unmarshal_value(ctx, args_item, ui_context, NULL);
                 ir_expr_list_add(&args_list, arg_expr);
            }
            return ir_new_func_call_expr(call_name, args_list);
        }
        fprintf(stderr, "Warning: Unhandled JSON object structure in unmarshal_value. Object was: %s\n", cJSON_PrintUnformatted(value));
        return ir_new_literal("NULL");
    } else if (cJSON_IsNull(value)) {
        return ir_new_literal("NULL");
    }

    fprintf(stderr, "Warning: Unhandled JSON type (%d) in unmarshal_value. Returning NULL literal.\n", value->type);
    return ir_new_literal("NULL");
}


static void process_properties(GenContext* ctx, cJSON* node_json_containing_properties, const char* target_c_var_name, IRStmtBlock* current_block, const char* obj_type_for_api_lookup, cJSON* ui_context) {
    if (!node_json_containing_properties) {
        return;
    }

    cJSON* source_of_props = node_json_containing_properties;
    cJSON* properties_sub_object = cJSON_GetObjectItem(node_json_containing_properties, "properties");

    if (properties_sub_object && cJSON_IsObject(properties_sub_object)) {
        source_of_props = properties_sub_object;
    }

    cJSON* prop = NULL;

    for (prop = source_of_props->child; prop != NULL; prop = prop->next) {
        const char* prop_name = prop->string;
        if (!prop_name) continue;

        if (strncmp(prop_name, "//", 2) == 0) {
            if (cJSON_IsString(prop)) {
                IRStmt* comment_stmt = ir_new_comment(prop->valuestring);
                if (comment_stmt) ir_block_add_stmt(current_block, comment_stmt);
                else fprintf(stderr, "Warning: Failed to create IR comment for: %s\n", prop->valuestring);
            } else {
                fprintf(stderr, "Warning: Value of comment key '%s' is not a string. Skipping comment.\n", prop_name);
            }
            continue;
        }

        if (strcmp(prop_name, "style") == 0) {
            if (cJSON_IsString(prop) && prop->valuestring != NULL && prop->valuestring[0] == '@') {
                IRExpr* style_expr = unmarshal_value(ctx, prop, ui_context, NULL);
                if (style_expr) {
                    IRExprNode* args_list = ir_new_expr_node(ir_new_variable(target_c_var_name));
                    ir_expr_list_add(&args_list, style_expr);
                    ir_expr_list_add(&args_list, ir_new_literal("0"));
                    IRStmt* call_stmt = ir_new_func_call_stmt("lv_obj_add_style", args_list);
                    ir_block_add_stmt(current_block, call_stmt);
                    continue;
                } else {
                    fprintf(stderr, "Warning: Failed to unmarshal style reference '%s' for var '%s'.\n", prop->valuestring, target_c_var_name);
                }
            } else {
                 fprintf(stderr, "Warning: 'style' property for var '%s' is not a valid @-prefixed string reference. Value: %s\n", target_c_var_name, prop ? cJSON_PrintUnformatted(prop): "NULL");
            }
        }

        if (strcmp(prop_name, "type") == 0 || strcmp(prop_name, "id") == 0 || strcmp(prop_name, "named") == 0 ||
            strcmp(prop_name, "context") == 0 || strcmp(prop_name, "children") == 0 ||
            strcmp(prop_name, "view_id") == 0 || strcmp(prop_name, "inherits") == 0 ||
            strcmp(prop_name, "use-view") == 0 ||
            strcmp(prop_name, "style") == 0 ||
            strcmp(prop_name, "create") == 0 || strcmp(prop_name, "c_type") == 0 || strcmp(prop_name, "init_func") == 0 ||
            strcmp(prop_name, "with") == 0 || strcmp(prop_name, "properties") == 0) {
            continue;
        }

        const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, obj_type_for_api_lookup, prop_name);

        if (!prop_def) {
            if (strcmp(obj_type_for_api_lookup, "obj") != 0 && strcmp(obj_type_for_api_lookup, "style") != 0) {
                const PropertyDefinition* fallback_prop_def = api_spec_find_property(ctx->api_spec, "obj", prop_name);
                if (fallback_prop_def) {
                    prop_def = fallback_prop_def;
                }
            }
            if (!prop_def) {
                 if (strncmp(prop_name, "style_", 6) == 0 && strcmp(obj_type_for_api_lookup, "style") != 0) {
                    fprintf(stderr, "Info: Property '%s' on obj_type '%s' looks like a style property. Ensure it's applied to a style object or handled by add_style.\n", prop_name, obj_type_for_api_lookup);
                 } else {
                    fprintf(stderr, "Warning: Property '%s' for object type '%s' (C var '%s') not found in API spec. Skipping.\n", prop_name, obj_type_for_api_lookup, target_c_var_name);
                 }
                continue;
            }
        }

        const char* actual_setter_name_const = prop_def->setter;
        char* actual_setter_name_allocated = NULL;
        if (!actual_setter_name_const) {
            char constructed_setter[128];
            if (prop_def->obj_setter_prefix && prop_def->obj_setter_prefix[0] != '\0') {
                snprintf(constructed_setter, sizeof(constructed_setter), "%s_%s", prop_def->obj_setter_prefix, prop_name);
            } else if (strcmp(obj_type_for_api_lookup, "style") == 0) {
                 snprintf(constructed_setter, sizeof(constructed_setter), "lv_style_set_%s", prop_name);
            } else {
                 snprintf(constructed_setter, sizeof(constructed_setter), "lv_%s_set_%s",
                         prop_def->widget_type_hint ? prop_def->widget_type_hint : obj_type_for_api_lookup,
                         prop_name);
            }
            actual_setter_name_allocated = strdup(constructed_setter);
            actual_setter_name_const = actual_setter_name_allocated;
        }

        IRExprNode* args_list = NULL;

        if (prop_def->func_args != NULL) {
            int expected_json_arg_count = count_expected_json_args(prop_def->func_args);
            int provided_json_args_count = 0;

            if (cJSON_IsArray(prop)) {
                provided_json_args_count = cJSON_GetArraySize(prop);
            } else {
                provided_json_args_count = 1; // A single value is considered 1 argument
            }

            if (expected_json_arg_count != provided_json_args_count) {
                fprintf(stderr, "ERROR: Property '%s' on object type '%s' (setter %s) expects %d value argument(s), but %d provided in JSON.\n",
                        prop_name, obj_type_for_api_lookup, actual_setter_name_const, expected_json_arg_count, provided_json_args_count);
                // Note: Generation will still proceed and might lead to C compile errors or runtime issues.
            }

            FunctionArg* current_func_arg = prop_def->func_args;

            // Add the target object as the first argument to the C function call
            if (current_func_arg && current_func_arg->type && (strstr(current_func_arg->type, "lv_obj_t*") || strstr(current_func_arg->type, "lv_style_t*"))) {
                ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
                current_func_arg = current_func_arg->next; // Move to the first actual value argument
            } else if (strcmp(obj_type_for_api_lookup, "style") != 0 && prop_def->obj_setter_prefix == NULL) {
                // If not a style object and not a global style property, assume first arg is target
                 ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
                 // No current_func_arg advancement here if func_args doesn't list the obj*
            }


            if (cJSON_IsArray(prop)) {
                cJSON* val_item_json;
                cJSON_ArrayForEach(val_item_json, prop) {
                    if (!current_func_arg) {
                        // This case (too many provided args) is already covered by the check above,
                        // but we break here to avoid crashing.
                        break;
                    }
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, val_item_json, ui_context, current_func_arg->expected_enum_type));
                    current_func_arg = current_func_arg->next;
                }
            } else if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value")) {
                // This structure is typically for style properties with value/part/state.
                // The func_args for such properties should ideally list these explicitly.
                // The current count validation might be simplistic for this specific object structure if func_args map differently.
                cJSON* value_json = cJSON_GetObjectItem(prop, "value");
                cJSON* part_json = cJSON_GetObjectItem(prop, "part");
                cJSON* state_json = cJSON_GetObjectItem(prop, "state");

                if (value_json && current_func_arg) {
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, value_json, ui_context, current_func_arg->expected_enum_type));
                    current_func_arg = current_func_arg->next;
                }
                if (part_json && current_func_arg) {
                    // Assuming part and state are not typically enums needing specific type check from FunctionArg,
                    // but if they were, this would need current_func_arg->expected_enum_type too.
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, part_json, ui_context, NULL));
                    current_func_arg = current_func_arg->next;
                } else if (!part_json && current_func_arg && prop_def->style_part_default &&
                           (prop_def->num_style_args == 2 || prop_def->num_style_args == -1)) {
                     // Potentially use default part
                }

                if (state_json && current_func_arg) {
                    // Assuming part and state are not typically enums needing specific type check from FunctionArg
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, state_json, ui_context, NULL));
                    current_func_arg = current_func_arg->next;
                } else if (!state_json && current_func_arg && prop_def->style_state_default &&
                           prop_def->num_style_args > 0) {
                     // Potentially use default state
                }
            } else {
                if (current_func_arg) {
                    IRExpr* val_expr_func_arg = unmarshal_value(ctx, prop, ui_context, current_func_arg->expected_enum_type);
                    ir_expr_list_add(&args_list, val_expr_func_arg);
                    current_func_arg = current_func_arg->next;
                } else if (!args_list && strcmp(obj_type_for_api_lookup, "style") != 0) {
                     IRExpr* val_expr_func_arg = unmarshal_value(ctx, prop, ui_context, prop_def->expected_enum_type); // Fallback to prop_def's type if only one value expected by func_args
                     ir_expr_list_add(&args_list, val_expr_func_arg);
                }
            }

            if (current_func_arg != NULL) {
                // This means not enough arguments were provided in the JSON for all func_args
                // The earlier check (expected_json_arg_count != provided_json_args_count) should have caught this.
                // This is more of a safeguard.
                fprintf(stderr, "Warning: Not all arguments for C function %s (property '%s' on C var '%s') were provided by the JSON value (mismatch after processing).\n", actual_setter_name_const, prop_name, target_c_var_name);
            }

        } else {
            ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));

            cJSON* value_to_unmarshal = prop;
            const char* part_str = prop_def->style_part_default ? prop_def->style_part_default : "LV_PART_MAIN";
            const char* state_str = prop_def->style_state_default ? prop_def->style_state_default : "LV_STATE_DEFAULT";

            if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value")) {
                cJSON* part_json = cJSON_GetObjectItem(prop, "part");
                cJSON* state_json = cJSON_GetObjectItem(prop, "state");
                cJSON* value_json = cJSON_GetObjectItem(prop, "value");
                if (cJSON_IsString(part_json)) part_str = part_json->valuestring;
                if (cJSON_IsString(state_json)) state_str = state_json->valuestring;
                value_to_unmarshal = value_json;
            }

            IRExpr* val_expr;
            if (cJSON_IsNull(value_to_unmarshal)) {
                const char* prop_type_str = prop_def->c_type;
                if (prop_type_str) {
                    if (strcmp(prop_type_str, "char*") == 0 || strcmp(prop_type_str, "const char*") == 0 || strcmp(prop_type_str, "string") == 0) {
                        val_expr = ir_new_literal("NULL");
                    } else if (strcmp(prop_type_str, "lv_color_t") == 0) {
                        IRExprNode* hex_arg = ir_new_expr_node(ir_new_literal("0"));
                        val_expr = ir_new_func_call_expr("lv_color_hex", hex_arg);
                    } else if (strstr(prop_type_str, "lv_obj_t*") != NULL || strstr(prop_type_str, "lv_style_t*") != NULL || strstr(prop_type_str, "lv_font_t*") != NULL) {
                        val_expr = ir_new_literal("NULL");
                    }
                    else {
                        val_expr = ir_new_literal("0");
                    }
                } else {
                    if (strcmp(prop_name, "text") == 0 || strstr(prop_name, "font") != NULL || strstr(prop_name, "style") != NULL || (actual_setter_name_const && (strstr(actual_setter_name_const, "_font") || strstr(actual_setter_name_const, "_style")))) {
                        val_expr = ir_new_literal("NULL");
                    } else if (strstr(prop_name, "color") != NULL || (actual_setter_name_const && strstr(actual_setter_name_const, "_color"))) {
                        IRExprNode* hex_arg = ir_new_expr_node(ir_new_literal("0"));
                        val_expr = ir_new_func_call_expr("lv_color_hex", hex_arg);
                    } else {
                        val_expr = ir_new_literal("0");
                    }
                }
            } else {
                val_expr = unmarshal_value(ctx, value_to_unmarshal, ui_context, prop_def->expected_enum_type);
            }

            _dprintf(stderr, "DEBUG prop: %s, resolved_prop_def_name: %s, num_style_args: %d, type: %s, setter: %s, obj_setter_prefix: %s, expected_enum: %s\n",
                    prop_name, prop_def->name, prop_def->num_style_args, obj_type_for_api_lookup, prop_def->setter ? prop_def->setter : "NULL", prop_def->obj_setter_prefix ? prop_def->obj_setter_prefix : "NULL", prop_def->expected_enum_type ? prop_def->expected_enum_type : "NULL");

            // Argument count validation for non-func_args cases
            int expected_arg_count_for_setter = 1; // Always 1 for the value itself
            if (prop_def->num_style_args == -1 && strcmp(obj_type_for_api_lookup, "style") != 0) { // value + selector
                expected_arg_count_for_setter = 2;
            } else if (prop_def->num_style_args == 1 && strcmp(obj_type_for_api_lookup, "style") == 0) { // state + value
                expected_arg_count_for_setter = 2;
            } else if (prop_def->num_style_args == 2 && strcmp(obj_type_for_api_lookup, "style") != 0) { // part + state + value
                expected_arg_count_for_setter = 3;
            } else if (prop_def->num_style_args == 0) { // just value
                expected_arg_count_for_setter = 1;
            }
            // Note: The target_c_var_name is implicitly the first argument to the C function,
            // so we compare the JSON-provided arguments against expected_arg_count_for_setter - 1 (value only)
            // or against the specific logic for complex objects.

            int provided_json_args_count_simple = 0;
            if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value")) {
                // For {value: ..., part: ..., state: ...} structure, we consider "value" as the primary argument.
                // The part/state are handled by num_style_args logic.
                // This means the JSON provides 1 "effective" argument (the value itself)
                // that maps to one part of the C function's signature.
                // The other parts (part, state) are derived or defaulted.
                if (cJSON_GetObjectItem(prop, "value") != NULL) {
                     provided_json_args_count_simple = 1;
                }
            } else if (!cJSON_IsArray(prop)) { // Simple value like "align": "LV_ALIGN_CENTER"
                provided_json_args_count_simple = 1;
            } else { // Array value like "align": ["LV_ALIGN_CENTER"] - this is unusual for simple setters
                fprintf(stderr, "Warning: Property '%s' (simple setter) received an array value. This is typically for multi-arg setters. Argument count check might be unreliable.\n", prop_name);
                provided_json_args_count_simple = cJSON_GetArraySize(prop); // Treat as multiple, though likely an error
            }

            // The expected_arg_count_for_setter counts the C arguments *after* the object pointer.
            // For simple setters, the JSON usually provides one "value" which maps to one C argument.
            // Style properties are special:
            // - num_style_args = 0: expects 1 C arg (value)
            // - num_style_args = -1 (obj): expects 2 C args (value, selector)
            // - num_style_args = 1 (style): expects 2 C args (state, value)
            // - num_style_args = 2 (obj): expects 3 C args (part, state, value)
            // The `provided_json_args_count_simple` should be 1 if JSON gives a direct value or a {value: X} object.
            // The C function will take 1 (for value) + extras (for part/state/selector).
            // So, the check is if the single JSON value is appropriate for the setter's needs.
            // This check is more about "is the JSON providing a single value when expected" rather than strict C arg count.
            // The more detailed C arg count is implicitly handled by num_style_args logic.

            if (prop_def->num_style_args == 0) { // Expects 1 value from JSON
                if (provided_json_args_count_simple != 1 && !(cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value"))) {
                     fprintf(stderr, "ERROR: Property '%s' on object type '%s' (setter %s) expects a single value, but JSON provided %d effective argument(s).\n",
                             prop_name, obj_type_for_api_lookup, actual_setter_name_const, provided_json_args_count_simple);
                }
            } else if ( (prop_def->num_style_args == -1 || prop_def->num_style_args == 1 || prop_def->num_style_args == 2) ) {
                 // These expect a single "value" from JSON, part/state are implicit or explicit in the object.
                 if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value")) {
                    // Correct structure {value: V, part: P, state: S} - this is fine.
                 } else if (provided_json_args_count_simple != 1) {
                     fprintf(stderr, "ERROR: Property '%s' on object type '%s' (setter %s) expects a single value (potentially within a value/part/state object), but JSON provided %d effective argument(s).\n",
                             prop_name, obj_type_for_api_lookup, actual_setter_name_const, provided_json_args_count_simple);
                 }
            }


            if (prop_def->num_style_args == -1 && strcmp(obj_type_for_api_lookup, "style") != 0) {
                ir_expr_list_add(&args_list, val_expr);
                char selector_str[128];
                snprintf(selector_str, sizeof(selector_str), "%s | %s", part_str, state_str);
                ir_expr_list_add(&args_list, ir_new_literal(selector_str));
            } else if (prop_def->num_style_args == 1 && strcmp(obj_type_for_api_lookup, "style") == 0) {
                ir_expr_list_add(&args_list, ir_new_literal((char*)state_str));
                ir_expr_list_add(&args_list, val_expr);
            } else if (prop_def->num_style_args == 2 && strcmp(obj_type_for_api_lookup, "style") != 0) {
                ir_expr_list_add(&args_list, ir_new_literal((char*)part_str));
                ir_expr_list_add(&args_list, ir_new_literal((char*)state_str));
                ir_expr_list_add(&args_list, val_expr);
            } else if (prop_def->num_style_args == 0) {
                ir_expr_list_add(&args_list, val_expr);
            } else {
                fprintf(stderr, "Warning: Unhandled num_style_args (%d) for property '%s' on C var '%s' (type '%s', no func_args). Adding value only after target.\n",
                        prop_def->num_style_args, prop_name, target_c_var_name, obj_type_for_api_lookup);
                ir_expr_list_add(&args_list, val_expr);
            }
        }

        IRStmt* call_stmt = ir_new_func_call_stmt(actual_setter_name_const, args_list);
        ir_block_add_stmt(current_block, call_stmt);

        if (actual_setter_name_allocated) {
            free(actual_setter_name_allocated);
        }
    }
}

// Wrapper function
static void process_node(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context) {
    process_node_internal(ctx, node_json, parent_block, parent_c_var, default_obj_type, ui_context, NULL);
}

static void process_node_internal(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context, const char* forced_c_var_name) {
    if (!cJSON_IsObject(node_json)) return;

    IRStmtBlock* current_node_ir_block = ir_new_block();
    if (!current_node_ir_block) {
        fprintf(stderr, "Error: Failed to create IR block for node. Skipping.\n");
        return;
    }
    ir_block_add_stmt(parent_block, (IRStmt*)current_node_ir_block);

    cJSON* node_specific_context = cJSON_GetObjectItem(node_json, "context");
    cJSON* effective_context = NULL;
    bool own_effective_context = false;

    if (ui_context && node_specific_context) {
        effective_context = cJSON_Duplicate(ui_context, true);
        cJSON* item_ctx_iter;
        for (item_ctx_iter = node_specific_context->child; item_ctx_iter != NULL; item_ctx_iter = item_ctx_iter->next) {
            if (cJSON_GetObjectItem(effective_context, item_ctx_iter->string)) {
                cJSON_ReplaceItemInObject(effective_context, item_ctx_iter->string, cJSON_Duplicate(item_ctx_iter, true));
            } else {
                cJSON_AddItemToObject(effective_context, item_ctx_iter->string, cJSON_Duplicate(item_ctx_iter, true));
            }
        }
        own_effective_context = true;
    } else if (node_specific_context) {
        effective_context = node_specific_context;
    } else if (ui_context) {
        effective_context = ui_context;
    }

    char* c_var_name_for_node = NULL;
    char* allocated_c_var_name = NULL;
    const char* type_str = NULL;
    const WidgetDefinition* widget_def = NULL;

    if (forced_c_var_name) {
        c_var_name_for_node = (char*)forced_c_var_name;
    } else {
        cJSON* named_item = cJSON_GetObjectItem(node_json, "named");
        cJSON* id_item = cJSON_GetObjectItem(node_json, "id");
        const char* c_name_source = NULL;
        const char* id_key_for_registry = NULL;

        if (id_item && cJSON_IsString(id_item) && id_item->valuestring[0] == '@') {
            id_key_for_registry = id_item->valuestring + 1;
        }

        if (named_item && cJSON_IsString(named_item) && named_item->valuestring[0] != '\0') {
            c_name_source = named_item->valuestring;
            if (!id_key_for_registry) {
                 if (named_item->valuestring[0] != '@') {
                    id_key_for_registry = c_name_source;
                 }
            }
        } else if (id_key_for_registry) {
            c_name_source = id_key_for_registry;
        }

        if (c_name_source) {
            allocated_c_var_name = sanitize_c_identifier(c_name_source);
            c_var_name_for_node = allocated_c_var_name;
            if (id_key_for_registry) {
                registry_add_generated_var(ctx->registry, id_key_for_registry, c_var_name_for_node);
            } else if (c_name_source[0] != '@') {
                registry_add_generated_var(ctx->registry, c_name_source, c_var_name_for_node);
            }
        } else {
            const char* temp_type_str_for_name = default_obj_type;
            cJSON* type_item_for_name = cJSON_GetObjectItem(node_json, "type");
            if (type_item_for_name && cJSON_IsString(type_item_for_name)) {
                 temp_type_str_for_name = type_item_for_name->valuestring;
            }
            allocated_c_var_name = generate_unique_var_name(ctx, temp_type_str_for_name && temp_type_str_for_name[0] != '@' ? temp_type_str_for_name : "obj");
            c_var_name_for_node = allocated_c_var_name;
        }
    }

    bool is_with_assignment_node = false;
    cJSON* named_attr_for_check = cJSON_GetObjectItem(node_json, "named");
    cJSON* with_attr_for_check = cJSON_GetObjectItem(node_json, "with");

    if (named_attr_for_check && cJSON_IsString(named_attr_for_check) && named_attr_for_check->valuestring[0] != '\0' && with_attr_for_check) {
        is_with_assignment_node = true;
        cJSON* item = NULL;
        for (item = node_json->child; item != NULL; item = item->next) {
            const char* key = item->string;
            if (strcmp(key, "type") != 0 &&
                strcmp(key, "id") != 0 &&
                strcmp(key, "named") != 0 &&
                strcmp(key, "with") != 0 &&
                strcmp(key, "context") != 0 &&
                strncmp(key, "//", 2) != 0) {
                is_with_assignment_node = false;
                break;
            }
        }
    }
    bool object_successfully_created = false;

    cJSON* use_view_item = cJSON_GetObjectItemCaseSensitive(node_json, "use-view");
    if (use_view_item && cJSON_IsString(use_view_item)) {
        const char* component_id_to_use = use_view_item->valuestring;
        if (!component_id_to_use || component_id_to_use[0] != '@') {
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "Error: 'use-view' value '%s' must be a component ID starting with '@'.", component_id_to_use ? component_id_to_use : "NULL");
            ir_block_add_stmt(current_node_ir_block, ir_new_comment(err_buf));
            fprintf(stderr, "%s Node: %s\n", err_buf, cJSON_PrintUnformatted(node_json));
        } else {
            const cJSON* component_root_json = registry_get_component(ctx->registry, component_id_to_use + 1);
            if (!component_root_json) {
                char err_buf[256];
                snprintf(err_buf, sizeof(err_buf), "Error: Component definition '%s' not found in registry for 'use-view'.", component_id_to_use);
                ir_block_add_stmt(current_node_ir_block, ir_new_comment(err_buf));
                fprintf(stderr, "%s Node: %s\n", err_buf, cJSON_PrintUnformatted(node_json));
            } else {
                cJSON* comp_root_type_item = cJSON_GetObjectItem(component_root_json, "type");
                const char* comp_root_type_str = comp_root_type_item ? cJSON_GetStringValue(comp_root_type_item) : "obj";
                process_node_internal(ctx, (cJSON*)component_root_json, current_node_ir_block, parent_c_var, comp_root_type_str, effective_context, c_var_name_for_node);
                process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, comp_root_type_str, effective_context);
            }
        }
        if (allocated_c_var_name) free(allocated_c_var_name);
        if (own_effective_context) cJSON_Delete(effective_context);
        return;
    }


    if (is_with_assignment_node) {
        ir_block_add_stmt(current_node_ir_block, ir_new_comment( "// Node is a 'with' assignment target. Processing 'with' blocks for assignment."));
        cJSON* item_w_assign = NULL;
        for (item_w_assign = node_json->child; item_w_assign != NULL; item_w_assign = item_w_assign->next) {
            if (item_w_assign->string && strcmp(item_w_assign->string, "with") == 0) {
                process_single_with_block(ctx, item_w_assign, current_node_ir_block, effective_context, c_var_name_for_node);
            }
        }
    } else if (forced_c_var_name) {
        type_str = default_obj_type;
        widget_def = api_spec_find_widget(ctx->api_spec, type_str);
        process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, type_str, effective_context);
        cJSON* children_json_in_do = cJSON_GetObjectItem(node_json, "children");
        if (cJSON_IsArray(children_json_in_do)) {
            cJSON* child_node_json_in_do;
            cJSON_ArrayForEach(child_node_json_in_do, children_json_in_do) {
                process_node_internal(ctx, child_node_json_in_do, current_node_ir_block, c_var_name_for_node, "obj", effective_context, NULL);
            }
        }
    } else {
        cJSON* type_item_local = cJSON_GetObjectItem(node_json, "type");
        type_str = type_item_local ? cJSON_GetStringValue(type_item_local) : default_obj_type;

    if (!type_str || type_str[0] == '\0') {
         fprintf(stderr, "Error: Node missing valid 'type' (or default_obj_type for component root). C var: %s. Skipping node processing.\n", c_var_name_for_node);
         if (allocated_c_var_name) free(allocated_c_var_name);
         if (own_effective_context) cJSON_Delete(effective_context);
         return;
    }

    widget_def = api_spec_find_widget(ctx->api_spec, type_str);

    if (strcmp(type_str, "use-view") == 0) {
        cJSON* component_ref_item = cJSON_GetObjectItem(node_json, "view_id");
        if (!cJSON_IsString(component_ref_item)) {
            component_ref_item = cJSON_GetObjectItem(node_json, "id");
        }
        if (!cJSON_IsString(component_ref_item) || !component_ref_item->valuestring || component_ref_item->valuestring[0] == '\0') {
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "Error: Node with type 'use-view' missing 'view_id' or valid 'id' attribute.");
            fprintf(stderr, "%s Node: %s\n", err_buf, cJSON_PrintUnformatted(node_json));
            ir_block_add_stmt(current_node_ir_block, ir_new_comment(err_buf));
        } else {
            const char* component_def_id_from_json = component_ref_item->valuestring;
            if (component_def_id_from_json[0] != '@') {
                char err_buf[256];
                snprintf(err_buf, sizeof(err_buf), "Error: 'use-view' type node's component reference '%s' must start with '@'.", component_def_id_from_json);
                ir_block_add_stmt(current_node_ir_block, ir_new_comment(err_buf));
                fprintf(stderr, "%s\n", err_buf);
            } else {
                const cJSON* component_root_json = registry_get_component(ctx->registry, component_def_id_from_json + 1);
                if (!component_root_json) {
                    char err_buf[256];
                    snprintf(err_buf, sizeof(err_buf), "Error: Component definition '%s' not found for 'use-view' type node.", component_def_id_from_json);
                    ir_block_add_stmt(current_node_ir_block, ir_new_comment(err_buf));
                    fprintf(stderr, "%s\n", err_buf);
                } else {
                    cJSON* comp_root_type_item = cJSON_GetObjectItem(component_root_json, "type");
                    const char* comp_root_type_str = comp_root_type_item ? cJSON_GetStringValue(comp_root_type_item) : "obj";
                    process_node_internal(ctx, (cJSON*)component_root_json, current_node_ir_block, parent_c_var, comp_root_type_str, effective_context, c_var_name_for_node);
                }
            }
        }
    } else if (widget_def && widget_def->create && widget_def->create[0] != '\0') {
        IRExpr* parent_var_expr = NULL;
        if (parent_c_var && parent_c_var[0] != '\0') {
            parent_var_expr = ir_new_variable(parent_c_var);
        }
        ir_block_add_stmt(current_node_ir_block,
                          ir_new_widget_allocate_stmt(c_var_name_for_node,
                                                      "lv_obj_t",
                                                      widget_def->create,
                                                      parent_var_expr));
        object_successfully_created = true;
        process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, type_str, effective_context);
        cJSON* children_json = cJSON_GetObjectItem(node_json, "children");
        if (cJSON_IsArray(children_json)) {
            cJSON* child_node_json;
            cJSON_ArrayForEach(child_node_json, children_json) {
                process_node_internal(ctx, child_node_json, current_node_ir_block, c_var_name_for_node, "obj", effective_context, NULL);
            }
        }
    } else if (widget_def && widget_def->init_func && widget_def->init_func[0] != '\0') {
        if (!widget_def->c_type || widget_def->c_type[0] == '\0') {
            fprintf(stderr, "Error: Object type '%s' (var %s) has init_func '%s' but no c_type defined. Skipping.\n",
                    type_str, c_var_name_for_node, widget_def->init_func);
            char error_comment[256];
            snprintf(error_comment, sizeof(error_comment), "Error: Object type '%s' missing c_type for init_func '%s'", type_str, widget_def->init_func);
            ir_block_add_stmt(current_node_ir_block, ir_new_comment(error_comment));
        } else {
            ir_block_add_stmt(current_node_ir_block,
                              ir_new_object_allocate_stmt(c_var_name_for_node,
                                                          widget_def->c_type,
                                                          widget_def->init_func));
            object_successfully_created = true;
            process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, type_str, effective_context);
        }
    } else if (type_str[0] == '@' && !forced_c_var_name) {
        const cJSON* component_root_json = registry_get_component(ctx->registry, type_str + 1);
        if (!component_root_json) {
            fprintf(stderr, "Error: Component definition '%s' (used as type) not found. Skipping node.\n", type_str);
        } else {
            cJSON* comp_root_type_item = cJSON_GetObjectItem(component_root_json, "type");
            const char* comp_root_type_str = comp_root_type_item ? cJSON_GetStringValue(comp_root_type_item) : "obj";
            process_node_internal(ctx, (cJSON*)component_root_json, current_node_ir_block, parent_c_var, comp_root_type_str, effective_context, c_var_name_for_node);
        }
    } else {
        if (strcmp(type_str, "obj") == 0 || forced_c_var_name) {
            IRExpr* parent_var_expr = NULL;
            if (parent_c_var && parent_c_var[0] != '\0') {
                parent_var_expr = ir_new_variable(parent_c_var);
            }
            const char* actual_create_type = (forced_c_var_name && widget_def && widget_def->create) ? type_str : "obj";
            const char* actual_create_func = (forced_c_var_name && widget_def && widget_def->create) ? widget_def->create : "lv_obj_create";
            const char* c_type_for_alloc = (forced_c_var_name && widget_def && widget_def->c_type) ? widget_def->c_type : "lv_obj_t";

            if (strcmp(actual_create_type, "style")==0) {
                 ir_block_add_stmt(current_node_ir_block,
                              ir_new_object_allocate_stmt(c_var_name_for_node,
                                                          c_type_for_alloc,
                                                          actual_create_func));
            } else {
                 ir_block_add_stmt(current_node_ir_block,
                                  ir_new_widget_allocate_stmt(c_var_name_for_node,
                                                              c_type_for_alloc,
                                                              actual_create_func,
                                                              parent_var_expr));
                 object_successfully_created = true;
            }
            process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, actual_create_type, effective_context);

            cJSON* children_json = cJSON_GetObjectItem(node_json, "children");
            if (cJSON_IsArray(children_json)) {
                cJSON* child_node_json;
                cJSON_ArrayForEach(child_node_json, children_json) {
                    process_node_internal(ctx, child_node_json, current_node_ir_block, c_var_name_for_node, "obj", effective_context, NULL);
                }
            }
        } else {
            char warning_comment[256];
            snprintf(warning_comment, sizeof(warning_comment), "Warning: Type '%s' (var %s) not directly instantiable. Children attach to '%s'.",
                     type_str, c_var_name_for_node, parent_c_var ? parent_c_var : "default_parent");
            char info_comment[256];
            snprintf(info_comment, sizeof(info_comment), "Info: Type '%s' (var %s) has no specific create/init. Creating as lv_obj_t and applying properties.", type_str, c_var_name_for_node);
            ir_block_add_stmt(current_node_ir_block, ir_new_comment(info_comment));

            IRExpr* parent_var_expr = (parent_c_var && parent_c_var[0] != '\0') ? ir_new_variable(parent_c_var) : NULL;
            const char* c_type_for_alloc = (widget_def && widget_def->c_type && widget_def->c_type[0] != '\0') ? widget_def->c_type : "lv_obj_t";
            ir_block_add_stmt(current_node_ir_block,
                              ir_new_widget_allocate_stmt(c_var_name_for_node,
                                                          c_type_for_alloc,
                                                          "lv_obj_create",
                                                          parent_var_expr));
            object_successfully_created = true;
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
    }

    if (object_successfully_created && c_var_name_for_node) {
        cJSON* id_item_for_reg = cJSON_GetObjectItem(node_json, "id");
        if (id_item_for_reg && cJSON_IsString(id_item_for_reg) && id_item_for_reg->valuestring && id_item_for_reg->valuestring[0] == '@') {
            const char* json_id_val = id_item_for_reg->valuestring + 1;
            const char* type_for_registry = type_str;
            if (type_str && type_str[0] == '@') {
                const WidgetDefinition* comp_widget_def = api_spec_find_widget(ctx->api_spec, type_str);
                if (comp_widget_def) {
                    type_for_registry = comp_widget_def->c_type;
                } else {
                    const cJSON* component_root_json = registry_get_component(ctx->registry, type_str + 1);
                    if (component_root_json) {
                        cJSON* comp_type_item = cJSON_GetObjectItem(component_root_json, "type");
                        if (comp_type_item && cJSON_IsString(comp_type_item)) {
                            type_for_registry = comp_type_item->valuestring;
                        } else {
                            type_for_registry = "obj";
                        }
                    } else {
                         type_for_registry = "unknown_component_type";
                    }
                }
            }
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_variable("ui_registry"));
            ir_expr_list_add(&args, ir_new_variable(c_var_name_for_node));
            ir_expr_list_add(&args, ir_new_literal_string(json_id_val));
            ir_expr_list_add(&args, type_for_registry ? ir_new_literal_string(type_for_registry) : ir_new_literal("NULL"));
            IRStmt* add_ptr_stmt = ir_new_func_call_stmt("registry_add_pointer", args);
            ir_block_add_stmt(current_node_ir_block, add_ptr_stmt);
        }
    }

    if (!is_with_assignment_node && !forced_c_var_name) {
        bool regular_node_can_have_with = (widget_def && (widget_def->create || widget_def->init_func)) ||
                                     (type_str && strcmp(type_str, "style") == 0) ||
                                     (type_str && strcmp(type_str, "obj") == 0);
        if (c_var_name_for_node && regular_node_can_have_with) {
            cJSON* item_w_regular = NULL;
            for (item_w_regular = node_json->child; item_w_regular != NULL; item_w_regular = item_w_regular->next) {
                if (item_w_regular->string && strcmp(item_w_regular->string, "with") == 0) {
                    process_single_with_block(ctx, item_w_regular, current_node_ir_block, effective_context, NULL);
                }
            }
        }
    }

    if (allocated_c_var_name) {
        free(allocated_c_var_name);
    }

    if (own_effective_context) {
        cJSON_Delete(effective_context);
    }
}

static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context, const char* explicit_target_var_name) {
    if (!cJSON_IsObject(with_node)) {
        fprintf(stderr, "Error: 'with' block item must be an object (when processing for target: %s).\n", explicit_target_var_name ? explicit_target_var_name : "temp_var");
        return;
    }

    cJSON* obj_json = cJSON_GetObjectItem(with_node, "obj");

    if (!obj_json) {
        fprintf(stderr, "Error: 'with' block missing 'obj' key (when processing for target: %s).\n", explicit_target_var_name ? explicit_target_var_name : "temp_var");
        return;
    }

    IRExpr* obj_expr = unmarshal_value(ctx, obj_json, ui_context, NULL);
    if (!obj_expr) {
        fprintf(stderr, "Error: Failed to unmarshal 'obj' in 'with' block (when processing for target: %s).\n", explicit_target_var_name ? explicit_target_var_name : "temp_var");
        return;
    }

    const char* target_c_var_name = NULL;
    char* generated_var_name_to_free = NULL;
    const char* obj_type_for_props = "obj";
    const char* temp_var_c_type = "lv_obj_t*";

    if (obj_expr->type == IR_EXPR_VARIABLE) {
        const char* var_name = ((IRExprVariable*)obj_expr)->name;
        if (strstr(var_name, "style") != NULL || strncmp(var_name, "s_", 2) == 0) {
            obj_type_for_props = "style";
            temp_var_c_type = "lv_style_t*";
        } else if (strstr(var_name, "label") != NULL || strncmp(var_name, "l_", 2) == 0) {
            obj_type_for_props = "label";
            temp_var_c_type = "lv_obj_t*";
        } else {
            obj_type_for_props = "obj";
            temp_var_c_type = "lv_obj_t*";
        }
    } else if (obj_expr->type == IR_EXPR_FUNC_CALL) {
        IRExprFuncCall* func_call_expr = (IRExprFuncCall*)obj_expr;
        const char* func_name = func_call_expr->func_name;
        const char* c_return_type_str = api_spec_get_function_return_type(ctx->api_spec, func_name);
        if (c_return_type_str && c_return_type_str[0] != '\0') {
            temp_var_c_type = c_return_type_str;
        } else {
            fprintf(stderr, "Warning: Could not determine return type for function '%s'. Defaulting to '%s'.\n", func_name, temp_var_c_type);
        }
        obj_type_for_props = get_obj_type_from_c_type(temp_var_c_type);
    } else if (obj_expr->type == IR_EXPR_ADDRESS_OF) {
        IRExprAddressOf* addr_of_expr = (IRExprAddressOf*)obj_expr;
        if (addr_of_expr->expr && addr_of_expr->expr->type == IR_EXPR_VARIABLE) {
            const char* addressed_var_name = ((IRExprVariable*)addr_of_expr->expr)->name;
            if (strstr(addressed_var_name, "style") != NULL || strncmp(addressed_var_name, "s_", 2) == 0) {
                temp_var_c_type = "lv_style_t*";
                obj_type_for_props = "style";
            }
        }
    } else if (obj_expr->type == IR_EXPR_LITERAL) {
    }


    if (explicit_target_var_name) {
        target_c_var_name = explicit_target_var_name;
        IRStmt* var_decl_stmt = ir_new_var_decl(temp_var_c_type, target_c_var_name, obj_expr);
        ir_block_add_stmt(parent_ir_block, var_decl_stmt);
    } else {
        if (obj_expr->type == IR_EXPR_VARIABLE) {
            target_c_var_name = strdup(((IRExprVariable*)obj_expr)->name);
            generated_var_name_to_free = (char*)target_c_var_name;
            ir_free((IRNode*)obj_expr);
        } else {
            generated_var_name_to_free = generate_unique_var_name(ctx, obj_type_for_props);
            target_c_var_name = generated_var_name_to_free;
            IRStmt* var_decl_stmt = ir_new_var_decl(temp_var_c_type, target_c_var_name, obj_expr);
            ir_block_add_stmt(parent_ir_block, var_decl_stmt);
        }
    }

    if (!target_c_var_name) {
         fprintf(stderr, "Error: target_c_var_name could not be determined in 'with' block (explicit: %s).\n", explicit_target_var_name ? explicit_target_var_name : "NULL");
         if (obj_expr && !(explicit_target_var_name && obj_expr->type != IR_EXPR_VARIABLE) && !(generated_var_name_to_free && obj_expr->type != IR_EXPR_VARIABLE) ) {
         }
         return;
    }

    cJSON* do_json = cJSON_GetObjectItem(with_node, "do");
    if (cJSON_IsObject(do_json)) {
        process_node_internal(ctx, do_json, parent_ir_block, NULL, obj_type_for_props, ui_context, target_c_var_name);
    } else if (do_json && !cJSON_IsNull(do_json)) {
        fprintf(stderr, "Error: 'with' block 'do' key exists but is not an object or null (type: %d) for target C var '%s'. Skipping 'do' processing.\n", do_json->type, target_c_var_name);
    }

    if (generated_var_name_to_free) {
        free(generated_var_name_to_free);
    }
}

static void process_styles(GenContext* ctx, cJSON* styles_json, IRStmtBlock* global_block) {
    if (!cJSON_IsObject(styles_json)) return;
    cJSON* style_item = NULL;
    for (style_item = styles_json->child; style_item != NULL; style_item = style_item->next) {
        const char* style_name_json = style_item->string;
        if (!cJSON_IsObject(style_item)) {
            fprintf(stderr, "Warning: Style '%s' is not a JSON object. Skipping.\n", style_name_json);
            continue;
        }
        char* style_c_var = sanitize_c_identifier(style_name_json);
        const char* registry_key = (style_name_json[0] == '@') ? style_name_json + 1 : style_name_json;
        registry_add_generated_var(ctx->registry, registry_key, style_c_var);

        ir_block_add_stmt(global_block, ir_new_object_allocate_stmt(style_c_var, "lv_style_t", "lv_style_init"));

        process_properties(ctx, style_item, style_c_var, global_block, "style", NULL);
        free(style_c_var);
    }
}

IRStmtBlock* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
    return generate_ir_from_ui_spec_with_registry(ui_spec_root, api_spec, NULL);
}

static void generate_ir_from_ui_spec_internal_logic(GenContext* ctx, const cJSON* ui_spec_root) {
    cJSON* item_json = NULL;
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        cJSON* type_node = cJSON_GetObjectItemCaseSensitive(item_json, "type");
        if (type_node && cJSON_IsString(type_node) && strcmp(type_node->valuestring, "component") == 0) {
            const char* id_str = NULL;
            cJSON* id_json = cJSON_GetObjectItemCaseSensitive(item_json, "id");
            if (cJSON_IsString(id_json) && id_json->valuestring != NULL) {
                id_str = id_json->valuestring;
            } else {
                fprintf(stderr, "Warning: Component definition missing string 'id'. Skipping component registration. Node: %s\n", cJSON_PrintUnformatted(item_json));
                continue;
            }

            if (id_str[0] != '@') {
                fprintf(stderr, "Warning: Component definition id '%s' must start with '@'. Skipping. Node: %s\n", id_str, cJSON_PrintUnformatted(item_json));
                continue;
            }

            cJSON* component_body_json = cJSON_GetObjectItemCaseSensitive(item_json, "root");
            if (!component_body_json) {
                component_body_json = cJSON_GetObjectItemCaseSensitive(item_json, "content");
            }

            if (!component_body_json) {
                fprintf(stderr, "Warning: Component definition '%s' missing 'root' or 'content' definition. Skipping. Node: %s\n", id_str, cJSON_PrintUnformatted(item_json));
                continue;
            }
            registry_add_component(ctx->registry, id_str + 1, component_body_json);
        }
    }

    cJSON_ArrayForEach(item_json, ui_spec_root) {
        cJSON* type_node = cJSON_GetObjectItem(item_json, "type");
        const char* type_str = type_node ? cJSON_GetStringValue(type_node) : NULL;

        if (type_str && strcmp(type_str, "component") == 0) {
            continue;
        }
        process_node(ctx, item_json, ctx->current_global_block, "parent", type_str ? type_str : "obj", NULL);
    }
}

IRStmtBlock* generate_ir_from_ui_spec_with_registry(
    const cJSON* ui_spec_root,
    const ApiSpec* api_spec,
    Registry* string_registry_for_gencontext) {

    if (!ui_spec_root) {
        fprintf(stderr, "Error: UI Spec root is NULL in generate_ir_from_ui_spec_with_registry.\n");
        return NULL;
    }
    if (!api_spec) {
        fprintf(stderr, "Error: API Spec is NULL in generate_ir_from_ui_spec_with_registry.\n");
        return NULL;
    }
    if (!cJSON_IsArray(ui_spec_root)) {
        fprintf(stderr, "Error: UI Spec root must be an array of definitions in generate_ir_from_ui_spec_with_registry.\n");
        return NULL;
    }

    GenContext ctx;
    ctx.api_spec = api_spec;
    ctx.var_counter = 0;

    bool own_registry = false;
    if (string_registry_for_gencontext) {
        ctx.registry = string_registry_for_gencontext;
    } else {
        ctx.registry = registry_create();
        if (!ctx.registry) {
            fprintf(stderr, "Error: Failed to create registry in generate_ir_from_ui_spec_with_registry.\n");
            return NULL;
        }
        own_registry = true;
    }

    IRStmtBlock* root_ir_block = ir_new_block();
    if (!root_ir_block) {
        fprintf(stderr, "Error: Failed to create root IR block in generate_ir_from_ui_spec_with_registry.\n");
        if (own_registry) {
            registry_free(ctx.registry);
        }
        return NULL;
    }
    ctx.current_global_block = root_ir_block;

    generate_ir_from_ui_spec_internal_logic(&ctx, ui_spec_root);

    if (own_registry) {
        registry_free(ctx.registry);
    }
    return root_ir_block;
}

[end of generator.c]

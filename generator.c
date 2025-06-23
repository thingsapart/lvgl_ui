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
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context, const char* expected_enum_type_for_arg, const char* expected_c_type_for_arg); // New signature
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

static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context, const char* expected_enum_type_for_arg, const char* expected_c_type_for_arg) {
    if (!value) return ir_new_literal("NULL");

    // Stricter Boolean Checking (if C type is bool)
    if (expected_c_type_for_arg && strcmp(expected_c_type_for_arg, "bool") == 0) {
        if (cJSON_IsBool(value)) {
            return ir_new_literal(cJSON_IsTrue(value) ? "true" : "false");
        } else if (cJSON_IsNumber(value)) {
            fprintf(stderr, "WARNING: Numeric value %g provided for C bool type. Interpreting non-zero as true.\n", value->valuedouble);
            return ir_new_literal(value->valuedouble != 0 ? "true" : "false");
        } else if (cJSON_IsString(value)) {
            const char* s = value->valuestring;
            if (strcasecmp(s, "true") == 0) return ir_new_literal("true");
            if (strcasecmp(s, "false") == 0) return ir_new_literal("false");
            fprintf(stderr, "ERROR: String value \"%s\" is not a valid boolean. Expected 'true' or 'false'. Defaulting to false.\n", s);
            return ir_new_literal("false");
        } else {
            fprintf(stderr, "ERROR: Incompatible JSON type for C bool. Defaulting to false.\n");
            return ir_new_literal("false");
        }
    }

    if (cJSON_IsString(value)) {
        const char* s_orig = value->valuestring;

        if (s_orig == NULL) return ir_new_literal("NULL");

        if (s_orig[0] == '$' && s_orig[1] != '\0') {
            if (ui_context) {
                cJSON* ctx_val = cJSON_GetObjectItem(ui_context, s_orig + 1);
                if (ctx_val) {
                    // Recursive call, pass along type expectations. expected_c_type_for_arg is passed through.
                    return unmarshal_value(ctx, ctx_val, ui_context, expected_enum_type_for_arg, expected_c_type_for_arg);
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

        // char * vs const char * warning for plain string literals
        if (expected_c_type_for_arg && strcmp(expected_c_type_for_arg, "char *") == 0 && s_orig[0] != '!') {
            // It's okay if it's also an enum, constant, or special prefix like @ or #
            bool is_special_prefix = (s_orig[0] == '@' || s_orig[0] == '#' || s_orig[0] == '%');
            bool is_enum_or_const = false;
            if (!is_special_prefix) {
                if (expected_enum_type_for_arg && api_spec_is_enum_member(ctx->api_spec, expected_enum_type_for_arg, s_orig)) {
                    is_enum_or_const = true;
                }
                if (!is_enum_or_const && api_spec_is_global_enum_member(ctx->api_spec, s_orig)) {
                    is_enum_or_const = true;
                }
                if (!is_enum_or_const && api_spec_is_constant(ctx->api_spec, s_orig)) {
                    is_enum_or_const = true;
                }
            }
            if (!is_special_prefix && !is_enum_or_const) {
                 fprintf(stderr, "WARNING: Plain string literal \"%s\" provided for non-const 'char *' argument. Consider using '!%s' for a heap-allocated string if modification is intended or string must persist.\n", s_orig, s_orig);
            }
        }

        // JSON String for C Number check
        if (expected_c_type_for_arg &&
            (strcmp(expected_c_type_for_arg, "int") == 0 || strcmp(expected_c_type_for_arg, "int32_t") == 0 ||
             strcmp(expected_c_type_for_arg, "uint32_t") == 0 || strcmp(expected_c_type_for_arg, "int16_t") == 0 ||
             strcmp(expected_c_type_for_arg, "uint16_t") == 0 || strcmp(expected_c_type_for_arg, "int8_t") == 0 ||
             strcmp(expected_c_type_for_arg, "uint8_t") == 0 || strcmp(expected_c_type_for_arg, "lv_coord_t") == 0 ||
             strcmp(expected_c_type_for_arg, "float") == 0 || strcmp(expected_c_type_for_arg, "double") == 0)) {

            bool is_numeric_string = true;
            char *endptr;
            if (strstr(expected_c_type_for_arg, "float") || strstr(expected_c_type_for_arg, "double")) {
                strtod(s_orig, &endptr);
            } else {
                strtol(s_orig, &endptr, 10);
            }
            if (*endptr != '\0') { // Conversion stopped before the end of the string
                is_numeric_string = false;
            }

            // If it's not a special prefixed string AND not an enum/constant AND not a valid number string for the expected C type
            if (s_orig[0] != '@' && s_orig[0] != '#' && s_orig[0] != '!' && s_orig[0] != '%' &&
                !(expected_enum_type_for_arg && api_spec_is_enum_member(ctx->api_spec, expected_enum_type_for_arg, s_orig)) &&
                !api_spec_is_global_enum_member(ctx->api_spec, s_orig) &&
                !api_spec_is_constant(ctx->api_spec, s_orig) &&
                !is_numeric_string) {
                fprintf(stderr, "ERROR: String value \"%s\" is not a valid number or recognized constant/enum for C type '%s'.\n", s_orig, expected_c_type_for_arg);
                // Fallback to string literal, C compiler will likely error.
                return ir_new_literal_string(s_orig);
            }
        }


        // This is a plain string, not prefixed by '$', '@', '#', '!' or '%'. Apply enum logic if applicable.
        if (expected_enum_type_for_arg && expected_enum_type_for_arg[0] != '\0') {
            const cJSON* all_enums = api_spec_get_enums(ctx->api_spec);
            if (all_enums) {
                cJSON* specific_enum_type_json = cJSON_GetObjectItem(all_enums, expected_enum_type_for_arg);
                if (specific_enum_type_json && cJSON_IsObject(specific_enum_type_json)) {
                    bool found_in_specific = cJSON_GetObjectItem(specific_enum_type_json, s_orig) != NULL;
                    if (found_in_specific) {
                        return ir_new_literal(s_orig); // Found in specific expected enum
                    } else {
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
                            fprintf(stderr, "ERROR: \"%s\" (enum %s) is not of expected enum type %s.\n", s_orig, actual_enum_type_name, expected_enum_type_for_arg);
                        }
                        // Continue to global search
                    }
                } else {
                     fprintf(stderr, "Warning: Expected enum type '%s' not found in API spec for string value '%s'. Falling back to global search.\n", expected_enum_type_for_arg, s_orig);
                }
            }
        }

        // Fallback for strings: global constant and enum search, then literal string
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
        return ir_new_literal_string(s_orig); // Default for non-prefixed, non-enum/constant strings

    } else if (cJSON_IsNumber(value)) {
        // If an enum type is expected, pass the number directly, but validate it.
        if (expected_enum_type_for_arg && expected_enum_type_for_arg[0] != '\0') {
            if (!api_spec_is_valid_enum_int_value(ctx->api_spec, expected_enum_type_for_arg, (int)value->valuedouble)) {
                fprintf(stderr, "ERROR: Integer value %d is not a defined value for expected enum type '%s'.\n", (int)value->valuedouble, expected_enum_type_for_arg);
                // Consider adding property and object context here if available through params to unmarshal_value in future
            }
            // Optional: fprintf(stderr, "Warning: Integer value %d used for expected enum type '%s'. Prefer string names for clarity.\n", (int)value->valuedouble, expected_enum_type_for_arg);
        }
        // JSON Float for C Integer check
        if (expected_c_type_for_arg &&
            (strcmp(expected_c_type_for_arg, "int") == 0 || strcmp(expected_c_type_for_arg, "int32_t") == 0 ||
             strcmp(expected_c_type_for_arg, "uint32_t") == 0 || strcmp(expected_c_type_for_arg, "int16_t") == 0 ||
             strcmp(expected_c_type_for_arg, "uint16_t") == 0 || strcmp(expected_c_type_for_arg, "int8_t") == 0 ||
             strcmp(expected_c_type_for_arg, "uint8_t") == 0 || strcmp(expected_c_type_for_arg, "lv_coord_t") == 0)) {
            if (value->valuedouble != (double)((int)value->valuedouble)) {
                fprintf(stderr, "WARNING: Floating point value %f provided for C integer type '%s'. Value will be truncated to %d.\n", value->valuedouble, expected_c_type_for_arg, (int)value->valuedouble);
            }
        }
        char buf[32];
        // TODO: Handle float/double C types more precisely if expected_c_type_for_arg indicates float/double.
        snprintf(buf, sizeof(buf), "%d", (int)value->valuedouble);
        return ir_new_literal(buf);
    } else if (cJSON_IsBool(value)) { // This case is now handled earlier if expected_c_type_for_arg is "bool"
        return ir_new_literal(cJSON_IsTrue(value) ? "true" : "false");
    } else if (cJSON_IsArray(value)) {
        IRExprNode* elements = NULL;
        cJSON* elem_json;
        cJSON_ArrayForEach(elem_json, value) {
            // When unmarshalling array elements, we don't have specific type info for each element from here.
            // expected_enum_type_for_arg and expected_c_type_for_arg are NULL.
            ir_expr_list_add(&elements, unmarshal_value(ctx, elem_json, ui_context, NULL, NULL));
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
                    // For "call" objects, we don't have specific type info for each arg from here.
                    IRExpr* arg_expr = unmarshal_value(ctx, arg_json, ui_context, NULL, NULL);
                    ir_expr_list_add(&args_list, arg_expr);
                }
            } else if (args_item != NULL) {
                 // Single argument to a "call" object.
                 IRExpr* arg_expr = unmarshal_value(ctx, args_item, ui_context, NULL, NULL);
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
                // For style references like "@style1", expected_enum and expected_c_type are NULL.
                IRExpr* style_expr = unmarshal_value(ctx, prop, ui_context, NULL, NULL);
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
        const FunctionArg* effective_func_args = NULL;
        bool is_global_func_sig = false; // Flag to indicate if we fetched args from global functions

        if (prop_def->func_args) {
            effective_func_args = prop_def->func_args;
        } else {
            // Try to get function signature from global functions list
            effective_func_args = api_spec_get_function_args_by_name(ctx->api_spec, actual_setter_name_const);
            if (effective_func_args) {
                is_global_func_sig = true;
            } else {
                // This is a fallback for truly simple setters not in global functions (e.g. direct struct member access, though rare for LVGL)
                // or if the setter name construction failed to find a match.
                // Add target object
                ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
                // Unmarshal the single value
                IRExpr* val_expr = unmarshal_value(ctx, prop, ui_context, prop_def->expected_enum_type, prop_def->c_type);
                ir_expr_list_add(&args_list, val_expr);

                // Argument count check for this very simple case
                if (cJSON_IsArray(prop)) {
                     fprintf(stderr, "ERROR: Property '%s' on C var '%s' (type '%s', simple setter '%s') expects a single value, but received an array.\n",
                            prop_name, target_c_var_name, obj_type_for_api_lookup, actual_setter_name_const);
                }
                // No further complex arg processing or default selector logic for this minimal path.
                // This path assumes a very basic setter like `target->member = value;` or `func(target, value);`
            }
        }

        if (effective_func_args) {
            int expected_c_args_total = 0;
            const FunctionArg* counter_arg = effective_func_args;
            while(counter_arg) {
                expected_c_args_total++;
                counter_arg = counter_arg->next;
            }

            int expected_json_arg_count = 0;
            const FunctionArg* first_val_arg = effective_func_args;
            bool first_arg_is_target_obj = false;

            // Add the target object as the first argument to the C function call, if listed in func_args
            if (first_val_arg && first_val_arg->type &&
                (strstr(first_val_arg->type, "lv_obj_t*") || strstr(first_val_arg->type, "lv_style_t*"))) {
                ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
                first_val_arg = first_val_arg->next; // Move to the first actual value argument from JSON
                first_arg_is_target_obj = true;
                expected_json_arg_count = count_expected_json_args(effective_func_args); // Recalculate, excluding target
            } else {
                // Implicit target object not listed as first in func_args (common for style setters or generic obj setters)
                ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
                expected_json_arg_count = count_expected_json_args(effective_func_args); // All func_args are from JSON
            }

            int provided_json_args_count = 0;
            if (cJSON_IsArray(prop)) {
                provided_json_args_count = cJSON_GetArraySize(prop);
            } else if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value") && !cJSON_HasObjectItem(prop, "call")) {
                // Special case for {value: V, part:P, state:S} where func_args might list value, part, state
                // If func_args expects multiple args, this structure is unmarshalled as multiple.
                // If func_args expects one (e.g. a color struct), this would be unmarshalled by unmarshal_value.
                // This part is tricky if func_args expects e.g. (obj, val, part, state)
                // and JSON is { value: V, part:P, state:S}.
                // Let's assume for now that if prop is an object like this, it maps to *one* JSON value that unmarshal_value handles.
                // The argument count check below will catch mismatches if func_args expects more.
                provided_json_args_count = 1;
                 if (expected_json_arg_count > 1 && !(cJSON_IsArray(prop))) {
                     // If the function expects multiple args, but we got a value/part/state object (not an array)
                     // this is likely an error unless unmarshal_value itself can decompose it.
                     // This needs careful handling of how {value,part,state} maps to func_args.
                     // For now, this path assumes it's a single complex value or will be caught by count mismatch.
                 }
            }
             else {
                // If prop is explicitly cJSON_IsNull, it counts as one argument.
                // If prop is another type (string, number, bool), it also counts as one.
                // If prop is NULL (key not found in JSON), then it's 0 arguments. This case should ideally be handled by cJSON_GetObjectItem returning NULL for 'prop'.
                provided_json_args_count = (prop == NULL) ? 0 : 1;
            }


            if (expected_json_arg_count != provided_json_args_count) {
                 // For style properties that might take an implicit selector, allow one less JSON arg
                 bool is_style_setter_func = (strncmp(actual_setter_name_const, "lv_obj_set_style_", 17) == 0) ||
                                          (strncmp(actual_setter_name_const, "lv_style_set_", 13) == 0);
                 bool can_have_implicit_selector = false;
                 if(is_style_setter_func && effective_func_args) {
                     const FunctionArg* last_arg = effective_func_args;
                     // int arg_idx = 0; // Unused
                     while(last_arg->next) { last_arg = last_arg->next; /* arg_idx++; */ }
                     // Check if the last C func arg is selector/state and if JSON provided one less
                     int c_func_value_args_count = expected_c_args_total - (first_arg_is_target_obj ? 1 : 0);
                     if(provided_json_args_count == c_func_value_args_count - 1) {
                        if (strcmp(last_arg->type, "lv_style_selector_t") == 0 || strcmp(last_arg->type, "lv_state_t") == 0) {
                            can_have_implicit_selector = true;
                        }
                     }
                 }

                if (!can_have_implicit_selector) {
                    fprintf(stderr, "ERROR: Property '%s' on C var '%s' (type '%s', setter '%s') expects %d JSON value argument(s), but %d provided.\n",
                        prop_name, target_c_var_name, obj_type_for_api_lookup, actual_setter_name_const, expected_json_arg_count, provided_json_args_count);
                }
            }

            const FunctionArg* current_json_arg_def = first_val_arg;
            if (cJSON_IsArray(prop)) {
                cJSON* val_item_json;
                cJSON_ArrayForEach(val_item_json, prop) {
                    if (!current_json_arg_def) {
                        // Too many JSON args, already warned. Break to avoid crash.
                        break;
                    }
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, val_item_json, ui_context, current_json_arg_def->expected_enum_type, current_json_arg_def->type));
                    current_json_arg_def = current_json_arg_def->next;
                }
            } else if (prop != NULL) { // Single JSON value provided (could be cJSON_IsNull(prop) which unmarshal_value handles)
                if (current_json_arg_def) {
                     // If JSON is {value:V, part:P, state:S} but func_args expects multiple separate args for these,
                     // this simple unmarshal_value won't work. The structure of func_args should guide decomposition.
                     // This is a complex case. For now, assume if prop is object, it's a single complex value.
                    if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value") && !cJSON_HasObjectItem(prop, "call")) {
                        // This is the value/part/state case. How this maps to func_args needs care.
                        // If func_args = (obj, value, part, state), then we need to extract them.
                        // If func_args = (obj, complex_struct_ptr), unmarshal_value might handle it if it creates a temp var.
                        // Current logic assumes direct mapping or unmarshal_value handles complex types.
                        // This part of the logic might need the most refinement if func_args are for value/part/state.

                        // Simplified: if func_args expects multiple, but we have a single obj {value,part,state}
                        // this is an issue. The arg count check above should catch it.
                        // We proceed to unmarshal the single object as the first expected JSON value.
                        ir_expr_list_add(&args_list, unmarshal_value(ctx, prop, ui_context, current_json_arg_def->expected_enum_type, current_json_arg_def->type));
                        current_json_arg_def = current_json_arg_def->next; // Consumed one expected arg
                    } else {
                        // Standard single value for a single function argument
                        ir_expr_list_add(&args_list, unmarshal_value(ctx, prop, ui_context, current_json_arg_def->expected_enum_type, current_json_arg_def->type));
                        current_json_arg_def = current_json_arg_def->next;
                    }
                } else if (provided_json_args_count > 0) {
                     // Provided a JSON value, but no more function arguments were expected (already warned by count check)
                }
            }

            // Implicit Style Selector/State Handling
            int actual_c_args_provided_via_json = 0;
            IRExprNode* temp_node = args_list;
            while(temp_node) { actual_c_args_provided_via_json++; temp_node = temp_node->next; }
            if (first_arg_is_target_obj) actual_c_args_provided_via_json--; // Don't count the obj_ptr itself if it was from func_args list
            else if (args_list && args_list->expr && args_list->expr->type == IR_EXPR_VARIABLE && strcmp(((IRExprVariable*)args_list->expr)->name, target_c_var_name)==0) {
                 actual_c_args_provided_via_json--; // Don't count the obj_ptr if it was added implicitly
            }


            int c_func_total_params = 0;
            const FunctionArg* temp_fa = effective_func_args;
            while(temp_fa) { c_func_total_params++; temp_fa = temp_fa->next; }
            int c_func_value_params = c_func_total_params - (first_arg_is_target_obj ? 1:0);


            if (actual_c_args_provided_via_json == c_func_value_params - 1) {
                // Potentially one argument missing, check if it's a selector/state
                const FunctionArg* last_c_func_arg = effective_func_args;
                int k = 0;
                while(last_c_func_arg && k < (c_func_value_params -1) ) { // find the last actual C value arg descriptor
                    last_c_func_arg = last_c_func_arg->next;
                    k++;
                }
                if(last_c_func_arg && last_c_func_arg->type) { // Check if this last C arg is selector or state
                    // bool is_obj_style_setter = strncmp(actual_setter_name_const, "lv_obj_set_style_", 17) == 0; // Unused
                    bool is_style_style_setter = strncmp(actual_setter_name_const, "lv_style_set_", 13) == 0;

                    if (strcmp(last_c_func_arg->type, "lv_style_selector_t") == 0) {
                        ir_expr_list_add(&args_list, ir_new_literal("LV_PART_MAIN | LV_STATE_DEFAULT"));
                    } else if (strcmp(last_c_func_arg->type, "lv_state_t") == 0 && is_style_style_setter) {
                        // Only default lv_state_t for lv_style_set_ functions if it's the pattern (style, state, value)
                        // and JSON only gave value.
                        ir_expr_list_add(&args_list, ir_new_literal("LV_STATE_DEFAULT"));
                    }
                }
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
    const char* type_str_from_json = NULL; // The "type" string directly from the JSON node
    const char* obj_type_for_api_lookup = NULL; // The type string to use for API spec lookups
    const char* type_str_for_registry = NULL; // The JSON type string to register for this ID
    const WidgetDefinition* widget_def = NULL;

    if (forced_c_var_name) {
        c_var_name_for_node = (char*)forced_c_var_name;
        // When forced_c_var_name is used (e.g. 'do' block of 'with'), default_obj_type is the primary type.
        type_str_from_json = default_obj_type; // This is the type of the 'with' target.
        obj_type_for_api_lookup = default_obj_type;
        type_str_for_registry = default_obj_type;
    } else {
        cJSON* named_item = cJSON_GetObjectItem(node_json, "named");
        cJSON* id_item = cJSON_GetObjectItem(node_json, "id");
        const char* c_name_source = NULL;
        const char* id_key_for_registry_val = NULL;

        if (id_item && cJSON_IsString(id_item) && id_item->valuestring[0] == '@') {
            id_key_for_registry_val = id_item->valuestring + 1;
        }

        if (named_item && cJSON_IsString(named_item) && named_item->valuestring[0] != '\0') {
            c_name_source = named_item->valuestring;
            if (!id_key_for_registry_val) {
                 if (named_item->valuestring[0] != '@') { // Don't use "@foo" from "named" as a direct registry key unless it's the only option
                    id_key_for_registry_val = c_name_source;
                 }
            }
        } else if (id_key_for_registry_val) {
            c_name_source = id_key_for_registry_val;
        }

        // Determine type_str_from_json (from "type" key or default)
        cJSON* type_item_json = cJSON_GetObjectItem(node_json, "type");
        if (type_item_json && cJSON_IsString(type_item_json)) {
            type_str_from_json = type_item_json->valuestring;
        } else {
            type_str_from_json = default_obj_type;
        }

        // Initial assumption for type_str_for_registry and obj_type_for_api_lookup
        type_str_for_registry = type_str_from_json;
        obj_type_for_api_lookup = type_str_from_json;


        if (c_name_source) {
            allocated_c_var_name = sanitize_c_identifier(c_name_source);
            c_var_name_for_node = allocated_c_var_name;
            if (id_key_for_registry_val) { // If an @id was present, always use it for generated var registration
                registry_add_generated_var(ctx->registry, id_key_for_registry_val, c_var_name_for_node);
            } else if (c_name_source[0] != '@') { // If "named" was "foo" (no @id), register "foo"
                 registry_add_generated_var(ctx->registry, c_name_source, c_var_name_for_node);
            }
        } else {
            // For unnamed nodes, generate a unique name. Base it on type_str_from_json if available.
            const char* base_for_name_gen = type_str_from_json;
            if (!base_for_name_gen || base_for_name_gen[0] == '@') { // If type is like "@comp", use "obj"
                base_for_name_gen = "obj";
            }
            allocated_c_var_name = generate_unique_var_name(ctx, base_for_name_gen);
            c_var_name_for_node = allocated_c_var_name;
        }
    }

    bool is_with_assignment_node = false;
    cJSON* named_attr_for_check = cJSON_GetObjectItem(node_json, "named");
    cJSON* with_attr_for_check = cJSON_GetObjectItem(node_json, "with");

    // A "with assignment node" is one that *only* has 'named', 'id', 'with', 'context', comments.
    // It assigns the result of 'with' to 'named'.
    if (named_attr_for_check && cJSON_IsString(named_attr_for_check) && named_attr_for_check->valuestring[0] != '\0' && with_attr_for_check) {
        is_with_assignment_node = true;
        cJSON* item = NULL;
        for (item = node_json->child; item != NULL; item = item->next) {
            const char* key = item->string;
            if (strcmp(key, "type") != 0 && // "type" is allowed on 'with' assignment for clarity but doesn't create new obj
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


    // Handle `use-view` first as it defines the object and its type for properties
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

                obj_type_for_api_lookup = comp_root_type_str; // Use component's root type for API lookups
                type_str_for_registry = comp_root_type_str;   // And for registry type

                // Process the component's root, forcing the current node's C variable name
                process_node_internal(ctx, (cJSON*)component_root_json, current_node_ir_block, parent_c_var, comp_root_type_str, effective_context, c_var_name_for_node);
                object_successfully_created = true; // Assume component root creation is successful

                // Process overriding properties from the use-view node itself
                process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, obj_type_for_api_lookup, effective_context);
            }
        }
        // For use-view, the object_successfully_created flag and registry are handled here or within the recursive call.
        // Clean up and return
        if (allocated_c_var_name) free(allocated_c_var_name);
        if (own_effective_context) cJSON_Delete(effective_context);
        // The main registry_add_pointer call later will handle use-view if it has an id.
        // No, this is wrong. The 'object_successfully_created' and 'type_str_for_registry' must be set correctly *before* the final registry add.
        // The process_node_internal call for the component root will handle ITS OWN registry if IT has an ID.
        // The CURRENT node (the use-view node) needs its ID registered with ITS effective type.
        // The 'type_str_for_registry' should be the component's root type.
    } else if (is_with_assignment_node) {
        // This node just names the result of a 'with' block. No new object is created here.
        // Properties are not processed here. The 'with' block handles its own properties.
        ir_block_add_stmt(current_node_ir_block, ir_new_comment( "// Node is a 'with' assignment target. Processing 'with' blocks for assignment."));
        cJSON* item_w_assign = NULL;
        for (item_w_assign = node_json->child; item_w_assign != NULL; item_w_assign = item_w_assign->next) {
            if (item_w_assign->string && strcmp(item_w_assign->string, "with") == 0) {
                // The type of the assigned variable comes from the 'with' block's 'obj'
                // process_single_with_block will declare the var.
                // We need to determine type_str_for_registry if this node has an ID.
                cJSON* with_obj_item = cJSON_GetObjectItem(item_w_assign, "obj");
                if (with_obj_item && cJSON_IsString(with_obj_item)) {
                    const char* with_obj_ref = with_obj_item->valuestring;
                    if (with_obj_ref[0] == '@') {
                        const char* reg_type = registry_get_json_type_for_id(ctx->registry, with_obj_ref + 1);
                        if (reg_type) {
                            type_str_for_registry = reg_type;
                            obj_type_for_api_lookup = reg_type;
                        } else {
                            // Could be a style, or an undeclared var. Default to obj for safety.
                            type_str_for_registry = strstr(with_obj_ref, "style") ? "style" : "obj";
                            obj_type_for_api_lookup = type_str_for_registry;
                        }
                    } else { // Direct C var name - cannot infer type easily here, default.
                        type_str_for_registry = "obj"; // Or try to parse from name?
                        obj_type_for_api_lookup = "obj";
                    }
                } else { // Complex 'obj' or missing
                    type_str_for_registry = "obj"; // Fallback
                    obj_type_for_api_lookup = "obj";
                }
                process_single_with_block(ctx, item_w_assign, current_node_ir_block, effective_context, c_var_name_for_node);
                object_successfully_created = true; // The variable is created/assigned.
            }
        }
    } else if (forced_c_var_name) { // Inside a 'with ... do:' block
        // type_str_from_json, obj_type_for_api_lookup, type_str_for_registry already set from default_obj_type
        widget_def = api_spec_find_widget(ctx->api_spec, obj_type_for_api_lookup);
        // No new object created here, properties applied to forced_c_var_name
        process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, obj_type_for_api_lookup, effective_context);
        object_successfully_created = true; // It's an existing object

        cJSON* children_json_in_do = cJSON_GetObjectItem(node_json, "children");
        if (cJSON_IsArray(children_json_in_do)) {
            cJSON* child_node_json_in_do;
            cJSON_ArrayForEach(child_node_json_in_do, children_json_in_do) {
                process_node_internal(ctx, child_node_json_in_do, current_node_ir_block, c_var_name_for_node, "obj", effective_context, NULL);
            }
        }
    } else { // Standard node processing (not use-view, not with-assignment, not forced_c_var)
        // type_str_from_json, obj_type_for_api_lookup, type_str_for_registry are already initialized from "type" or default.

        if (!type_str_from_json || type_str_from_json[0] == '\0') {
             fprintf(stderr, "Error: Node missing valid 'type' (or default_obj_type for component root). C var: %s. Skipping node processing.\n", c_var_name_for_node);
             if (allocated_c_var_name) free(allocated_c_var_name);
             if (own_effective_context) cJSON_Delete(effective_context);
             return;
        }

        // If type is a component alias like type: "@my_component", resolve it
        if (type_str_from_json[0] == '@') {
            const cJSON* component_root_json = registry_get_component(ctx->registry, type_str_from_json + 1);
            if (!component_root_json) {
                fprintf(stderr, "Error: Component definition '%s' (used as type) not found. Skipping node C var: %s.\n", type_str_from_json, c_var_name_for_node);
                if (allocated_c_var_name) free(allocated_c_var_name);
                if (own_effective_context) cJSON_Delete(effective_context);
                return;
            }
            cJSON* comp_root_type_item = cJSON_GetObjectItem(component_root_json, "type");
            const char* comp_root_type_str = comp_root_type_item ? cJSON_GetStringValue(comp_root_type_item) : "obj";

            obj_type_for_api_lookup = comp_root_type_str; // Use component's root type for API lookups
            type_str_for_registry = comp_root_type_str;   // And for registry type

            // Process the component's root, forcing the current node's C variable name
            process_node_internal(ctx, (cJSON*)component_root_json, current_node_ir_block, parent_c_var, comp_root_type_str, effective_context, c_var_name_for_node);
            object_successfully_created = true;
            // Apply overriding properties from the alias node itself
            process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, obj_type_for_api_lookup, effective_context);
            // Children are handled by the recursive call to process_node_internal for the component root.
        } else {
            // Standard widget type (not a component alias)
            widget_def = api_spec_find_widget(ctx->api_spec, obj_type_for_api_lookup);

            if (widget_def && widget_def->create && widget_def->create[0] != '\0') {
                IRExpr* parent_var_expr = (parent_c_var && parent_c_var[0] != '\0') ? ir_new_variable(parent_c_var) : NULL;
                ir_block_add_stmt(current_node_ir_block,
                                  ir_new_widget_allocate_stmt(c_var_name_for_node,
                                                              "lv_obj_t", // Usually lv_obj_t, specific types handled by init_func
                                                              widget_def->create,
                                                              parent_var_expr));
                object_successfully_created = true;
            } else if (widget_def && widget_def->init_func && widget_def->init_func[0] != '\0') {
                if (!widget_def->c_type || widget_def->c_type[0] == '\0') {
                    fprintf(stderr, "Error: Object type '%s' (var %s) has init_func '%s' but no c_type defined. Skipping.\n",
                            obj_type_for_api_lookup, c_var_name_for_node, widget_def->init_func);
                } else {
                    ir_block_add_stmt(current_node_ir_block,
                                      ir_new_object_allocate_stmt(c_var_name_for_node,
                                                                  widget_def->c_type,
                                                                  widget_def->init_func));
                    object_successfully_created = true;
                }
            } else if (strcmp(obj_type_for_api_lookup, "obj") == 0) { // Generic "obj" type
                IRExpr* parent_var_expr = (parent_c_var && parent_c_var[0] != '\0') ? ir_new_variable(parent_c_var) : NULL;
                ir_block_add_stmt(current_node_ir_block,
                                  ir_new_widget_allocate_stmt(c_var_name_for_node,
                                                              "lv_obj_t",
                                                              "lv_obj_create",
                                                              parent_var_expr));
                object_successfully_created = true;
            } else if (strcmp(obj_type_for_api_lookup, "style") == 0) { // Specific "style" type
                 ir_block_add_stmt(current_node_ir_block,
                              ir_new_object_allocate_stmt(c_var_name_for_node,
                                                          "lv_style_t",
                                                          "lv_style_init"));
                object_successfully_created = true;
            } else {
                // Type is not a component, not 'obj', not 'style', and has no create/init.
                // This could be a base type for other widgets, or an error.
                // For now, if it's not 'obj' or 'style', assume it's not directly creatable unless widget_def says so.
                // If it's a known widget type but no create/init, it might be an abstract type.
                // Fallback to lv_obj_create if no other rule applies, but this might be incorrect for some types.
                fprintf(stderr, "Info: Type '%s' (var %s) has no specific create/init in API. Creating as generic lv_obj_t and applying properties.\n", obj_type_for_api_lookup, c_var_name_for_node);
                IRExpr* parent_var_expr = (parent_c_var && parent_c_var[0] != '\0') ? ir_new_variable(parent_c_var) : NULL;
                ir_block_add_stmt(current_node_ir_block,
                                  ir_new_widget_allocate_stmt(c_var_name_for_node,
                                                              "lv_obj_t",
                                                              "lv_obj_create",
                                                              parent_var_expr));
                object_successfully_created = true;
            }

            if (object_successfully_created) {
                process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, obj_type_for_api_lookup, effective_context);
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

    // Add to registry if an object was successfully created/referenced and has an ID
    if (object_successfully_created && c_var_name_for_node) {
        cJSON* id_item_for_reg = cJSON_GetObjectItem(node_json, "id");
        if (id_item_for_reg && cJSON_IsString(id_item_for_reg) && id_item_for_reg->valuestring && id_item_for_reg->valuestring[0] == '@') {
            const char* json_id_val = id_item_for_reg->valuestring + 1;

            // Ensure type_str_for_registry is valid. It should have been set correctly by the logic above.
            // For use-view, it's comp_root_type_str. For type: "@comp", it's comp_root_type_str.
            // For direct types, it's type_str_from_json. For 'with' assignment, it's inferred.
            const char* final_type_for_registry = type_str_for_registry ? type_str_for_registry : "obj"; // Fallback

            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_variable("ui_registry"));
            ir_expr_list_add(&args, ir_new_variable(c_var_name_for_node));
            ir_expr_list_add(&args, ir_new_literal_string(json_id_val));
            ir_expr_list_add(&args, ir_new_literal_string(final_type_for_registry));
            IRStmt* add_ptr_stmt = ir_new_func_call_stmt("registry_add_pointer", args);
            ir_block_add_stmt(current_node_ir_block, add_ptr_stmt);
        }
    }

    // Process 'with' blocks for regular nodes (not 'with assignment' nodes and not 'do' blocks)
    if (!is_with_assignment_node && !forced_c_var_name) {
        // Determine if this node type can meaningfully have 'with' blocks applied to it.
        // Generally, if an object was created or is being directly manipulated.
        bool regular_node_can_have_with = object_successfully_created;

        if (c_var_name_for_node && regular_node_can_have_with) {
            cJSON* item_w_regular = NULL;
            for (item_w_regular = node_json->child; item_w_regular != NULL; item_w_regular = item_w_regular->next) {
                if (item_w_regular->string && strcmp(item_w_regular->string, "with") == 0) {
                    // 'with' block on a regular node does not get an explicit target var name from here.
                    // It will generate its own temp var if needed or use direct var from its 'obj'.
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
    // For 'with obj:', expected_enum and expected_c_type are generally not known/applicable at this point.
    IRExpr* obj_expr = unmarshal_value(ctx, obj_json, ui_context, NULL, NULL);
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
        // Try to get type from registry if it's an @-reference
        if (cJSON_IsString(obj_json) && obj_json->valuestring[0] == '@') {
            const char* registered_json_type = registry_get_json_type_for_id(ctx->registry, obj_json->valuestring + 1);
            if (registered_json_type) {
                obj_type_for_props = registered_json_type;
                if (strcmp(obj_type_for_props, "style") == 0) {
                    temp_var_c_type = "lv_style_t*";
                } else {
                    // For other widget types, it's still lv_obj_t* for the variable
                    temp_var_c_type = "lv_obj_t*";
                }
            } else { // Fallback if not in registry (e.g. forward reference or error)
                 obj_type_for_props = strstr(var_name, "style") || strncmp(var_name, "s_", 2) == 0 ? "style" : "obj";
                 temp_var_c_type = strcmp(obj_type_for_props, "style") == 0 ? "lv_style_t*" : "lv_obj_t*";
                 fprintf(stderr, "Warning: ID '%s' in 'with.obj' not found in registry for type inference. Defaulting type to '%s'.\n", obj_json->valuestring, obj_type_for_props);
            }
        } else { // Plain variable name, try to infer from name
            obj_type_for_props = strstr(var_name, "style") || strncmp(var_name, "s_", 2) == 0 ? "style" : "obj";
            temp_var_c_type = strcmp(obj_type_for_props, "style") == 0 ? "lv_style_t*" : "lv_obj_t*";
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
                temp_var_c_type = "lv_style_t*"; // Taking address of a style var makes it a style pointer
                obj_type_for_props = "style";
            }
        }
    } else if (obj_expr->type == IR_EXPR_LITERAL) {
        // This case is unlikely for 'with.obj' unless it's "NULL", but handle defensively.
        obj_type_for_props = "obj"; // Cannot infer much from a literal like 0 or NULL
        temp_var_c_type = "void*"; // Most generic
    }


    if (explicit_target_var_name) {
        target_c_var_name = explicit_target_var_name;
        // The temp_var_c_type here should ideally match the actual type of explicit_target_var_name if known.
        // For 'with assignment', the explicit_target_var_name is freshly declared.
        // Its C type needs to be determined by the obj_expr.
        const WidgetDefinition* w_def_for_type = api_spec_find_widget(ctx->api_spec, obj_type_for_props);
        const char* c_type_for_decl = temp_var_c_type; // Default from inference above
        if (w_def_for_type && w_def_for_type->c_type) {
            c_type_for_decl = w_def_for_type->c_type;
        } else if (strcmp(obj_type_for_props, "style") == 0) {
            c_type_for_decl = "lv_style_t*";
        } else if (strcmp(obj_type_for_props, "obj") == 0) {
            c_type_for_decl = "lv_obj_t*";
        }
        // Ensure obj_expr isn't an address_of if we are not expecting a pointer to pointer.
        // If obj_expr is already lv_style_t* and c_type_for_decl is lv_style_t*, it's fine.
        // If obj_expr is &my_style (IR_EXPR_ADDRESS_OF of IR_EXPR_VARIABLE my_style which is lv_style_t)
        // and c_type_for_decl is lv_style_t*, this is also fine.
        IRStmt* var_decl_stmt = ir_new_var_decl(c_type_for_decl, target_c_var_name, obj_expr);
        ir_block_add_stmt(parent_ir_block, var_decl_stmt);
    } else { // No explicit target, typically a 'with' block on a standard node
        if (obj_expr->type == IR_EXPR_VARIABLE) {
            // If obj_expr is just a variable, use that variable directly. No new declaration.
            target_c_var_name = strdup(((IRExprVariable*)obj_expr)->name);
            generated_var_name_to_free = (char*)target_c_var_name;
            ir_free((IRNode*)obj_expr); // Free the expression, we only need the name
        } else {
            // For complex expressions (func call, etc.), create a temporary variable.
            generated_var_name_to_free = generate_unique_var_name(ctx, obj_type_for_props);
            target_c_var_name = generated_var_name_to_free;
            // Determine C type for the temporary variable
            const WidgetDefinition* w_def_for_type = api_spec_find_widget(ctx->api_spec, obj_type_for_props);
            const char* c_type_for_decl = temp_var_c_type; // Default from inference
             if (w_def_for_type && w_def_for_type->c_type) {
                c_type_for_decl = w_def_for_type->c_type;
            } else if (strcmp(obj_type_for_props, "style") == 0) {
                c_type_for_decl = "lv_style_t*";
            } else if (strcmp(obj_type_for_props, "obj") == 0) { // Could be any lv_obj_t derived
                c_type_for_decl = "lv_obj_t*";
            }
            IRStmt* var_decl_stmt = ir_new_var_decl(c_type_for_decl, target_c_var_name, obj_expr);
            ir_block_add_stmt(parent_ir_block, var_decl_stmt);
        }
    }

    if (!target_c_var_name) {
         fprintf(stderr, "Error: target_c_var_name could not be determined in 'with' block (explicit: %s).\n", explicit_target_var_name ? explicit_target_var_name : "NULL");
         // Avoid double free if obj_expr was already handled (e.g. explicit_target_var_name path)
         if (obj_expr && !(explicit_target_var_name) && !(generated_var_name_to_free && obj_expr->type != IR_EXPR_VARIABLE)) {
             // If obj_expr was not used for a var_decl and not a simple variable whose name was taken, free it.
             // This condition is tricky; primarily, if obj_expr was passed to ir_new_var_decl, it's owned by that.
             // If it was a variable and we strdup'd its name, we freed the original IR_EXPR_VARIABLE.
             // This path suggests obj_expr might be unused if target_c_var_name is NULL.
         }
         return;
    }

    cJSON* do_json = cJSON_GetObjectItem(with_node, "do");
    if (cJSON_IsObject(do_json)) {
        // The 'default_obj_type' for the 'do' block is the type we inferred for 'obj_type_for_props'
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
        const char* registry_key_for_var = (style_name_json[0] == '@') ? style_name_json + 1 : style_name_json;
        registry_add_generated_var(ctx->registry, registry_key_for_var, style_c_var);

        // Also add to pointer registry with its JSON type "style"
        const char* id_for_ptr_reg = NULL;
        if (style_name_json[0] == '@') {
            id_for_ptr_reg = style_name_json + 1;
        } else {
            // For styles defined like "my_style_name": { ... }, we might not have an @id for pointer registry.
            // However, if they are referenced by @my_style_name, this explicit registration helps.
            // For consistency, let's register them if they look like an ID.
            id_for_ptr_reg = style_name_json; // Use the key as ID if not starting with @
        }

        ir_block_add_stmt(global_block, ir_new_object_allocate_stmt(style_c_var, "lv_style_t", "lv_style_init"));

        // Add to pointer registry *after* allocation, so var name is valid
        IRExprNode* args = NULL;
        ir_expr_list_add(&args, ir_new_variable("ui_registry"));
        ir_expr_list_add(&args, ir_new_variable(style_c_var)); // Pass the C variable itself
        ir_expr_list_add(&args, ir_new_literal_string(id_for_ptr_reg));
        ir_expr_list_add(&args, ir_new_literal_string("style")); // JSON type is "style"
        IRStmt* add_ptr_stmt = ir_new_func_call_stmt("registry_add_pointer", args);
        ir_block_add_stmt(global_block, add_ptr_stmt);


        process_properties(ctx, style_item, style_c_var, global_block, "style", NULL); // obj_type_for_api_lookup is "style"
        free(style_c_var);
    }
}

IRStmtBlock* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
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
    // For 'with obj:', expected_enum and expected_c_type are generally not known/applicable at this point.
    IRExpr* obj_expr = unmarshal_value(ctx, obj_json, ui_context, NULL, NULL);
    if (!obj_expr) {
        fprintf(stderr, "Error: Failed to unmarshal 'obj' in 'with' block (when processing for target: %s).\n", explicit_target_var_name ? explicit_target_var_name : "temp_var");
        return;
    }

    const char* target_c_var_name = NULL;
    char* generated_var_name_to_free = NULL;
    const char* obj_type_for_props = "obj";     // Default
    const char* temp_var_c_type = "lv_obj_t*"; // Default

    if (cJSON_IsString(obj_json) && obj_json->valuestring[0] == '@') {
        const char* id_key = obj_json->valuestring + 1;
        const char* registered_json_type = registry_get_json_type_for_id(ctx->registry, id_key);
        if (registered_json_type) {
            obj_type_for_props = registered_json_type;
            if (strcmp(obj_type_for_props, "style") == 0) {
                temp_var_c_type = "lv_style_t*";
            } else {
                const WidgetDefinition* w_def = api_spec_find_widget(ctx->api_spec, obj_type_for_props);
                if (w_def && w_def->c_type) {
                    temp_var_c_type = w_def->c_type; // e.g. lv_obj_t*
                } else {
                    temp_var_c_type = "lv_obj_t*"; // Fallback for custom or unknown widget types
                }
            }
        } else {
            // Not in registry, try basic name heuristic for generated C var if obj_expr is a variable
            if (obj_expr->type == IR_EXPR_VARIABLE) {
                const char* var_name = ((IRExprVariable*)obj_expr)->name;
                 obj_type_for_props = strstr(var_name, "style") || strncmp(var_name, "s_", 2) == 0 ? "style" : "obj";
                 temp_var_c_type = strcmp(obj_type_for_props, "style") == 0 ? "lv_style_t*" : "lv_obj_t*";
            }
            fprintf(stderr, "Warning: ID '%s' in 'with.obj' not found in registry for precise type inference. Defaulting type to '%s' (C type '%s').\n", id_key, obj_type_for_props, temp_var_c_type);
        }
    } else if (obj_expr->type == IR_EXPR_VARIABLE) { // Not an @-id, but a direct variable name
        const char* var_name = ((IRExprVariable*)obj_expr)->name;
        // Basic heuristic for non-@ variables
        if (strstr(var_name, "style") != NULL || strncmp(var_name, "s_", 2) == 0) {
            obj_type_for_props = "style";
            temp_var_c_type = "lv_style_t*";
        } else if (strstr(var_name, "label") != NULL) { // Add more heuristics if needed
            obj_type_for_props = "label";
            temp_var_c_type = "lv_obj_t*";
        } else {
            obj_type_for_props = "obj"; // Default for plain variables
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

        // Also add to pointer registry with its JSON type "style"
        const char* id_for_ptr_reg = NULL;
        if (style_name_json[0] == '@') {
            id_for_ptr_reg = style_name_json + 1;
        } else {
            // For styles defined like "my_style_name": { ... }, we might not have an @id for pointer registry.
            // However, if they are referenced by @my_style_name, this explicit registration helps.
            // For consistency, let's register them if they look like an ID.
            id_for_ptr_reg = style_name_json; // Use the key as ID if not starting with @
        }

        ir_block_add_stmt(global_block, ir_new_object_allocate_stmt(style_c_var, "lv_style_t", "lv_style_init"));

        // Add to pointer registry *after* allocation, so var name is valid
        IRExprNode* args = NULL;
        ir_expr_list_add(&args, ir_new_variable("ui_registry"));
        ir_expr_list_add(&args, ir_new_variable(style_c_var)); // Pass the C variable itself
        ir_expr_list_add(&args, ir_new_literal_string(id_for_ptr_reg));
        ir_expr_list_add(&args, ir_new_literal_string("style")); // JSON type is "style"
        IRStmt* add_ptr_stmt = ir_new_func_call_stmt("registry_add_pointer", args);
        ir_block_add_stmt(global_block, add_ptr_stmt);


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

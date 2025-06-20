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

// Helper to get C-type string from an object type string (e.g., "button" -> "lv_obj_t*")
static const char* get_c_type_for_object_type_str(const ApiSpec* api_spec, const char* obj_type_str, const char* default_c_type) {
    if (!obj_type_str || obj_type_str[0] == '\0') {
        return default_c_type;
    }

    size_t len = strlen(obj_type_str);
    if (len > 2) {
        if (strcmp(obj_type_str + len - 3, "_t*") == 0) { // e.g. lv_obj_t*
            return obj_type_str;
        }
        if (strcmp(obj_type_str + len - 2, "_t") == 0) { // e.g. lv_style_t
            // This could also be part of a name like "my_custom_type_t" if not a pointer.
            // For now, assume if it ends with _t, it's a C type name.
            return obj_type_str;
        }
    }
    if (strcmp(obj_type_str, "bool") == 0 || strcmp(obj_type_str, "int") == 0 || strcmp(obj_type_str, "const char*") == 0 || strcmp(obj_type_str, "char*") == 0 || strcmp(obj_type_str, "float") == 0 || strcmp(obj_type_str, "double") == 0) {
        return obj_type_str;
    }


    if (api_spec) {
        const WidgetDefinition* widget_def = api_spec_find_widget(api_spec, obj_type_str);
        if (widget_def && widget_def->c_type && widget_def->c_type[0] != '\0') {
            return widget_def->c_type;
        }
    }
    // If obj_type_str is "style", map to "lv_style_t".
    // api_spec_find_widget might not return "style" as a widget.
    if (strcmp(obj_type_str, "style") == 0) {
        return "lv_style_t"; // Typically lv_style_t variables are not pointers unless passed by address.
                             // But for variable declarations, lv_style_t is the type.
    }

    // Fallback if no specific widget definition C-type is found
    // For example, if obj_type_str is "obj" or an unknown type.
    if (strcmp(obj_type_str, "obj") == 0) {
        return "lv_obj_t*";
    }

    return default_c_type;
}

// Helper to get object type string (e.g. "button") from a C-type string (e.g. "lv_obj_t*")
static const char* get_object_type_from_c_type(const ApiSpec* api_spec, const char* c_type_str, const char* default_obj_type) {
    if (!c_type_str || c_type_str[0] == '\0') {
        return default_obj_type;
    }

    if (strcmp(c_type_str, "lv_style_t") == 0 || strcmp(c_type_str, "lv_style_t*") == 0) {
        return "style";
    }

    // Iterate through known widget definitions to find a match for the C type
    if (api_spec && api_spec->widgets_list_head) {
        WidgetMapNode* current_widget_node = api_spec->widgets_list_head;
        while (current_widget_node) {
            if (current_widget_node->widget && current_widget_node->widget->c_type &&
                strcmp(current_widget_node->widget->c_type, c_type_str) == 0) {
                if (current_widget_node->widget->name) { // Widget's JSON type name, e.g., "button"
                    return current_widget_node->widget->name;
                }
            }
            current_widget_node = current_widget_node->next;
        }
    }

    // If no specific widget c_type matched, but it's lv_obj_t*, it's a generic "obj"
    if (strcmp(c_type_str, "lv_obj_t*") == 0) {
        return "obj";
    }

    if (strcmp(c_type_str, "void*") == 0) { // void* could be anything, use default
        return default_obj_type;
    }

    // Fallback if no specific C type mapping is found
    _dprintf(stderr, "DEBUG: get_object_type_from_c_type: No specific object type found for C type '%s', using default '%s'.\n", c_type_str, default_obj_type);
    return default_obj_type;
}


static void process_properties(GenContext* ctx, cJSON* props_json, const char* target_c_var_name, IRStmtBlock* current_block, const char* obj_type_for_api_lookup, cJSON* ui_context);
static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context, const char* explicit_target_var_name);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);

// --- Enum Validation Helper Functions ---
static const cJSON* get_enum_definition_if_type_is_enum(const ApiSpec* api_spec, const char* c_type_name) {
    if (!api_spec || !c_type_name) return NULL;
    const cJSON* all_enums = api_spec_get_enums(api_spec);
    if (!all_enums) return NULL;
    const cJSON* enum_def = cJSON_GetObjectItem(all_enums, c_type_name);
    if (enum_def && cJSON_IsObject(enum_def)) {
        return enum_def;
    }
    return NULL;
}

// Returns true if enum_value_str is a valid member of the enum defined by specific_enum_def_json
static bool is_enum_member_valid_for_definition(const cJSON* specific_enum_def_json, const char* enum_value_str) {
    if (!specific_enum_def_json || !cJSON_IsObject(specific_enum_def_json) || !enum_value_str) {
        return false; // Cannot validate
    }
    if (cJSON_GetObjectItem(specific_enum_def_json, enum_value_str)) {
        return true; // Member found
    }
    return false; // Member not found
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

static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context) {
    if (!value) return ir_new_literal("NULL");

    if (cJSON_IsString(value)) {
        const char* s_orig = value->valuestring;

        if (s_orig == NULL) return ir_new_literal("NULL");

        if (s_orig[0] == '$' && s_orig[1] != '\0') {
            if (ui_context) {
                cJSON* ctx_val = cJSON_GetObjectItem(ui_context, s_orig + 1);
                if (ctx_val) {
                    return unmarshal_value(ctx, ctx_val, ui_context);
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
                 // Check if the ID or C variable name suggests it's a style object.
                 // Style objects (lv_style_t) are typically passed by address (&style_var).
                 // Other registered objects (like lv_obj_t*) are passed by value (the pointer itself).
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
        // MODIFICATION START: Handle '!' prefix for string registration
        if (s_orig[0] == '!' && s_orig[1] != '\0') {
            const char* registered_string = registry_add_str(ctx->registry, s_orig + 1);
            if (registered_string) {
                return ir_new_literal_string(registered_string);
            } else {
                fprintf(stderr, "Warning: registry_add_str failed for value: %s. Returning NULL literal.\n", s_orig + 1);
                return ir_new_literal("NULL"); // Or a default error string literal
            }
        }
        // MODIFICATION END

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

        const cJSON* constants = api_spec_get_constants(ctx->api_spec);
        if (constants && cJSON_GetObjectItem(constants, s_orig)) {
            return ir_new_literal(s_orig);
        }

        const cJSON* all_enum_types_json = api_spec_get_enums(ctx->api_spec);
        if (all_enum_types_json && cJSON_IsObject(all_enum_types_json)) {
            cJSON* enum_type_definition_json = NULL;
            for (enum_type_definition_json = all_enum_types_json->child; enum_type_definition_json != NULL; enum_type_definition_json = enum_type_definition_json->next) {
                if (cJSON_IsObject(enum_type_definition_json)) {
                    if (cJSON_GetObjectItem(enum_type_definition_json, s_orig)) {
                        return ir_new_literal(s_orig); // Found s_orig as a key in this enum type definition
                    }
                }
            }
        }
        // If not found in constants or any enum definitions, then treat as a string literal by default.
        _dprintf(stderr, "DEBUG_PRINT: unmarshal_value: String '%s' falling back to IR_EXPR_LITERAL_STRING\n", s_orig);
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
            ir_expr_list_add(&elements, unmarshal_value(ctx, elem_json, ui_context));
        }
        return ir_new_array(elements);
    } else if (cJSON_IsObject(value)) {
        cJSON* call_item = cJSON_GetObjectItem(value, "call");
        cJSON* args_item = cJSON_GetObjectItem(value, "args");
        if (cJSON_IsString(call_item)) {
            const char* call_name = call_item->valuestring;
            _dprintf(stderr, "DEBUG_PRINT: unmarshal_value: Processing 'call' object. Function name: '%s'\n", call_name);
            IRExprNode* args_list = NULL;
            if (cJSON_IsArray(args_item)) {
                 cJSON* arg_json;
                 int arg_idx = 0;
                 const FunctionDefinition* fd = api_spec_find_function(ctx->api_spec, call_name);

                 cJSON_ArrayForEach(arg_json, args_item) {
                    char* arg_json_str = cJSON_PrintUnformatted(arg_json);
                    _dprintf(stderr, "DEBUG_PRINT: unmarshal_value: 'call' %s, arg[%d] JSON: %s\n", call_name, arg_idx, arg_json_str);

                    // ENUM VALIDATION for function call arguments
                    const FunctionArg* func_arg_def_for_enum = NULL; // For enum check
                    if (fd && cJSON_IsString(arg_json)) {
                        func_arg_def_for_enum = api_spec_get_function_arg_by_index(fd, arg_idx);
                        if (func_arg_def_for_enum && func_arg_def_for_enum->type) {
                            const cJSON* specific_enum_def_json = get_enum_definition_if_type_is_enum(ctx->api_spec, func_arg_def_for_enum->type);
                            if (specific_enum_def_json) { // Expected type is an enum
                                const char* original_string_value = arg_json->valuestring;
                                if (!is_enum_member_valid_for_definition(specific_enum_def_json, original_string_value)) {
                                    fprintf(stderr, "Warning: Enum value '%s' is not a valid member of expected enum type '%s' for argument %d of function '%s'.\n",
                                            original_string_value, func_arg_def_for_enum->type, arg_idx, call_name);
                                }
                            }
                        }
                    }

                    IRExpr* arg_expr = unmarshal_value(ctx, arg_json, ui_context);

                    // TYPE MISMATCH CHECK for function call arguments
                    if (fd && arg_expr) {
                        const FunctionArg* func_arg_def_for_type = api_spec_get_function_arg_by_index(fd, arg_idx);
                        if (func_arg_def_for_type && func_arg_def_for_type->type) {
                            const char* expected_type = func_arg_def_for_type->type;
                            const char* actual_type = ir_expr_get_type(arg_expr, ctx->api_spec, ctx->registry);

                            if (actual_type && strncmp(actual_type, "unknown", 7) != 0 && strcmp(actual_type, "void*") != 0 && strcmp(expected_type, "void*") != 0) {
                                // Basic pointer compatibility: if expected is T* and actual is U* (and T != U), it's a mismatch unless one is void* (handled)
                                // More sophisticated checks (e.g. int vs float, or specific pointer compat) can be added.
                                bool is_expected_ptr = strchr(expected_type, '*') != NULL;
                                bool is_actual_ptr = strchr(actual_type, '*') != NULL;

                                if (is_expected_ptr && actual_type && strcmp(actual_type, "int") == 0 && arg_expr->type == IR_EXPR_LITERAL && strcmp(((IRExprLiteral*)arg_expr)->value, "0") == 0) {
                                    // Allow literal 0 for NULL pointer assignment, effectively "void*" for this case.
                                } else if (strcmp(expected_type, actual_type) != 0) {
                                    // Allow lv_obj_t* where lv_xxx_t* (widget type) is actual, or vice versa (common base type)
                                    bool allow_widget_base_mismatch = false;
                                    if (is_expected_ptr && is_actual_ptr) {
                                        if ((strstr(expected_type, "lv_obj_t") && strstr(actual_type, "_t*")) ||
                                            (strstr(actual_type, "lv_obj_t") && strstr(expected_type, "_t*"))) {
                                            allow_widget_base_mismatch = true;
                                        }
                                    }
                                    if (!allow_widget_base_mismatch) {
                                         fprintf(stderr, "Warning: Type mismatch for argument %d of function '%s'. Expected '%s', got '%s'.\n",
                                            arg_idx, call_name, expected_type, actual_type);
                                    }
                                }
                            }
                        }
                    }

                    if(arg_expr) {
                        const char* arg_expr_type_str = "UNKNOWN_EXPR_TYPE";
                        if(arg_expr->type == IR_EXPR_LITERAL) arg_expr_type_str = "IR_EXPR_LITERAL";
                        else if(arg_expr->type == IR_EXPR_VARIABLE) arg_expr_type_str = "IR_EXPR_VARIABLE";
                        else if(arg_expr->type == IR_EXPR_FUNC_CALL) arg_expr_type_str = "IR_EXPR_FUNC_CALL";
                        else if(arg_expr->type == IR_EXPR_ARRAY) arg_expr_type_str = "IR_EXPR_ARRAY";
                        else if(arg_expr->type == IR_EXPR_ADDRESS_OF) arg_expr_type_str = "IR_EXPR_ADDRESS_OF";
                        _dprintf(stderr, "DEBUG_PRINT: unmarshal_value: 'call' %s, arg[%d] unmarshalled to IR type: %s\n", call_name, arg_idx, arg_expr_type_str);
                    } else {
                        _dprintf(stderr, "DEBUG_PRINT: unmarshal_value: 'call' %s, arg[%d] unmarshalled to NULL IR expression\n", call_name, arg_idx);
                    }
                    if(arg_json_str) free(arg_json_str);
                    ir_expr_list_add(&args_list, arg_expr);
                    arg_idx++;
                }
            } else if (args_item != NULL) { // Single argument case (arg_idx = 0)
                 char* arg_json_str = cJSON_PrintUnformatted(args_item);
                 _dprintf(stderr, "DEBUG_PRINT: unmarshal_value: 'call' %s, single arg JSON: %s\n", call_name, arg_json_str);
                 const FunctionDefinition* fd_single_arg = api_spec_find_function(ctx->api_spec, call_name); // fd might be NULL if not found

                // ENUM VALIDATION for single function call argument
                const FunctionArg* func_arg_def_for_enum_single = NULL;
                if (fd_single_arg && cJSON_IsString(args_item)) {
                    func_arg_def_for_enum_single = api_spec_get_function_arg_by_index(fd_single_arg, 0); // arg_idx is 0
                    if (func_arg_def_for_enum_single && func_arg_def_for_enum_single->type) {
                        const cJSON* specific_enum_def_json = get_enum_definition_if_type_is_enum(ctx->api_spec, func_arg_def_for_enum_single->type);
                        if (specific_enum_def_json) { // Expected type is an enum
                            const char* original_string_value = args_item->valuestring;
                            if (!is_enum_member_valid_for_definition(specific_enum_def_json, original_string_value)) {
                                fprintf(stderr, "Warning: Enum value '%s' is not a valid member of expected enum type '%s' for argument 0 of function '%s'.\n",
                                        original_string_value, func_arg_def_for_enum_single->type, call_name);
                            }
                        }
                    }
                }

                 IRExpr* arg_expr = unmarshal_value(ctx, args_item, ui_context);

                // TYPE MISMATCH CHECK for single function call argument
                if (fd_single_arg && arg_expr) {
                    const FunctionArg* func_arg_def_for_type = api_spec_get_function_arg_by_index(fd_single_arg, 0);
                    if (func_arg_def_for_type && func_arg_def_for_type->type) {
                        const char* expected_type = func_arg_def_for_type->type;
                        const char* actual_type = ir_expr_get_type(arg_expr, ctx->api_spec, ctx->registry);

                        if (actual_type && strncmp(actual_type, "unknown", 7) != 0 && strcmp(actual_type, "void*") != 0 && strcmp(expected_type, "void*") != 0) {
                            bool is_expected_ptr = strchr(expected_type, '*') != NULL;
                            // bool is_actual_ptr = strchr(actual_type, '*') != NULL; // Not used in current logic block

                            if (is_expected_ptr && actual_type && strcmp(actual_type, "int") == 0 && arg_expr->type == IR_EXPR_LITERAL && strcmp(((IRExprLiteral*)arg_expr)->value, "0") == 0) {
                                // Allow literal 0 for NULL pointer.
                            } else if (strcmp(expected_type, actual_type) != 0) {
                                bool allow_widget_base_mismatch_single = false;
                                if (is_expected_ptr && strchr(actual_type, '*') != NULL) { // Both pointers
                                     if ((strstr(expected_type, "lv_obj_t") && strstr(actual_type, "_t*")) ||
                                         (strstr(actual_type, "lv_obj_t") && strstr(expected_type, "_t*"))) {
                                        allow_widget_base_mismatch_single = true;
                                    }
                                }
                                if (!allow_widget_base_mismatch_single) {
                                    fprintf(stderr, "Warning: Type mismatch for argument 0 of function '%s'. Expected '%s', got '%s'.\n",
                                        call_name, expected_type, actual_type);
                                }
                            }
                        }
                    }
                }

                 if(arg_expr) {
                     const char* arg_expr_type_str = "UNKNOWN_EXPR_TYPE";
                     if(arg_expr->type == IR_EXPR_LITERAL) arg_expr_type_str = "IR_EXPR_LITERAL";
                     else if(arg_expr->type == IR_EXPR_VARIABLE) arg_expr_type_str = "IR_EXPR_VARIABLE";
                     else if(arg_expr->type == IR_EXPR_FUNC_CALL) arg_expr_type_str = "IR_EXPR_FUNC_CALL";
                     else if(arg_expr->type == IR_EXPR_ARRAY) arg_expr_type_str = "IR_EXPR_ARRAY";
                     else if(arg_expr->type == IR_EXPR_ADDRESS_OF) arg_expr_type_str = "IR_EXPR_ADDRESS_OF";
                     _dprintf(stderr, "DEBUG_PRINT: unmarshal_value: 'call' %s, single arg unmarshalled to IR type: %s\n", call_name, arg_expr_type_str);
                 } else {
                     _dprintf(stderr, "DEBUG_PRINT: unmarshal_value: 'call' %s, single arg unmarshalled to NULL IR expression\n", call_name);
                 }
                 if(arg_json_str) free(arg_json_str);
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
    _dprintf(stderr, "DEBUG: process_properties: START. Target C var: %s, Obj type: %s\n", target_c_var_name, obj_type_for_api_lookup);
    if (!node_json_containing_properties) {
        _dprintf(stderr, "DEBUG: process_properties: node_json_containing_properties is NULL. Returning.\n");
        return;
    }

    cJSON* source_of_props = node_json_containing_properties;
    cJSON* properties_sub_object = cJSON_GetObjectItem(node_json_containing_properties, "properties");

    if (properties_sub_object && cJSON_IsObject(properties_sub_object)) {
        _dprintf(stderr, "DEBUG: process_properties: Found 'properties' sub-object. Iterating over that.\n");
        source_of_props = properties_sub_object;
    } else {
        _dprintf(stderr, "DEBUG: process_properties: No 'properties' sub-object found, or it's not an object. Iterating over the main node.\n");
    }

    // The specific check for "style" is removed as the general "properties" sub-object check should cover necessary cases.
    // If style properties are directly under node_json_containing_properties (e.g. from issue_spec.json for a style object),
    // source_of_props will remain node_json_containing_properties.
    // If style properties are nested under a "properties" sub-object (e.g. in a hypothetical component's style definition),
    // source_of_props will be updated to that sub-object.

    cJSON* prop = NULL;

#ifdef GENERATOR_REGISTRY_TEST_BYPASS_APISPEC_FIND_FOR_STRINGS
    // Test-only path to ensure unmarshal_value is called for potential '!' strings
    // props_json_to_iterate is the component node itself (e.g. the cJSON object for the "label")
    cJSON* properties_obj = cJSON_GetObjectItemCaseSensitive(props_json_to_iterate, "properties");
    if (cJSON_IsObject(properties_obj)) {
        cJSON* actual_prop = NULL;
        _dprintf(stderr, "DEBUG: GENERATOR_REGISTRY_TEST_BYPASS_APISPEC_FIND_FOR_STRINGS is active, found 'properties' object.\n");
        for (actual_prop = properties_obj->child; actual_prop != NULL; actual_prop = actual_prop->next) {
            // actual_prop is now the item like "text": "!Hello World"
            // The value of this item is actual_prop itself if it's a string, or actual_prop->child if it's an object.
            // unmarshal_value expects the cJSON node representing the value.
            if (cJSON_IsString(actual_prop) && actual_prop->valuestring && actual_prop->valuestring[0] == '!') {
                 _dprintf(stderr, "DEBUG: Test bypass: Processing property '%s' with value '%s'\n", actual_prop->string, actual_prop->valuestring);
                IRExpr* temp_expr = unmarshal_value(ctx, actual_prop, ui_context);
                if (temp_expr) {
                    ir_free((IRNode*)temp_expr);
                }
            }
            // This else-if is for cases like "prop_name": { "value": "!string_val" }
            // For the current test JSON, this branch won't be hit as properties are direct strings.
            else if (cJSON_IsObject(actual_prop)) {
                cJSON* val_item = cJSON_GetObjectItem(actual_prop, "value");
                 if (cJSON_IsString(val_item) && val_item->valuestring && val_item->valuestring[0] == '!') {
                    _dprintf(stderr, "DEBUG: Test bypass: Found object property '%s' with value field '%s'\n", actual_prop->string, val_item->valuestring);
                    IRExpr* temp_expr = unmarshal_value(ctx, val_item, ui_context);
                    if (temp_expr) {
                        ir_free((IRNode*)temp_expr);
                    }
                }
            }
        }
    }
#endif

    for (prop = source_of_props->child; prop != NULL; prop = prop->next) {
    //for (prop = props_json_to_iterate->child; prop != NULL; prop = prop->next) {

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
                IRExpr* style_expr = unmarshal_value(ctx, prop, ui_context);
                if (style_expr) {
                    IRExprNode* args_list = ir_new_expr_node(ir_new_variable(target_c_var_name));
                    ir_expr_list_add(&args_list, style_expr);
                    ir_expr_list_add(&args_list, ir_new_literal("0"));

                    IRStmt* call_stmt = ir_new_func_call_stmt("lv_obj_add_style", args_list);
                    ir_block_add_stmt(current_block, call_stmt);
                    _dprintf(stderr, "INFO: Added lv_obj_add_style for @style property '%s' on var '%s'\n", prop->valuestring, target_c_var_name);
                    continue;
                } else {
                    _dprintf(stderr, "Warning: Failed to unmarshal style reference '%s' for var '%s'.\n", prop->valuestring, target_c_var_name);
                }
            } else {
                 _dprintf(stderr, "Warning: 'style' property for var '%s' is not a valid @-prefixed string reference. Value: %s\n", target_c_var_name, prop ? cJSON_PrintUnformatted(prop): "NULL");
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
                prop_def = api_spec_find_property(ctx->api_spec, "obj", prop_name);
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
            _dprintf(stderr, "Info: Setter for '%s' on type '%s' constructed as '%s'. API spec should provide this.\n", prop_name, obj_type_for_api_lookup, actual_setter_name_const);
        }

        IRExprNode* args_list = NULL; // Initialize earlier

        // --- NEW ARGUMENT HANDLING LOGIC ---
        if (prop_def->func_args != NULL) {
            _dprintf(stderr, "DEBUG: Property '%s' has func_args. Using signature-based arg generation.\n", prop_name);
            FunctionArg* current_func_arg = prop_def->func_args;

            // 1. Add target object as the first argument if the function expects it
            if (current_func_arg && strstr(current_func_arg->type, "_t*") != NULL) { // Heuristic: is it an LVGL object pointer?
                ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
                current_func_arg = current_func_arg->next; // Move to next expected arg from JSON
            } else {
                // This case implies a global function that doesn't take the target_c_var_name as its first param.
                // Or, the first param is the target_c_var_name but func_args didn't list it (less likely if populated correctly).
                // For now, if it's not clearly an object, we assume the JSON must provide all args.
                // If setter is for a style object, target_c_var_name is still the style object.
                if (strcmp(obj_type_for_api_lookup, "style") == 0 && current_func_arg && strstr(current_func_arg->type, "lv_style_t*") != NULL) {
                    ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
                    current_func_arg = current_func_arg->next;
                }
                // If it's a global function not operating on target_c_var_name, args_list starts empty for JSON values.
            }

            // Unmarshal values from JSON for the remaining arguments
            if (cJSON_IsArray(prop)) { // Case: "prop_name": [val1, val2, ...]
                cJSON* val_item_json;
                cJSON_ArrayForEach(val_item_json, prop) {
                    if (!current_func_arg) {
                        fprintf(stderr, "Warning: Too many values in JSON array for function %s, property %s. Ignoring extra.\n", actual_setter_name_const, prop_name);
                        break;
                    }
                    // ENUM VALIDATION for func_args array item
                    if (current_func_arg && current_func_arg->type && cJSON_IsString(val_item_json)) {
                        const cJSON* specific_enum_def_json = get_enum_definition_if_type_is_enum(ctx->api_spec, current_func_arg->type);
                        if (specific_enum_def_json) {
                            const char* original_string_value = val_item_json->valuestring;
                            if (!is_enum_member_valid_for_definition(specific_enum_def_json, original_string_value)) {
                                fprintf(stderr, "Warning: Enum value '%s' is not a valid member of expected enum type '%s' for property '%s' (array argument).\n",
                                        original_string_value, current_func_arg->type, prop_name);
                            }
                        }
                    }
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, val_item_json, ui_context));
                    current_func_arg = current_func_arg->next;
                }
            } else if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value")) { // Case: "prop_name": {"value": X, "part": Y, "state": Z}
                cJSON* value_json = cJSON_GetObjectItem(prop, "value");
                cJSON* part_json = cJSON_GetObjectItem(prop, "part");
                cJSON* state_json = cJSON_GetObjectItem(prop, "state");

                if (value_json && current_func_arg) {
                    // ENUM VALIDATION for func_args object value field
                    if (current_func_arg->type && cJSON_IsString(value_json)) {
                        const cJSON* specific_enum_def_json = get_enum_definition_if_type_is_enum(ctx->api_spec, current_func_arg->type);
                        if (specific_enum_def_json) {
                            const char* original_string_value = value_json->valuestring;
                            if (!is_enum_member_valid_for_definition(specific_enum_def_json, original_string_value)) {
                                fprintf(stderr, "Warning: Enum value '%s' is not a valid member of expected enum type '%s' for property '%s' (object value field).\n",
                                        original_string_value, current_func_arg->type, prop_name);
                            }
                        }
                    }
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, value_json, ui_context));
                    current_func_arg = current_func_arg->next;
                }
                if (part_json && current_func_arg) {
                    // ENUM VALIDATION for func_args object part field
                    if (current_func_arg->type && cJSON_IsString(part_json)) {
                        const cJSON* specific_enum_def_json = get_enum_definition_if_type_is_enum(ctx->api_spec, current_func_arg->type);
                        if (specific_enum_def_json) {
                            const char* original_string_value = part_json->valuestring;
                            if (!is_enum_member_valid_for_definition(specific_enum_def_json, original_string_value)) {
                                fprintf(stderr, "Warning: Enum value '%s' is not a valid member of expected enum type '%s' for property '%s' (object part field).\n",
                                        original_string_value, current_func_arg->type, prop_name);
                            }
                        }
                    }
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, part_json, ui_context));
                    current_func_arg = current_func_arg->next;
                } else if (!part_json && current_func_arg && prop_def->style_part_default &&
                           (prop_def->num_style_args == 2 || prop_def->num_style_args == -1)) {
                     _dprintf(stderr, "DEBUG: Potentially using default part for %s with func_args.\n", prop_name);
                }

                if (state_json && current_func_arg) {
                    // ENUM VALIDATION for func_args object state field
                    if (current_func_arg->type && cJSON_IsString(state_json)) {
                        const cJSON* specific_enum_def_json = get_enum_definition_if_type_is_enum(ctx->api_spec, current_func_arg->type);
                        if (specific_enum_def_json) {
                            const char* original_string_value = state_json->valuestring;
                            if (!is_enum_member_valid_for_definition(specific_enum_def_json, original_string_value)) {
                                fprintf(stderr, "Warning: Enum value '%s' is not a valid member of expected enum type '%s' for property '%s' (object state field).\n",
                                        original_string_value, current_func_arg->type, prop_name);
                            }
                        }
                    }
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, state_json, ui_context));
                    current_func_arg = current_func_arg->next;
                } else if (!state_json && current_func_arg && prop_def->style_state_default &&
                           prop_def->num_style_args > 0) {
                     _dprintf(stderr, "DEBUG: Potentially using default state for %s with func_args.\n", prop_name);
                }
            } else { // Case: "prop_name": "simple_value" or other direct value
                if (current_func_arg) { // If there's an expected argument slot
                    // ENUM VALIDATION for func_args direct value
                    if (current_func_arg->type && cJSON_IsString(prop)) {
                        const cJSON* specific_enum_def_json = get_enum_definition_if_type_is_enum(ctx->api_spec, current_func_arg->type);
                        if (specific_enum_def_json) {
                            const char* original_string_value = prop->valuestring;
                            if (!is_enum_member_valid_for_definition(specific_enum_def_json, original_string_value)) {
                                fprintf(stderr, "Warning: Enum value '%s' is not a valid member of expected enum type '%s' for property '%s' (direct value).\n",
                                        original_string_value, current_func_arg->type, prop_name);
                            }
                        }
                    }
                    IRExpr* val_expr_func_arg = unmarshal_value(ctx, prop, ui_context);
                    ir_expr_list_add(&args_list, val_expr_func_arg);
                    current_func_arg = current_func_arg->next;
                } else if (!args_list && strcmp(obj_type_for_api_lookup, "style") != 0) {
                     IRExpr* val_expr_func_arg = unmarshal_value(ctx, prop, ui_context);
                     ir_expr_list_add(&args_list, val_expr_func_arg);
                }
            }

            if (current_func_arg != NULL) {
                _dprintf(stderr, "Warning: Not all arguments for function %s (prop %s) were provided by the JSON value.\n", actual_setter_name_const, prop_name);
            }

        } else {
            // --- EXISTING ARGUMENT HANDLING LOGIC (when prop_def->func_args is NULL) ---
            ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name)); // First arg is always the target object

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
                val_expr = unmarshal_value(ctx, value_to_unmarshal, ui_context);

                // ENUM VALIDATION for properties without func_args (simple setters)
                if (prop_def && prop_def->c_type && cJSON_IsString(value_to_unmarshal)) { // Enum validation only if original JSON was a string
                    const cJSON* specific_enum_def_json = get_enum_definition_if_type_is_enum(ctx->api_spec, prop_def->c_type);
                    if (specific_enum_def_json) { // It is an expected enum type
                        const char* original_string_value = value_to_unmarshal->valuestring;
                        if (!is_enum_member_valid_for_definition(specific_enum_def_json, original_string_value)) {
                            fprintf(stderr, "Warning: Enum value '%s' is not a valid member of enum type '%s' for property '%s'.\n",
                                    original_string_value, prop_def->c_type, prop_name);
                        }
                    }
                }
                // GENERAL TYPE MISMATCH CHECK for properties without func_args
                if (prop_def && prop_def->c_type && val_expr) {
                    const char* expected_prop_c_type = prop_def->c_type;
                    const char* actual_value_type = ir_expr_get_type(val_expr, ctx->api_spec, ctx->registry);

                    if (actual_value_type && strncmp(actual_value_type, "unknown", 7) != 0 &&
                        strcmp(actual_value_type, "void*") != 0 && strcmp(expected_prop_c_type, "void*") != 0 &&
                        strcmp(actual_value_type, "NULL_TYPE") != 0 /* NULL_TYPE from ir_expr_get_type can be compatible with pointers */ ) {

                        bool is_expected_ptr = strchr(expected_prop_c_type, '*') != NULL;
                        bool is_actual_ptr = strchr(actual_value_type, '*') != NULL;
                        bool types_compatible = false;

                        if (strcmp(expected_prop_c_type, actual_value_type) == 0) {
                            types_compatible = true;
                        } else if (is_expected_ptr && strcmp(actual_value_type, "int") == 0 && val_expr->type == IR_EXPR_LITERAL && strcmp(((IRExprLiteral*)val_expr)->value, "0") == 0) {
                            types_compatible = true; // Allow literal 0 for NULL pointer
                        } else if (is_expected_ptr && strcmp(actual_value_type, "void*") == 0) { // Actual is NULL literal effectively
                            types_compatible = true;
                        }
                        // Basic type compatibility (e.g. int vs lv_coord_t which is int32_t)
                        // This can be expanded. For example, if expected is lv_coord_t and actual is int.
                        else if ((strcmp(expected_prop_c_type, "lv_coord_t") == 0 && strcmp(actual_value_type, "int") == 0) ||
                                 (strcmp(expected_prop_c_type, "int") == 0 && strcmp(actual_value_type, "lv_coord_t") == 0) ) { // lv_coord_t is typedef int16_t
                            types_compatible = true;
                        }
                        // Allow lv_obj_t* where lv_xxx_t* (widget type) is actual, or vice versa (common base type)
                        else if (is_expected_ptr && is_actual_ptr) {
                            if ((strstr(expected_prop_c_type, "lv_obj_t") && strstr(actual_value_type, "_t*")) ||
                                (strstr(actual_value_type, "lv_obj_t") && strstr(expected_prop_c_type, "_t*"))) {
                                types_compatible = true;
                            }
                        }

                        if (!types_compatible) {
                            fprintf(stderr, "Warning: Type mismatch for property '%s' on object type '%s' (C var: '%s'). Expected C type '%s', got value type '%s'.\n",
                                    prop_name, obj_type_for_api_lookup, target_c_var_name, expected_prop_c_type, actual_value_type);
                        }
                    }
                }
            }

            _dprintf(stderr, "DEBUG prop: %s, resolved_prop_def_name: %s, num_style_args: %d, type: %s, setter: %s, obj_setter_prefix: %s\n",
                    prop_name, prop_def->name, prop_def->num_style_args, obj_type_for_api_lookup, prop_def->setter ? prop_def->setter : "NULL", prop_def->obj_setter_prefix ? prop_def->obj_setter_prefix : "NULL");

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
                fprintf(stderr, "Warning: Unhandled num_style_args (%d) for property '%s' on type '%s' (no func_args). Adding value only after target.\n",
                        prop_def->num_style_args, prop_name, obj_type_for_api_lookup);
                ir_expr_list_add(&args_list, val_expr);
            }
        } // End of if/else for prop_def->func_args

        IRStmt* call_stmt = ir_new_func_call_stmt(actual_setter_name_const, args_list);
        ir_block_add_stmt(current_block, call_stmt);

        if (actual_setter_name_allocated) {
            free(actual_setter_name_allocated);
        }
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
    const char* type_str = NULL; // MODIFIED: Moved declaration earlier
    const WidgetDefinition* widget_def = NULL; // MODIFIED: Moved declaration earlier

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
                _dprintf(stderr, "DEBUG: Registered ID '%s' to C-var '%s'\n", id_key_for_registry, c_var_name_for_node);
            } else if (c_name_source[0] != '@') {
                registry_add_generated_var(ctx->registry, c_name_source, c_var_name_for_node);
                 _dprintf(stderr, "DEBUG: Registered Name '%s' to C-var '%s'\n", c_name_source, c_var_name_for_node);
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
        is_with_assignment_node = true; // Assume true initially
        cJSON* item = NULL;
        for (item = node_json->child; item != NULL; item = item->next) {
            const char* key = item->string;
            if (strcmp(key, "type") != 0 &&
                strcmp(key, "id") != 0 &&
                strcmp(key, "named") != 0 &&
                strcmp(key, "with") != 0 &&
                strcmp(key, "context") != 0 &&
                strncmp(key, "//", 2) != 0) {
                is_with_assignment_node = false; // Found a non-allowed key
                break;
            }
        }
    }

    // MODIFICATION START: Logic for adding pointer to registry
    // This should be placed after the object/widget is successfully created and c_var_name_for_node is known.
    // The exact placement will be after the allocation blocks.
    // We will add a flag or check later to ensure it's only added if allocation was successful.
    // For now, this is a placeholder for the logic structure.
    bool object_successfully_created = false; // This flag will be set to true after successful allocation.
    // --- END OF PLACEHOLDER ---

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
        // Other properties or children are typically not processed for assignment nodes.
    } else if (forced_c_var_name) {
        // This path is for when process_node_internal is called to operate on an existing object
        // (e.g., from process_single_with_block for a "do" block).
        // c_var_name_for_node is already set (it's forced_c_var_name).

        type_str = default_obj_type; // Use the type context passed in (e.g., from with.obj)
        widget_def = api_spec_find_widget(ctx->api_spec, type_str);

        _dprintf(stderr, "DEBUG: process_node_internal (forced_c_var_name path for DO block): C_VAR_NAME: %s, Type for props: %s\n", c_var_name_for_node, type_str ? type_str : "NULL");

        // Process properties and children directly on c_var_name_for_node.
        // No new widget allocation. node_json here is the "do" block content.
        process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, type_str, effective_context);

        cJSON* children_json_in_do = cJSON_GetObjectItem(node_json, "children");
        if (cJSON_IsArray(children_json_in_do)) {
            cJSON* child_node_json_in_do;
            cJSON_ArrayForEach(child_node_json_in_do, children_json_in_do) {
                process_node_internal(ctx, child_node_json_in_do, current_node_ir_block, c_var_name_for_node, "obj", effective_context, NULL);
            }
        }
        // object_successfully_created is false, so no duplicate registry_add_pointer.
        // "id" on a "do" block is non-sensical, process_properties already skips "id".
    } else {
        // Original logic for creating a new widget/object
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
        object_successfully_created = true; // Mark as successful
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
            object_successfully_created = true; // Mark as successful
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
            _dprintf(stderr, "DEBUG: process_node_internal: Processing generic 'obj' or component root. C_VAR_NAME: %s, Type: %s\n", c_var_name_for_node, type_str);
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
                 object_successfully_created = true; // Mark as successful
            }
            process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, actual_create_type, effective_context);
            _dprintf(stderr, "DEBUG: process_node_internal: Finished properties for 'obj'/component root. C_VAR_NAME: %s\n", c_var_name_for_node);

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
            char info_comment[256]; // Changed from warning_comment for clarity
            snprintf(info_comment, sizeof(info_comment), "Info: Type '%s' (var %s) has no specific create/init. Creating as lv_obj_t and applying properties.", type_str, c_var_name_for_node);
            ir_block_add_stmt(current_node_ir_block, ir_new_comment(info_comment));

            IRExpr* parent_var_expr = (parent_c_var && parent_c_var[0] != '\0') ? ir_new_variable(parent_c_var) : NULL;
            // Default to lv_obj_t and lv_obj_create if no specific functions are defined for the type
            const char* c_type_for_alloc = (widget_def && widget_def->c_type && widget_def->c_type[0] != '\0') ? widget_def->c_type : "lv_obj_t";
            // If widget_def->create is NULL/empty, but we are in this path, we default to lv_obj_create.
            // This branch is for types that are not 'obj' but lack their own create/init.
            ir_block_add_stmt(current_node_ir_block,
                              ir_new_widget_allocate_stmt(c_var_name_for_node,
                                                          c_type_for_alloc,
                                                          "lv_obj_create", // Default create
                                                          parent_var_expr));
            object_successfully_created = true; // Mark as successful
            process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, type_str, effective_context);

            // Process children (this part was outside the original 'else' for this specific case, ensuring it always runs if not handled before)
            cJSON* children_json = cJSON_GetObjectItem(node_json, "children");
            if (cJSON_IsArray(children_json)) {
                cJSON* child_node_json;
                cJSON_ArrayForEach(child_node_json, children_json) {
                    process_node_internal(ctx, child_node_json, current_node_ir_block, c_var_name_for_node, "obj", effective_context, NULL);
                }
            }
        }
    }
    // --- MODIFICATION START: Closing brace for conditional allocation ---
    } // End of if(!is_with_assignment_node)
    // --- MODIFICATION END ---

    // MODIFICATION START: Add to pointer registry if object was created and has @id
    if (object_successfully_created && c_var_name_for_node) {
        cJSON* id_item_for_reg = cJSON_GetObjectItem(node_json, "id");
        if (id_item_for_reg && cJSON_IsString(id_item_for_reg) && id_item_for_reg->valuestring && id_item_for_reg->valuestring[0] == '@') {
            const char* json_id_val = id_item_for_reg->valuestring + 1; // Skip '@'

            // Determine the type string for registry_add_pointer
            // Use type_str (JSON type like "button") if available.
            // If widget_def and widget_def->c_type (like "lv_obj_t*") is available, it could also be used,
            // but for now, type_str is simpler as per plan.
            // Ensure type_str is valid (not NULL, not an @component reference itself for this purpose)
            // const char* type_for_registry = type_str; // OLD logic: used JSON type
            // NEW LOGIC: Determine C-type for the registry
            const char* c_type_for_registry_add = NULL;
            const char* effective_type_str_for_c_type_lookup = type_str; // type_str is JSON type e.g. "button"

            if (type_str && type_str[0] == '@') { // If type_str is like "@comp_button" (a component reference)
                // We need the base type of the component for C-type lookup.
                const cJSON* component_root_json = registry_get_component(ctx->registry, type_str + 1);
                if (component_root_json) {
                    cJSON* comp_type_item = cJSON_GetObjectItem(component_root_json, "type");
                    if (comp_type_item && cJSON_IsString(comp_type_item)) {
                        effective_type_str_for_c_type_lookup = comp_type_item->valuestring; // e.g. "button"
                    } else {
                        effective_type_str_for_c_type_lookup = "obj"; // Default if component root has no type
                    }
                } else {
                    effective_type_str_for_c_type_lookup = "obj"; // Fallback if component not found
                }
            }
            // Now effective_type_str_for_c_type_lookup is a non-component type string (e.g. "button", "label", "style", "obj")

            if (widget_def && widget_def->c_type && widget_def->c_type[0] != '\0') {
                // If we have a specific widget_def from parsing this node (not a component ref), its c_type is best.
                // This handles cases where type_str might be "obj" but widget_def is more specific due to context.
                // However, widget_def might be for the component itself if type_str was "@...",
                // so ensure widget_def corresponds to effective_type_str_for_c_type_lookup if different.
                if (strcmp(widget_def->name, effective_type_str_for_c_type_lookup) == 0) {
                    c_type_for_registry_add = widget_def->c_type;
                }
            }

            if (!c_type_for_registry_add) { // Fallback if not directly found from widget_def of current node
                 c_type_for_registry_add = get_c_type_for_object_type_str(ctx->api_spec, effective_type_str_for_c_type_lookup, "lv_obj_t*");
            }


            IRExprNode* args = NULL;
            // Populate the generator's internal registry for type lookups during generation
            // The pointer itself is not important here, just the id-type mapping.
            // Using c_var_name_for_node as a dummy non-NULL pointer.
            registry_add_pointer(ctx->registry, (void*)c_var_name_for_node, json_id_val, c_type_for_registry_add);
            _dprintf(stderr, "DEBUG: [Generator Registry] Added ID '%s' (C-var: %s) with C-Type '%s'\n", json_id_val, c_var_name_for_node, c_type_for_registry_add ? c_type_for_registry_add : "NULL");

            // Generate the IR statement for the C runtime registry_add_pointer call
            IRExprNode* args_runtime = NULL;
            ir_expr_list_add(&args_runtime, ir_new_variable("ui_registry"));
            ir_expr_list_add(&args_runtime, ir_new_variable(c_var_name_for_node));
            ir_expr_list_add(&args_runtime, ir_new_literal_string(json_id_val));
            ir_expr_list_add(&args_runtime, c_type_for_registry_add ? ir_new_literal_string(c_type_for_registry_add) : ir_new_literal("NULL"));

            IRStmt* add_ptr_stmt = ir_new_func_call_stmt("registry_add_pointer", args_runtime);
            ir_block_add_stmt(current_node_ir_block, add_ptr_stmt);
            _dprintf(stderr, "DEBUG: [IR Generation] Queued C runtime registry_add_pointer for ID '%s' (C-var: %s, C-Type for Reg: %s)\n", json_id_val, c_var_name_for_node, c_type_for_registry_add ? c_type_for_registry_add : "NULL");
        }
    }
    // MODIFICATION END

    // For regular nodes (not 'is_with_assignment_node' and not 'forced_c_var_name'),
    // "with" blocks are processed here, after the main object and its properties/children.
    // This should iterate if multiple "with" keys are present at the top level of a regular node.
    // Note: This assumes "with" is not handled by process_properties for the main node object itself.
    // If process_properties were to handle "with" for the main node, this block would be redundant.
    // Given the history of reverts, this explicit loop ensures "with" keys on regular nodes are processed.
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
    char* with_node_str = cJSON_PrintUnformatted(with_node);
    _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: START with_node: %s\n", with_node_str);
    if(with_node_str) free(with_node_str);

    cJSON* obj_json = cJSON_GetObjectItem(with_node, "obj");
    char* obj_json_str = obj_json ? cJSON_PrintUnformatted(obj_json) : strdup("NULL");
    _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: obj_json from with_node: %s\n", obj_json_str);
    if(obj_json && obj_json_str) free(obj_json_str); else if (!obj_json) free(obj_json_str);

    if (!obj_json) { // This check was already here and is fine
        fprintf(stderr, "Error: 'with' block missing 'obj' key (when processing for target: %s).\n", explicit_target_var_name ? explicit_target_var_name : "temp_var");
        return;
    }

    IRExpr* obj_expr = unmarshal_value(ctx, obj_json, ui_context);
    if (obj_expr) {
        const char* obj_expr_type_str = "UNKNOWN_EXPR_TYPE";
        if(obj_expr->type == IR_EXPR_LITERAL) obj_expr_type_str = "IR_EXPR_LITERAL";
        else if(obj_expr->type == IR_EXPR_VARIABLE) obj_expr_type_str = "IR_EXPR_VARIABLE";
        else if(obj_expr->type == IR_EXPR_FUNC_CALL) obj_expr_type_str = "IR_EXPR_FUNC_CALL";
        _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: obj_expr unmarshalled to IR type: %s\n", obj_expr_type_str);
        if (obj_expr->type == IR_EXPR_FUNC_CALL) {
            IRExprFuncCall* fc = (IRExprFuncCall*)obj_expr;
            _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: obj_expr is func_call: %s\n", fc->func_name);
        }
    } else {
        _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: obj_expr unmarshalled to NULL IR expression!\n");
    }
    if (!obj_expr) {
        fprintf(stderr, "Error: Failed to unmarshal 'obj' in 'with' block (when processing for target: %s).\n", explicit_target_var_name ? explicit_target_var_name : "temp_var");
        return;
    }

    const char* target_c_var_name = NULL;
    char* generated_var_name_to_free = NULL;
    const char* obj_type_for_props = "obj";     // Default
    const char* temp_var_c_type = "lv_obj_t*"; // Default C type for the variable being declared/assigned

    // Determine obj_type_for_props and temp_var_c_type based on obj_expr
    // This logic needs to run before deciding on target_c_var_name generation for the NULL explicit_target_var_name case.
    if (obj_expr->type == IR_EXPR_VARIABLE) {
        const char* var_name = ((IRExprVariable*)obj_expr)->name;
        if (strstr(var_name, "style") != NULL || strncmp(var_name, "s_", 2) == 0) {
            obj_type_for_props = "style";
            // temp_var_c_type should be the type of the variable itself.
            // If it's a style object (not pointer), it's lv_style_t. If pointer, lv_style_t*.
            // Assuming variables holding styles are often direct objects or need to be if used with & later.
            // For simplicity, if we are assigning this var to another, its type should match.
            // The ir_expr_get_type would return lv_style_t for s_1.
            // If obj_expr is s_1, then temp_var_c_type = "lv_style_t".
            temp_var_c_type = get_c_type_for_object_type_str(ctx->api_spec, "style", "lv_style_t");
        } else {
            obj_type_for_props = "obj"; // Default for other vars
            temp_var_c_type = "lv_obj_t*"; // Assume other vars are object pointers
        }
         _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: Variable '%s' -> obj_type_for_props '%s', temp_var_c_type '%s'.\n", var_name, obj_type_for_props, temp_var_c_type);

    } else if (obj_expr->type == IR_EXPR_FUNC_CALL) {
        IRExprFuncCall* func_call_expr = (IRExprFuncCall*)obj_expr;
        const char* func_name = func_call_expr->func_name;

        if (strcmp(func_name, "registry_get_pointer") == 0) {
            temp_var_c_type = "void*"; // registry_get_pointer returns void*
            obj_type_for_props = "obj"; // Default if ID not found or no type in registry

            if (func_call_expr->args && func_call_expr->args->expr) { // Check ->args instead of ->args_head
                IRExpr* first_arg_expr = func_call_expr->args->expr;
                // The ID for registry_get_pointer is passed as a literal string (e.g. "my_id", not "@my_id" from JSON source)
                // ir_new_literal_string ensures it's quoted.
                if (first_arg_expr->type == IR_EXPR_LITERAL && ((IRExprLiteral*)first_arg_expr)->value && ((IRExprLiteral*)first_arg_expr)->value[0] == '"') {
                    const char* quoted_id_val = ((IRExprLiteral*)first_arg_expr)->value;
                    char* id_for_registry = NULL;
                    if (quoted_id_val) {
                        size_t len = strlen(quoted_id_val);
                        if (len >= 2) { // Remove quotes
                            id_for_registry = strndup(quoted_id_val + 1, len - 2);
                        }
                    }

                    if (id_for_registry && id_for_registry[0] != '\0') {
                        const char* c_type_from_registry = registry_get_type_by_id(ctx->registry, id_for_registry);
                        if (c_type_from_registry) {
                            obj_type_for_props = get_object_type_from_c_type(ctx->api_spec, c_type_from_registry, "obj");
                            _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: registry_get_pointer ID '%s' -> C-Type from Reg '%s', Inferred obj_type_for_props '%s'\n", id_for_registry, c_type_from_registry, obj_type_for_props);
                        } else {
                            fprintf(stderr, "Warning: Could not determine C type from registry for ID '%s' used in registry_get_pointer. Defaulting obj_type_for_props to 'obj'.\n", id_for_registry);
                        }
                    } else {
                         fprintf(stderr, "Warning: ID argument for registry_get_pointer is NULL or empty after unquoting. Defaulting obj_type_for_props.\n");
                    }
                    if (id_for_registry) free(id_for_registry);
                } else {
                     fprintf(stderr, "Warning: First argument to registry_get_pointer is not a string literal. Cannot determine ID for type lookup.\n");
                }
            } else {
                 fprintf(stderr, "Warning: registry_get_pointer called with no arguments. Cannot determine ID for type lookup.\n");
            }
        } else { // Other function calls
            const char* c_return_type_str = api_spec_get_function_return_type(ctx->api_spec, func_name);
            if (c_return_type_str && c_return_type_str[0] != '\0') {
                temp_var_c_type = c_return_type_str;
            } else {
                // temp_var_c_type keeps its default "lv_obj_t*" or "void*" if changed by prior logic
                fprintf(stderr, "Warning: Could not determine return type for function '%s'. Defaulting to '%s'.\n", func_name, temp_var_c_type);
            }
            obj_type_for_props = get_object_type_from_c_type(ctx->api_spec, temp_var_c_type, "obj");
             _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: Func call '%s' -> C-type '%s', obj_type_for_props '%s'\n", func_name, temp_var_c_type, obj_type_for_props);
        }
    } else if (obj_expr->type == IR_EXPR_ADDRESS_OF) {
        IRExprAddressOf* addr_of_expr = (IRExprAddressOf*)obj_expr;
        if (addr_of_expr->expr && addr_of_expr->expr->type == IR_EXPR_VARIABLE) {
            const char* addressed_var_name = ((IRExprVariable*)addr_of_expr->expr)->name;
            // Heuristic: if variable name contains "style" or starts with "s_"
            if (strstr(addressed_var_name, "style") != NULL || strncmp(addressed_var_name, "s_", 2) == 0) {
                obj_type_for_props = "style"; // Properties will be style properties
                temp_var_c_type = "lv_style_t*"; // The expression &style_var is of type lv_style_t*
            } else {
                // For &other_var, assume it's &lv_obj_t*, so type is lv_obj_t**
                // This is less common for LVGL setters.
                // obj_type_for_props might still be "obj" if we expect to operate on it as an object.
                obj_type_for_props = "obj";
                temp_var_c_type = "lv_obj_t**"; // Defaulting to this, but might need refinement
                _dprintf(stderr, "Warning: Taking address of non-style variable '%s', resulting C type '%s'. Property application might be unexpected.\n", addressed_var_name, temp_var_c_type);
            }
        } else {
            // Address of non-variable expression
            const char* inner_actual_type = ir_expr_get_type(addr_of_expr->expr, ctx->api_spec, ctx->registry);
            char pointer_type_buffer[128];
            snprintf(pointer_type_buffer, sizeof(pointer_type_buffer), "%s*", inner_actual_type);
            temp_var_c_type = strdup(pointer_type_buffer); // Leaks, for now. TODO: manage buffer.
            obj_type_for_props = get_object_type_from_c_type(ctx->api_spec, temp_var_c_type, "obj");
             _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: AddressOf non-variable, inner type '%s' -> C-type '%s', obj_type_for_props '%s'\n", inner_actual_type, temp_var_c_type, obj_type_for_props);
        }
    } else if (obj_expr->type == IR_EXPR_LITERAL) {
        // obj_type_for_props remains "obj" (default), temp_var_c_type remains "lv_obj_t*" (default) or "void*" (e.g. for NULL)
        // No change needed here as defaults are already set.
    }


    // Now, determine target_c_var_name
    if (explicit_target_var_name) {
        target_c_var_name = explicit_target_var_name;
        // We will always create a new variable declaration for explicit_target_var_name
        // to assign the result of obj_expr.
        // Example: lv_obj_t* my_named_var = lv_obj_get_child(...);
        // Or: lv_obj_t* my_named_var = existing_label; (alias)
        _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: About to create var_decl for '%s' with type '%s' from obj_expr.\n", target_c_var_name, temp_var_c_type);
        IRStmt* var_decl_stmt = ir_new_var_decl(temp_var_c_type, target_c_var_name, obj_expr);
        ir_block_add_stmt(parent_ir_block, var_decl_stmt);
        // obj_expr is now owned by var_decl_stmt, so it should not be freed separately.
    } else {
        // Original behavior: create a temporary variable or directly use the name if obj_expr is a variable.
        if (obj_expr->type == IR_EXPR_VARIABLE) {
            // This is the tricky case from before. We are NOT creating an alias here,
            // just using the existing variable's name directly for property processing.
            // No new IR declaration is made for target_c_var_name itself.
            target_c_var_name = strdup(((IRExprVariable*)obj_expr)->name);
            generated_var_name_to_free = (char*)target_c_var_name; // So it gets freed
            ir_free((IRNode*)obj_expr); // obj_expr itself is no longer needed as its name is copied
        } else {
            // For func calls, address_of, literals when no explicit target:
            // A new temporary variable is created.
            generated_var_name_to_free = generate_unique_var_name(ctx, obj_type_for_props);
            target_c_var_name = generated_var_name_to_free;
            _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: About to create var_decl for '%s' with type '%s' from obj_expr.\n", target_c_var_name, temp_var_c_type);
            IRStmt* var_decl_stmt = ir_new_var_decl(temp_var_c_type, target_c_var_name, obj_expr);
            ir_block_add_stmt(parent_ir_block, var_decl_stmt);
            // obj_expr is now owned by var_decl_stmt.
        }
    }
    _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: target_c_var_name: '%s', temp_var_c_type: '%s'\n", target_c_var_name ? target_c_var_name : "NULL", temp_var_c_type ? temp_var_c_type : "NULL");

    if (!target_c_var_name) {
         fprintf(stderr, "Error: target_c_var_name could not be determined in 'with' block (explicit: %s).\n", explicit_target_var_name ? explicit_target_var_name : "NULL");
         if (obj_expr && !(explicit_target_var_name && obj_expr->type != IR_EXPR_VARIABLE) && !(generated_var_name_to_free && obj_expr->type != IR_EXPR_VARIABLE) ) {
             // If obj_expr was not consumed by ir_new_var_decl or strdup logic
             // This condition is trying to avoid double free if obj_expr was passed to ir_new_var_decl
             // However, the logic above should ensure obj_expr is either consumed or freed.
             // This is a fallback, if target_c_var_name is NULL, something went wrong before consuming obj_expr.
             // ir_free((IRNode*)obj_expr); // Potentially risky if already consumed. Better to ensure it's always handled.
         }
         return;
    }

    // --- Process "do" block ---
    cJSON* do_json = cJSON_GetObjectItem(with_node, "do");
    if (cJSON_IsObject(do_json)) {
        // Call process_node_internal to handle the "do" block's contents (properties, children, comments).
        // parent_ir_block is where IR statements for the "do" block's operations will be added.
        // Pass NULL for parent_c_var as target_c_var_name is the established parent/target.
        // obj_type_for_props provides the type context for properties within do_json.
        // target_c_var_name is the existing C variable that do_json operates on (passed as forced_c_var_name).
        process_node_internal(ctx, do_json, parent_ir_block, NULL, obj_type_for_props, ui_context, target_c_var_name);
    } else if (do_json && !cJSON_IsNull(do_json)) {
        fprintf(stderr, "Error: 'with' block 'do' key exists but is not an object or null (type: %d) for target C var '%s'. Skipping 'do' processing.\n", do_json->type, target_c_var_name);
    }
    // If do_json is NULL or not present, nothing is done.

    if (generated_var_name_to_free) {
        // This was allocated only if explicit_target_var_name was NULL,
        // and for IR_EXPR_VARIABLE, it was a strdup, for others, it was a generated unique name.
        free(generated_var_name_to_free);
    }
    // Note: obj_expr is managed by ir_new_var_decl if passed to it, or freed if IR_EXPR_VARIABLE and explicit_target_var_name is NULL.
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

// The old body of generate_ir_from_ui_spec is largely moved into
// generate_ir_from_ui_spec_internal_logic and generate_ir_from_ui_spec_with_registry.
// This function now just becomes a wrapper.
IRStmtBlock* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
    return generate_ir_from_ui_spec_with_registry(ui_spec_root, api_spec, NULL);
}

// --- Function definitions for the new structure ---

// Internal logic, assuming GenContext is already set up, including ctx->registry.
// This function does NOT manage the lifecycle of ctx->registry itself but uses it.
// It populates the ctx->current_global_block.
static void generate_ir_from_ui_spec_internal_logic(GenContext* ctx, const cJSON* ui_spec_root) {
    // ctx->api_spec, ctx->registry, ctx->var_counter, ctx->current_global_block
    // are assumed to be initialized by the caller.

    // Pre-process components (uses ctx->registry for registry_add_component)
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

    // Process all top-level nodes in the UI spec
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        cJSON* type_node = cJSON_GetObjectItem(item_json, "type");
        const char* type_str = type_node ? cJSON_GetStringValue(type_node) : NULL;

        if (type_str && strcmp(type_str, "component") == 0) {
            continue;
        }
        process_node(ctx, item_json, ctx->current_global_block, "parent", type_str ? type_str : "obj", NULL);
    }
    // The root_ir_block (ctx->current_global_block) is populated by process_node.
}

IRStmtBlock* generate_ir_from_ui_spec_with_registry(
    const cJSON* ui_spec_root,
    const ApiSpec* api_spec,
    Registry* string_registry_for_gencontext) {

    // Initial checks from the original function
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
    ctx.var_counter = 0; // Initialize var_counter

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

    // Call the internal logic function, which populates ctx.current_global_block (root_ir_block)
    generate_ir_from_ui_spec_internal_logic(&ctx, ui_spec_root);

    if (own_registry) {
        registry_free(ctx.registry);
    }
    return root_ir_block;
}

// --- New function definitions END ---
// Ensure there are no other definitions of generate_ir_from_ui_spec below this point.

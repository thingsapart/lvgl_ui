#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include "api_spec.h" // Include an ApiSpec
#include "generator.h"
#include "ir.h"
#include "registry.h"
#include "utils.h"

// Node for storing ID to type mappings
typedef struct GenIdTypeNode {
    char* id_name;          // ID name (e.g., "my_button" from @my_button)
    char* json_type;        // JSON type (e.g., "button", "label", "style")
    char* c_type;           // C type (e.g., "lv_obj_t*", "lv_style_t*")
    struct GenIdTypeNode* next;
} GenIdTypeNode;

// --- Context for Generation ---
typedef struct {
    Registry* registry;
    const ApiSpec* api_spec; // Store ApiSpec
    int var_counter;
    IRStmtBlock* current_global_block;
    GenIdTypeNode* id_type_map_head; // Head of the ID -> type map
} GenContext;

// --- ID Type Map Helper Functions ---
static void add_to_id_type_map(GenContext* ctx, const char* id_name, const char* json_type_from_spec, const char* c_type_from_spec) {
    if (!ctx || !id_name) return;
    if (!json_type_from_spec && !c_type_from_spec) return; // No type info to store

    // Check if ID already exists
    GenIdTypeNode* current = ctx->id_type_map_head;
    while (current) {
        if (strcmp(current->id_name, id_name) == 0) {
            // Optionally update if new info is more specific, or just return
            // For now, let's prevent duplicates and keep the first entry.
            // Consider warning about re-definition if necessary.
            return;
        }
        current = current->next;
    }

    GenIdTypeNode* new_node = (GenIdTypeNode*)calloc(1, sizeof(GenIdTypeNode));
    if (!new_node) {
        perror("Failed to allocate GenIdTypeNode");
        return;
    }

    new_node->id_name = strdup(id_name);
    if (!new_node->id_name) {
        perror("Failed to strdup id_name for GenIdTypeNode");
        free(new_node);
        return;
    }

    if (json_type_from_spec) {
        new_node->json_type = strdup(json_type_from_spec);
        if (!new_node->json_type) {
            perror("Failed to strdup json_type for GenIdTypeNode");
            free(new_node->id_name);
            free(new_node);
            return;
        }
    } else {
        new_node->json_type = NULL;
    }

    if (c_type_from_spec) {
        new_node->c_type = strdup(c_type_from_spec);
        if (!new_node->c_type) {
            perror("Failed to strdup c_type for GenIdTypeNode");
            if (new_node->json_type) free(new_node->json_type);
            free(new_node->id_name);
            free(new_node);
            return;
        }
    } else {
        new_node->c_type = NULL;
    }

    new_node->next = ctx->id_type_map_head;
    ctx->id_type_map_head = new_node;
    _dprintf(stderr, "DEBUG: Added to ID-Type map: ID='%s', JSONType='%s', CType='%s'\n", id_name, json_type_from_spec ? json_type_from_spec : "N/A", c_type_from_spec ? c_type_from_spec : "N/A");
}

static bool get_from_id_type_map(GenContext* ctx, const char* id_name, const char** out_json_type, const char** out_c_type) {
    if (!ctx || !id_name || !out_json_type || !out_c_type) return false;
    *out_json_type = NULL;
    *out_c_type = NULL;

    GenIdTypeNode* current = ctx->id_type_map_head;
    while (current) {
        if (strcmp(current->id_name, id_name) == 0) {
            *out_json_type = current->json_type;
            *out_c_type = current->c_type;
            return true;
        }
        current = current->next;
    }
    return false;
}

static void free_id_type_map(GenContext* ctx) {
    if (!ctx) return;
    GenIdTypeNode* current = ctx->id_type_map_head;
    while (current) {
        GenIdTypeNode* next = current->next;
        if (current->id_name) free(current->id_name);
        if (current->json_type) free(current->json_type);
        if (current->c_type) free(current->c_type);
        free(current);
        current = next;
    }
    ctx->id_type_map_head = NULL;
}

// Forward declarations
static void process_node_internal(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context, const char* forced_c_var_name);
static void process_node(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context); // Wrapper
static void process_properties(GenContext* ctx, cJSON* props_json, const char* target_c_var_name, IRStmtBlock* current_block, const char* obj_type_for_api_lookup, cJSON* ui_context);
static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context, const char* explicit_target_var_name);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context, const char* expected_c_type);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);
static const char* get_json_type_from_c_type(const char* c_type_str);


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

// Function to check if a string value is a key in any of the enum type definitions.
// If it is, optionally returns the name of the enum type it belongs to.
static bool is_value_in_any_enum(const ApiSpec* spec, const char* value, const char** out_enum_type_name) {
    if (!spec || !spec->enums || !value) {
        return false;
    }
    const cJSON* all_enum_types_json = spec->enums; // Direct reference from ApiSpec
    cJSON* enum_type_definition_json = NULL;
    for (enum_type_definition_json = all_enum_types_json->child; enum_type_definition_json != NULL; enum_type_definition_json = enum_type_definition_json->next) {
        if (cJSON_IsObject(enum_type_definition_json)) {
            if (cJSON_GetObjectItem(enum_type_definition_json, value)) {
                if (out_enum_type_name) {
                    *out_enum_type_name = enum_type_definition_json->string; // The name of the enum type
                }
                return true;
            }
        }
    }
    return false;
}

// Function to check if a string value is a key in a specific enum type definition.
static bool is_value_in_specific_enum(const ApiSpec* spec, const char* enum_type_name, const char* value) {
    if (!spec || !spec->enums || !enum_type_name || !value) {
        return false;
    }
    const cJSON* enum_type_definition_json = cJSON_GetObjectItem(spec->enums, enum_type_name);
    if (cJSON_IsObject(enum_type_definition_json)) {
        return cJSON_GetObjectItem(enum_type_definition_json, value) != NULL;
    }
    return false;
}


static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context, const char* expected_c_type) {
    if (!value) {
        if (expected_c_type) { // Handle NULL value with an expected type
            if (strstr(expected_c_type, "*") != NULL || strcmp(expected_c_type, "void*") == 0 || api_spec_is_enum_type(ctx->api_spec, expected_c_type)) {
                 // Pointers and enums can be NULL/0 conceptually
                return ir_new_literal("NULL");
            } else if (strcmp(expected_c_type, "bool") == 0) {
                return ir_new_literal("false");
            } else if (strcmp(expected_c_type, "int") == 0 || strcmp(expected_c_type, "float") == 0 || strcmp(expected_c_type, "double") == 0 ) { // Add other numeric types as needed
                return ir_new_literal("0");
            } else {
                // Default for other non-pointer, non-primitive types if necessary
                fprintf(stderr, "Warning: unmarshal_value: NULL JSON value encountered for non-pointer, non-enum expected type '%s'. Defaulting to NULL literal.\n", expected_c_type);
                return ir_new_literal("NULL");
            }
        }
        return ir_new_literal("NULL");
    }

    if (cJSON_IsString(value)) {
        const char* s_orig = value->valuestring;
        if (s_orig == NULL) return ir_new_literal("NULL"); // Should not happen if cJSON_IsString is true

        // 1. Handle special prefixes first
        if (s_orig[0] == '$' && s_orig[1] != '\0') { // Context variable
            if (ui_context) {
                cJSON* ctx_val = cJSON_GetObjectItem(ui_context, s_orig + 1);
                if (ctx_val) {
                    return unmarshal_value(ctx, ctx_val, ui_context, expected_c_type); // Pass expected_c_type down
                }
                fprintf(stderr, "Warning: Context variable '%s' not found. Expected C type: %s.\n", s_orig + 1, expected_c_type ? expected_c_type : "any");
                return ir_new_literal("NULL"); // Or handle error as per policy
            }
             fprintf(stderr, "Warning: Attempted to access context variable '%s' with NULL context. Expected C type: %s.\n", s_orig + 1, expected_c_type ? expected_c_type : "any");
             return ir_new_literal("NULL");
        }
        if (s_orig[0] == '@' && s_orig[1] != '\0') { // Registered ID
            const char* id_key_for_lookup = s_orig + 1;
            const char* registered_c_var = registry_get_generated_var(ctx->registry, id_key_for_lookup);

            if (expected_c_type && id_key_for_lookup) {
                const char* actual_json_type_from_map = NULL;
                const char* actual_c_type_from_map = NULL;
                bool found_in_map = get_from_id_type_map(ctx, id_key_for_lookup, &actual_json_type_from_map, &actual_c_type_from_map);

                if (found_in_map) {
                    const char* type_to_check_against_expected = actual_c_type_from_map;
                    if (!type_to_check_against_expected && actual_json_type_from_map) {
                        // Try to resolve JSON type to C type via ApiSpec
                        const WidgetDefinition* wd_for_json_type = api_spec_find_widget(ctx->api_spec, actual_json_type_from_map);
                        if (wd_for_json_type && wd_for_json_type->c_type) {
                            type_to_check_against_expected = wd_for_json_type->c_type;
                        } else { // Fallback for common types if not in widget defs explicitly with c_type
                            if (strcmp(actual_json_type_from_map, "style") == 0) type_to_check_against_expected = "lv_style_t*";
                            else if (strcmp(actual_json_type_from_map, "anim") == 0) type_to_check_against_expected = "lv_anim_t*";
                            // Add other direct json_type to c_type mappings if common
                        }
                    }

                    if (type_to_check_against_expected) {
                        if (strcmp(type_to_check_against_expected, expected_c_type) != 0) {
                            bool compatible = false;
                            // Compatibility: expected lv_obj_t* and actual is lv_obj_t* (even if from a more specific widget JSON type)
                            if ((strcmp(expected_c_type, "lv_obj_t*") == 0 || strcmp(expected_c_type, "const lv_obj_t*") == 0) &&
                                (strcmp(type_to_check_against_expected, "lv_obj_t*") == 0 || strcmp(type_to_check_against_expected, "const lv_obj_t*") == 0)) {
                                compatible = true;
                            } else if (actual_json_type_from_map &&
                                       (strcmp(expected_c_type, "lv_obj_t*") == 0 || strcmp(expected_c_type, "const lv_obj_t*") == 0) &&
                                       strcmp(type_to_check_against_expected, "lv_style_t*") != 0 && // Ensure it's not a style/anim being cast to obj
                                       strcmp(type_to_check_against_expected, "lv_anim_t*") != 0) {
                                // If actual_json_type is a known widget (e.g. "button") and its C type (type_to_check_against_expected)
                                // is something like "lv_obj_t*" (or specific like "lv_btn_t*" which is typedef to lv_obj_t*), consider compatible.
                                const WidgetDefinition* wd_check = api_spec_find_widget(ctx->api_spec, actual_json_type_from_map);
                                if(wd_check && wd_check->c_type && (strcmp(wd_check->c_type, "lv_obj_t*")==0 || strstr(wd_check->c_type, "lv_obj_t *"))) { // Check if its C type is lv_obj_t*
                                     compatible = true;
                                } else if (wd_check && !wd_check->c_type && wd_check->create) { // No explicit C-type but has a create func (likely lv_obj_t*)
                                     compatible = true;
                                }
                            }
                            // Compatibility: expected void*
                            if (strcmp(expected_c_type, "void*") == 0 || strcmp(expected_c_type, "const void*") == 0) {
                                compatible = true;
                            }

                            if (!compatible) {
                                fprintf(stderr, "Warning: Type mismatch for ID '@%s'. Resolved C type: '%s' (from JSON type '%s'). Expected C type: '%s'.\n",
                                        id_key_for_lookup, type_to_check_against_expected, actual_json_type_from_map ? actual_json_type_from_map : "N/A", expected_c_type);
                            }
                        }
                    } else {
                        fprintf(stderr, "Warning: Could not determine C type for ID '@%s' (JSON type '%s') from map/spec to check against expected C type '%s'.\n",
                                id_key_for_lookup, actual_json_type_from_map ? actual_json_type_from_map : "N/A", expected_c_type);
                    }
                } else {
                     fprintf(stderr, "Warning: ID '@%s' not found in ID-type map (may be forward reference or defined outside main spec). Expected C type: '%s'.\n", id_key_for_lookup, expected_c_type);
                }
            }

            if (registered_c_var) {
                // Existing logic for style address-of or variable
                bool is_style_type_by_name = (strstr(id_key_for_lookup, "style") != NULL || strstr(registered_c_var, "style") != NULL || strncmp(registered_c_var, "s_", 2) == 0);
                bool is_expected_style = (expected_c_type && (strcmp(expected_c_type, "lv_style_t*") == 0 || strstr(expected_c_type, "Style") !=0));

                if (is_expected_style || (!expected_c_type && is_style_type_by_name)) {
                     return ir_new_address_of(ir_new_variable(registered_c_var));
                }
                return ir_new_variable(registered_c_var);
            }
            // If not in registry_get_generated_var, it might be a direct C variable name (e.g. screen_1 from a with block)
            // or an error if it was expected to be in the registry.
            // The ID-Type map check above provides warnings, here we proceed with assumption it's a var if not in registry.
            fprintf(stderr, "Info: ID '@%s' not found in generated variable registry, treating as direct variable name. Expected C type: %s.\n", id_key_for_lookup, expected_c_type ? expected_c_type : "any");
            return ir_new_variable(id_key_for_lookup); // Treat s_orig+1 (id_key_for_lookup) as the variable name
        }
        if (s_orig[0] == '#' && s_orig[1] != '\0') { // Color hex
            if (expected_c_type && strcmp(expected_c_type, "lv_color_t") != 0 && strcmp(expected_c_type, "uint32_t") != 0) { // uint32_t for lv_color_to_u32
                fprintf(stderr, "Warning: Color hex value '%s' encountered, but expected C type is '%s'. Generating lv_color_hex anyway.\n", s_orig, expected_c_type);
            }
            long hex_val = strtol(s_orig + 1, NULL, 16); // TODO: error check strtol
            char hex_str_arg[32];
            snprintf(hex_str_arg, sizeof(hex_str_arg), "0x%06lX", hex_val);
            return ir_new_func_call_expr("lv_color_hex", ir_new_expr_node(ir_new_literal(hex_str_arg)));
        }
        if (s_orig[0] == '!' && s_orig[1] != '\0') { // Registered string
             if (expected_c_type && strcmp(expected_c_type, "const char*") != 0 && strcmp(expected_c_type, "char*") != 0) {
                fprintf(stderr, "Warning: Registered string '!' prefix for '%s', but expected C type is '%s'. Proceeding as string.\n", s_orig+1, expected_c_type);
            }
            const char* registered_string = registry_add_str(ctx->registry, s_orig + 1);
            return ir_new_literal_string(registered_string ? registered_string : (s_orig+1)); // Fallback to original if registration fails
        }
        if (strlen(s_orig) > 0 && s_orig[strlen(s_orig)-1] == '%') { // Percentage Check, ensure s_orig is not empty
            char* temp_s = strdup(s_orig);
            if (!temp_s) { perror("Failed to strdup for percentage"); return ir_new_literal_string(s_orig); } // Should not happen
            temp_s[strlen(s_orig)-1] = '\0';
            char* endptr;
            long num_val = strtol(temp_s, &endptr, 10);
            if (*endptr == '\0' && endptr != temp_s) { // Valid integer
                char num_str_arg[32];
                snprintf(num_str_arg, sizeof(num_str_arg), "%ld", num_val);
                free(temp_s);
                return ir_new_func_call_expr("lv_pct", ir_new_expr_node(ir_new_literal(num_str_arg)));
            }
            free(temp_s);
            fprintf(stderr, "Warning: Percentage value '%s' is not a valid number. Treating as literal string. Expected C type: %s\n", s_orig, expected_c_type ? expected_c_type : "any");
        }

        // 2. Handle based on expected_c_type
        if (expected_c_type) {
            if (api_spec_is_enum_type(ctx->api_spec, expected_c_type)) {
                if (is_value_in_specific_enum(ctx->api_spec, expected_c_type, s_orig)) {
                    return ir_new_literal(s_orig);
                } else {
                    fprintf(stderr, "Error: String value '%s' is not a valid member of expected enum type '%s'. Defaulting to string literal.\n", s_orig, expected_c_type);
                    return ir_new_literal_string(s_orig);
                }
            } else {
                const char* actual_enum_type_if_any = NULL;
                if (is_value_in_any_enum(ctx->api_spec, s_orig, &actual_enum_type_if_any)) {
                     fprintf(stderr, "Error: String value '%s' is a member of enum '%s', but expected C type is non-enum '%s'. Using literal '%s'.\n", s_orig, actual_enum_type_if_any, expected_c_type, s_orig);
                     return ir_new_literal(s_orig);
                }
                if (cJSON_GetObjectItem(api_spec_get_constants(ctx->api_spec), s_orig)) {
                    return ir_new_literal(s_orig);
                }
                if (strcmp(expected_c_type, "const char*") == 0 || strcmp(expected_c_type, "char*") == 0) {
                    return ir_new_literal_string(s_orig);
                }
                fprintf(stderr, "Error: String value '%s' encountered for expected C type '%s', which is not an enum or string type. Defaulting to string literal.\n", s_orig, expected_c_type);
                return ir_new_literal_string(s_orig);
            }
        } else {
            if (cJSON_GetObjectItem(api_spec_get_constants(ctx->api_spec), s_orig)) {
                return ir_new_literal(s_orig);
            }
            if (is_value_in_any_enum(ctx->api_spec, s_orig, NULL)) {
                return ir_new_literal(s_orig);
            }
            return ir_new_literal_string(s_orig);
        }

    } else if (cJSON_IsNumber(value)) {
        if (expected_c_type && !(strcmp(expected_c_type, "int") == 0 || strcmp(expected_c_type, "uint32_t") == 0 || strcmp(expected_c_type, "int32_t") == 0 ||
                                  strcmp(expected_c_type, "int16_t") == 0 || strcmp(expected_c_type, "uint16_t") == 0 ||
                                  strcmp(expected_c_type, "int8_t") == 0 || strcmp(expected_c_type, "uint8_t") == 0 ||
                                  strcmp(expected_c_type, "float") == 0 || strcmp(expected_c_type, "double") == 0 ||
                                  api_spec_is_enum_type(ctx->api_spec, expected_c_type) )) {
            fprintf(stderr, "Warning: Number value %f encountered, but expected C type is '%s'. Using literal number.\n", value->valuedouble, expected_c_type);
        }
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", (int)value->valuedouble);
        return ir_new_literal(buf);

    } else if (cJSON_IsBool(value)) {
        if (expected_c_type && strcmp(expected_c_type, "bool") != 0 && strcmp(expected_c_type, "_Bool") != 0) {
            if (!api_spec_is_enum_type(ctx->api_spec, expected_c_type) || (cJSON_IsTrue(value) && !is_value_in_specific_enum(ctx->api_spec, expected_c_type, "1")) || (!cJSON_IsTrue(value) && !is_value_in_specific_enum(ctx->api_spec, expected_c_type, "0")) ) {
                fprintf(stderr, "Warning: Boolean value %s encountered, but expected C type is '%s'. Using 'true'/'false'.\n", cJSON_IsTrue(value) ? "true" : "false", expected_c_type);
            }
        }
        return ir_new_literal(cJSON_IsTrue(value) ? "true" : "false");

    } else if (cJSON_IsNull(value)) {
        if (expected_c_type) {
            if (strstr(expected_c_type, "*") != NULL || strcmp(expected_c_type, "void*") == 0 || api_spec_is_enum_type(ctx->api_spec, expected_c_type)) {
                return ir_new_literal("NULL");
            }
            fprintf(stderr, "Error: JSON null value encountered for non-pointer, non-enum expected C type '%s'. Defaulting to NULL literal.\n", expected_c_type);
            return ir_new_literal("NULL");
        }
        return ir_new_literal("NULL");

    } else if (cJSON_IsArray(value)) {
        if (expected_c_type && !strstr(expected_c_type, "[]") && strcmp(expected_c_type, "cparray") !=0 && strcmp(expected_c_type, "const char**") !=0) {
            fprintf(stderr, "Warning: Array JSON value encountered, but expected C type is non-array '%s'. Processing as generic array.\n", expected_c_type);
        }
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
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, arg_json, ui_context, NULL));
                }
            } else if (args_item != NULL) {
                 ir_expr_list_add(&args_list, unmarshal_value(ctx, args_item, ui_context, NULL));
            }
            return ir_new_func_call_expr(call_name, args_list);
        }
        fprintf(stderr, "Warning: Unhandled JSON object structure in unmarshal_value. Expected C type: %s. Object: %s\n", expected_c_type ? expected_c_type : "any", cJSON_PrintUnformatted(value));
        return ir_new_literal("NULL");
    }

    fprintf(stderr, "Warning: Unhandled JSON type (%d) in unmarshal_value. Expected C type: %s. Defaulting to NULL literal.\n", value->type, expected_c_type ? expected_c_type : "any");
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

    cJSON* prop = NULL;

#ifdef GENERATOR_REGISTRY_TEST_BYPASS_APISPEC_FIND_FOR_STRINGS
    cJSON* props_json_to_iterate = node_json_containing_properties; // Fallback to this if "properties" not found
    cJSON* properties_obj_test = cJSON_GetObjectItemCaseSensitive(props_json_to_iterate, "properties");
    if (cJSON_IsObject(properties_obj_test)) { // Check if "properties" exists and is an object
         _dprintf(stderr, "DEBUG: GENERATOR_REGISTRY_TEST_BYPASS_APISPEC_FIND_FOR_STRINGS is active, found 'properties' object.\n");
        props_json_to_iterate = properties_obj_test; // Iterate over the "properties" object itself
    } else {
         _dprintf(stderr, "DEBUG: GENERATOR_REGISTRY_TEST_BYPASS_APISPEC_FIND_FOR_STRINGS is active, no 'properties' sub-object, iterating original node.\n");
    }

    cJSON* actual_prop_test = NULL;
    for (actual_prop_test = props_json_to_iterate->child; actual_prop_test != NULL; actual_prop_test = actual_prop_test->next) {
        if (cJSON_IsString(actual_prop_test) && actual_prop_test->valuestring && actual_prop_test->valuestring[0] == '!') {
             _dprintf(stderr, "DEBUG: Test bypass: Processing property '%s' with value '%s'\n", actual_prop_test->string, actual_prop_test->valuestring);
            IRExpr* temp_expr = unmarshal_value(ctx, actual_prop_test, ui_context, "const char*");
            if (temp_expr) {
                ir_free((IRNode*)temp_expr);
            }
        }
        else if (cJSON_IsObject(actual_prop_test)) {
            cJSON* val_item_test = cJSON_GetObjectItem(actual_prop_test, "value");
             if (cJSON_IsString(val_item_test) && val_item_test->valuestring && val_item_test->valuestring[0] == '!') {
                _dprintf(stderr, "DEBUG: Test bypass: Found object property '%s' with value field '%s'\n", actual_prop_test->string, val_item_test->valuestring);
                IRExpr* temp_expr = unmarshal_value(ctx, val_item_test, ui_context, "const char*");
                if (temp_expr) {
                    ir_free((IRNode*)temp_expr);
                }
            }
        }
    }
#endif

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
                // For @style references, the expected C type of the reference itself is lv_style_t*
                IRExpr* style_expr = unmarshal_value(ctx, prop, ui_context, "lv_style_t*");
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
        const char* expected_prop_c_type = prop_def->c_type;

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
            _dprintf(stderr, "DEBUG: Property '%s' has func_args. Using signature-based arg generation.\n", prop_name);
            FunctionArg* current_func_arg = prop_def->func_args;
            const char* arg_c_type_from_spec = NULL;

            if (current_func_arg && strstr(current_func_arg->type, "_t*") != NULL) {
                ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
                arg_c_type_from_spec = current_func_arg->type;
                current_func_arg = current_func_arg->next;
            } else if (strcmp(obj_type_for_api_lookup, "style") == 0 && current_func_arg && strstr(current_func_arg->type, "lv_style_t*") != NULL) {
                 ir_expr_list_add(&args_list, ir_new_variable(target_c_var_name));
                 arg_c_type_from_spec = current_func_arg->type;
                 current_func_arg = current_func_arg->next;
            }


            if (cJSON_IsArray(prop)) {
                cJSON* val_item_json;
                cJSON_ArrayForEach(val_item_json, prop) {
                    if (!current_func_arg) {
                        fprintf(stderr, "Warning: Too many values in JSON array for function %s, property %s. Ignoring extra.\n", actual_setter_name_const, prop_name);
                        break;
                    }
                    arg_c_type_from_spec = current_func_arg ? current_func_arg->type : NULL; // Check current_func_arg
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, val_item_json, ui_context, arg_c_type_from_spec));
                    if (current_func_arg) current_func_arg = current_func_arg->next;
                }
            } else if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value")) {
                cJSON* value_json = cJSON_GetObjectItem(prop, "value");
                cJSON* part_json = cJSON_GetObjectItem(prop, "part");
                cJSON* state_json = cJSON_GetObjectItem(prop, "state");

                // Order of processing value, part, state might need to be more flexible
                // depending on how they map to current_func_arg.
                // This assumes a common order: value, then part, then state, if they are func args.
                if (value_json && current_func_arg) {
                    arg_c_type_from_spec = current_func_arg->type; // Type from the current function argument
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, value_json, ui_context, arg_c_type_from_spec));
                    current_func_arg = current_func_arg->next;
                }
                if (part_json && current_func_arg) {
                    arg_c_type_from_spec = current_func_arg->type; // Type from the current function argument (e.g. "lv_part_t")
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, part_json, ui_context, arg_c_type_from_spec));
                    current_func_arg = current_func_arg->next;
                }
                if (state_json && current_func_arg) {
                    arg_c_type_from_spec = current_func_arg->type; // Type from the current function argument (e.g. "lv_state_t")
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, state_json, ui_context, arg_c_type_from_spec));
                    current_func_arg = current_func_arg->next;
                }
            } else { // Simple direct value for a function argument
                if (current_func_arg) {
                    arg_c_type_from_spec = current_func_arg->type; // Type from the current function argument
                    IRExpr* val_expr_func_arg = unmarshal_value(ctx, prop, ui_context, arg_c_type_from_spec);
                    ir_expr_list_add(&args_list, val_expr_func_arg);
                    current_func_arg = current_func_arg->next;
                } else if (!args_list && strcmp(obj_type_for_api_lookup, "style") != 0 ) {
                     // This case implies the property itself is the value for a function that might not take the object as first arg,
                     // or the func_args list was exhausted. Use the property's own c_type.
                     IRExpr* val_expr_func_arg = unmarshal_value(ctx, prop, ui_context, expected_prop_c_type);
                     ir_expr_list_add(&args_list, val_expr_func_arg);
                }
            }
            if (current_func_arg != NULL) {
                 _dprintf(stderr, "Warning: Not all arguments for function %s (prop %s) were provided by the JSON value (expected type for next arg: %s).\n", actual_setter_name_const, prop_name, current_func_arg->type);
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

            // Use prop_def->c_type as the expected_c_type for the main value of the property
            IRExpr* val_expr = unmarshal_value(ctx, value_to_unmarshal, ui_context, prop_def ? prop_def->c_type : NULL);

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
        }

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

        _dprintf(stderr, "DEBUG: process_node_internal (forced_c_var_name path for DO block): C_VAR_NAME: %s, Type for props: %s\n", c_var_name_for_node, type_str ? type_str : "NULL");
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
         // Before returning, try to add to ID map if ID exists, even if type is problematic.
         // This helps in debugging or if the ID is referenced elsewhere expecting a certain structure.
         cJSON* id_item_for_map_error_case = cJSON_GetObjectItem(node_json, "id");
         if (id_item_for_map_error_case && cJSON_IsString(id_item_for_map_error_case) && id_item_for_map_error_case->valuestring && id_item_for_map_error_case->valuestring[0] == '@') {
             const char* id_key_for_map = id_item_for_map_error_case->valuestring + 1;
             // Store whatever type_str we have, even if it's problematic, or NULL if it's really bad.
             add_to_id_type_map(ctx, id_key_for_map, type_str ? type_str : "unknown_type_at_error", NULL);
         }
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
        // After successful creation and property processing, if an ID exists, add type info to map.
        // THIS BLOCK IS FOR widget_def->create CASE
        if (object_successfully_created) {
            cJSON* id_item_for_map = cJSON_GetObjectItem(node_json, "id");
            if (id_item_for_map && cJSON_IsString(id_item_for_map) && id_item_for_map->valuestring && id_item_for_map->valuestring[0] == '@') {
                const char* id_key_for_map = id_item_for_map->valuestring + 1;
                // For 'create' (typically lv_obj_t* returning), widget_def->c_type might be NULL or the specific type like lv_btn_t*.
                // We primarily store the JSON type_str and the most specific C type we can find.
                const char* c_type_to_store = widget_def ? widget_def->c_type : NULL;
                if (!c_type_to_store && widget_def) { // If widget_def exists but c_type is NULL, it's likely lv_obj_t*
                    // Heuristic: if create exists, it's likely an lv_obj_t based widget if c_type isn't more specific
                     if (strcmp(type_str, "style") != 0 && strcmp(type_str, "anim") !=0) { // Styles/anims handled by init_func path
                        c_type_to_store = "lv_obj_t*";
                     }
                }
                add_to_id_type_map(ctx, id_key_for_map, type_str, c_type_to_store);
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
        // After successful creation and property processing, if an ID exists, add type info to map.
        if (object_successfully_created) {
            cJSON* id_item_for_map = cJSON_GetObjectItem(node_json, "id");
            if (id_item_for_map && cJSON_IsString(id_item_for_map) && id_item_for_map->valuestring && id_item_for_map->valuestring[0] == '@') {
                const char* id_key_for_map = id_item_for_map->valuestring + 1;
                const char* c_type_to_store = widget_def ? widget_def->c_type : NULL;
                if (!c_type_to_store) { // Fallback for C type if not directly on widget_def
                    if (strcmp(type_str, "style") == 0) c_type_to_store = "lv_style_t*";
                    else if (strcmp(type_str, "anim") == 0) c_type_to_store = "lv_anim_t*";
                    // Could add more here, or rely on api_spec_find_widget for the JSON type_str later.
                }
                add_to_id_type_map(ctx, id_key_for_map, type_str, c_type_to_store);
            }
        }
    } else if (type_str[0] == '@' && !forced_c_var_name) {
        const cJSON* component_root_json = registry_get_component(ctx->registry, type_str + 1);
        if (!component_root_json) {
            fprintf(stderr, "Error: Component definition '%s' (used as type) not found. Skipping node.\n", type_str);
        } else {
            cJSON* comp_root_type_item = cJSON_GetObjectItem(component_root_json, "type");
            const char* comp_root_type_str = comp_root_type_item ? cJSON_GetStringValue(comp_root_type_item) : "obj";
            process_node_internal(ctx, (cJSON*)component_root_json, current_node_ir_block, parent_c_var, comp_root_type_str, effective_context, c_var_name_for_node);
            process_properties(ctx, node_json, c_var_name_for_node, current_node_ir_block, comp_root_type_str, effective_context);
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
                 object_successfully_created = true;
            } else {
                 ir_block_add_stmt(current_node_ir_block,
                                  ir_new_widget_allocate_stmt(c_var_name_for_node,
                                                              c_type_for_alloc,
                                                              actual_create_func,
                                                              parent_var_expr));
                 object_successfully_created = true;
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
            // After successful creation and property processing, if an ID exists, add type info to map.
            // This covers the "obj" case within the final else.
            if (object_successfully_created) {
                cJSON* id_item_for_map = cJSON_GetObjectItem(node_json, "id");
                if (id_item_for_map && cJSON_IsString(id_item_for_map) && id_item_for_map->valuestring && id_item_for_map->valuestring[0] == '@') {
                    const char* id_key_for_map = id_item_for_map->valuestring + 1;
                    const char* c_type_to_store = (widget_def && widget_def->c_type) ? widget_def->c_type : "lv_obj_t*"; // Default for "obj"
                    add_to_id_type_map(ctx, id_key_for_map, type_str, c_type_to_store);
                }
            }
        } else {
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
            // After successful creation and property processing, if an ID exists, add type info to map.
            // This covers the "default create" path within the final else.
            if (object_successfully_created) {
                cJSON* id_item_for_map = cJSON_GetObjectItem(node_json, "id");
                if (id_item_for_map && cJSON_IsString(id_item_for_map) && id_item_for_map->valuestring && id_item_for_map->valuestring[0] == '@') {
                    const char* id_key_for_map = id_item_for_map->valuestring + 1;
                    const char* c_type_to_store = (widget_def && widget_def->c_type) ? widget_def->c_type : "lv_obj_t*"; // Default for unknown types created as lv_obj_t
                    add_to_id_type_map(ctx, id_key_for_map, type_str, c_type_to_store);
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
                    type_for_registry = comp_widget_def->c_type ? comp_widget_def->c_type : comp_widget_def->name;
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
            _dprintf(stderr, "DEBUG: Added registry_add_pointer for ID '%s' (C-var: %s, Type: %s)\n", json_id_val, c_var_name_for_node, type_for_registry ? type_for_registry: "NULL");
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
    char* with_node_str = cJSON_PrintUnformatted(with_node);
    _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: START with_node: %s\n", with_node_str);
    if(with_node_str) free(with_node_str);

    cJSON* obj_json = cJSON_GetObjectItem(with_node, "obj"); // This is the cJSON for the "obj" field
    char* obj_json_debug_str = obj_json ? cJSON_PrintUnformatted(obj_json) : strdup("NULL");
    _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: obj_json from with_node: %s\n", obj_json_debug_str);
    if(obj_json && obj_json_debug_str) free(obj_json_debug_str); else if (!obj_json) free(obj_json_debug_str);


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
    const char* obj_type_for_props = NULL; // Initialized to NULL
    const char* temp_var_c_type = NULL;    // Initialized to NULL
    bool type_inferred_from_map = false;

    // New logic: Try to infer type from ID map if obj_json is an @id reference
    if (cJSON_IsString(obj_json) && obj_json->valuestring && obj_json->valuestring[0] == '@') {
        const char* id_key = obj_json->valuestring + 1;
        if (id_key[0] != '\0') { // Ensure ID key is not empty
            const char* map_json_type = NULL;
            const char* map_c_type = NULL;
            if (get_from_id_type_map(ctx, id_key, &map_json_type, &map_c_type)) {
                if (map_json_type || map_c_type) { // Found in map and has some type info
                    type_inferred_from_map = true;
                    if (map_c_type) {
                        temp_var_c_type = map_c_type; // Prefer C type from map if available
                    } else if (map_json_type) { // Fallback to resolving JSON type from map
                        const WidgetDefinition* wd = api_spec_find_widget(ctx->api_spec, map_json_type);
                        if (wd && wd->c_type && wd->c_type[0] != '\0') {
                            temp_var_c_type = wd->c_type;
                        } else { // Fallback for common JSON types if no explicit C type in ApiSpec
                            if (strcmp(map_json_type, "style") == 0) temp_var_c_type = "lv_style_t*";
                            else if (strcmp(map_json_type, "anim") == 0) temp_var_c_type = "lv_anim_t*";
                            else temp_var_c_type = "lv_obj_t*"; // Default for other JSON types
                        }
                    }

                    if (map_json_type) {
                        obj_type_for_props = map_json_type; // Prefer JSON type from map for property lookup
                    } else if (map_c_type) { // Fallback to deriving JSON type from C type
                        obj_type_for_props = get_json_type_from_c_type(map_c_type);
                    }
                     _dprintf(stderr, "DEBUG: 'with obj: @%s' - Used map. JSON type for props: %s, C type for var: %s\n",
                             id_key, obj_type_for_props ? obj_type_for_props : "N/A", temp_var_c_type ? temp_var_c_type : "N/A");
                }
            }
        }
    }

    if (!type_inferred_from_map) {
        // ORIGINAL HEURISTIC LOGIC MOVED HERE
        if (obj_expr->type == IR_EXPR_VARIABLE) {
            const char* var_name = ((IRExprVariable*)obj_expr)->name;
            if (strstr(var_name, "style") != NULL || strncmp(var_name, "s_", 2) == 0) {
                obj_type_for_props = "style";
                temp_var_c_type = "lv_style_t*";
            } else if (strstr(var_name, "label") != NULL || strncmp(var_name, "l_", 2) == 0) {
                obj_type_for_props = "label";
                temp_var_c_type = "lv_obj_t*";
            } else {
                obj_type_for_props = "obj"; // Default for variable-based
                temp_var_c_type = "lv_obj_t*";
            }
        } else if (obj_expr->type == IR_EXPR_FUNC_CALL) {
            IRExprFuncCall* func_call_expr = (IRExprFuncCall*)obj_expr;
            const char* func_name = func_call_expr->func_name;
            const char* c_return_type_str = api_spec_get_function_return_type(ctx->api_spec, func_name);
            if (c_return_type_str && c_return_type_str[0] != '\0') {
                temp_var_c_type = c_return_type_str;
            } else {
                // Keep temp_var_c_type as NULL, will be defaulted later
                fprintf(stderr, "Warning: Could not determine C return type for function '%s'. Will use default.\n", func_name);
            }
            obj_type_for_props = get_obj_type_from_c_type(temp_var_c_type);
        } else if (obj_expr->type == IR_EXPR_ADDRESS_OF) {
            IRExprAddressOf* addr_of_expr = (IRExprAddressOf*)obj_expr;
            if (addr_of_expr->expr && addr_of_expr->expr->type == IR_EXPR_VARIABLE) {
                const char* addressed_var_name = ((IRExprVariable*)addr_of_expr->expr)->name;
                if (strstr(addressed_var_name, "style") != NULL || strncmp(addressed_var_name, "s_", 2) == 0) {
                    temp_var_c_type = "lv_style_t*";
                    obj_type_for_props = "style";
                } else {
                    // Keep types as NULL, will be defaulted
                }
            }
        } else if (obj_expr->type == IR_EXPR_LITERAL && strcmp(((IRExprLiteral*)obj_expr)->value, "NULL") == 0) {
            // For "obj": NULL, types will be defaulted later.
        } else {
            _dprintf(stderr, "DEBUG: 'with obj': obj_expr is not a variable, func_call, or address_of. Type for props/var will be default. IR Type: %d\n", obj_expr->type);
        }
    }

    // Ensure final defaults if still NULL after map lookup or heuristics
    if (obj_type_for_props == NULL || obj_type_for_props[0] == '\0') {
        obj_type_for_props = "obj";
         _dprintf(stderr, "DEBUG: 'with obj': obj_type_for_props defaulted to 'obj'.\n");
    }
    if (temp_var_c_type == NULL || temp_var_c_type[0] == '\0') {
        temp_var_c_type = "lv_obj_t*";
        _dprintf(stderr, "DEBUG: 'with obj': temp_var_c_type defaulted to 'lv_obj_t*'.\n");
    }

    // --- Variable declaration/assignment logic ---
    if (explicit_target_var_name) {
        target_c_var_name = explicit_target_var_name;
        _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: About to create var_decl for EXPLICIT '%s' with type '%s' from obj_expr.\n", target_c_var_name, temp_var_c_type);
        IRStmt* var_decl_stmt = ir_new_var_decl(temp_var_c_type, target_c_var_name, obj_expr); // obj_expr consumed here
        ir_block_add_stmt(parent_ir_block, var_decl_stmt);
    } else {
        if (obj_expr->type == IR_EXPR_VARIABLE && !type_inferred_from_map) { // Only use direct var name if not from map (to avoid re-using ID as var name directly)
            target_c_var_name = strdup(((IRExprVariable*)obj_expr)->name);
            generated_var_name_to_free = (char*)target_c_var_name;
            ir_free((IRNode*)obj_expr); // obj_expr (variable) is consumed by strdup'ing its name
        } else { // For function calls, map-inferred types, literals, or address_of, create a new temp var
            generated_var_name_to_free = generate_unique_var_name(ctx, obj_type_for_props);
            target_c_var_name = generated_var_name_to_free;
            _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: About to create var_decl for TEMP '%s' with type '%s' from obj_expr.\n", target_c_var_name, temp_var_c_type);
            IRStmt* var_decl_stmt = ir_new_var_decl(temp_var_c_type, target_c_var_name, obj_expr); // obj_expr consumed here
            ir_block_add_stmt(parent_ir_block, var_decl_stmt);
        }
    }
    _dprintf(stderr, "DEBUG_PRINT: process_single_with_block: FINAL target_c_var_name: '%s', obj_type_for_props: '%s', temp_var_c_type: '%s'\n",
        target_c_var_name ? target_c_var_name : "NULL",
        obj_type_for_props ? obj_type_for_props : "NULL",
        temp_var_c_type ? temp_var_c_type : "NULL");

    if (!target_c_var_name) { // Should ideally not happen if defaults are set
         fprintf(stderr, "Error: target_c_var_name could not be determined in 'with' block (explicit: %s). obj_expr type: %d\n",
            explicit_target_var_name ? explicit_target_var_name : "NULL", obj_expr ? obj_expr->type : -1);
         // If obj_expr was not consumed by ir_new_var_decl or strdup, it needs freeing.
         // This path indicates an issue, but try to free obj_expr if it wasn't passed to ir_new_var_decl.
         // The logic above should ensure obj_expr is always consumed or freed if target_c_var_name is set.
         // If target_c_var_name is NULL here, it means obj_expr was not used.
         if (obj_expr) ir_free((IRNode*)obj_expr);
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
    // obj_expr is consumed by ir_new_var_decl or freed if IR_EXPR_VARIABLE and not explicit_target_var_name
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
        const char* node_type_for_process = type_str ? type_str : "obj";
        process_node(ctx, item_json, ctx->current_global_block, "parent", node_type_for_process, NULL);
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
    ctx.id_type_map_head = NULL; // Initialize the map head

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
        free_id_type_map(&ctx); // Free map
        return NULL;
    }
    ctx.current_global_block = root_ir_block;

    generate_ir_from_ui_spec_internal_logic(&ctx, ui_spec_root);

    if (own_registry) {
        registry_free(ctx.registry);
    }
    free_id_type_map(&ctx); // Free map
    return root_ir_block;
}

static const char* get_json_type_from_c_type(const char* c_type_str) {
    if (!c_type_str) return "obj";
    if (strcmp(c_type_str, "lv_style_t*") == 0) return "style";
    if (strcmp(c_type_str, "lv_anim_t*") == 0) return "anim";
    // Add more specific C type to JSON type mappings if needed
    // For example, if "lv_btn_t*" should map to "button"
    // For now, a generic lv_..._t* that isn't style/anim might be considered "obj"
    if (strncmp(c_type_str, "lv_", 3) == 0 && strstr(c_type_str, "_t*")) return "obj";
    // Default fallback
    return "obj";
}

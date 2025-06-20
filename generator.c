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
static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);


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
        if (s_orig[0] == '!' && s_orig[1] != '\0') {
            return ir_new_literal_string(s_orig + 1);
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
        // If not found in constants or any enum definitions, then treat as a string.
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
            IRExprNode* args_list = NULL;
            if (cJSON_IsArray(args_item)) {
                 cJSON* arg_json;
                 cJSON_ArrayForEach(arg_json, args_item) {
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, arg_json, ui_context));
                }
            } else if (args_item != NULL) {
                 ir_expr_list_add(&args_list, unmarshal_value(ctx, args_item, ui_context));
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

    cJSON* props_json_to_iterate = node_json_containing_properties;

    if (strcmp(obj_type_for_api_lookup, "style") == 0) {
        cJSON* style_props_sub_object = cJSON_GetObjectItem(node_json_containing_properties, "properties");
        if (style_props_sub_object && cJSON_IsObject(style_props_sub_object)) {
            props_json_to_iterate = style_props_sub_object;
        }
    }

    cJSON* prop = NULL;
    for (prop = props_json_to_iterate->child; prop != NULL; prop = prop->next) {
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

        IRExprNode* args_list = ir_new_expr_node(ir_new_variable(target_c_var_name));

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
            fprintf(stderr, "Warning: Unhandled num_style_args (%d) for property '%s' on type '%s'. Adding value only.\n",
                    prop_def->num_style_args, prop_name, obj_type_for_api_lookup);
            ir_expr_list_add(&args_list, val_expr);
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

    cJSON* type_item = cJSON_GetObjectItem(node_json, "type");
    const char* type_str = type_item ? cJSON_GetStringValue(type_item) : default_obj_type;

    if (!type_str || type_str[0] == '\0') {
         fprintf(stderr, "Error: Node missing valid 'type' (or default_obj_type for component root). C var: %s. Skipping node processing.\n", c_var_name_for_node);
         if (allocated_c_var_name) free(allocated_c_var_name);
         if (own_effective_context) cJSON_Delete(effective_context);
         return;
    }

    const WidgetDefinition* widget_def = api_spec_find_widget(ctx->api_spec, type_str);

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
            ir_block_add_stmt(current_node_ir_block, ir_new_comment(warning_comment));
            cJSON* children_json = cJSON_GetObjectItem(node_json, "children");
            if (cJSON_IsArray(children_json)) {
                cJSON* child_node_json;
                cJSON_ArrayForEach(child_node_json, children_json) {
                    process_node_internal(ctx, child_node_json, current_node_ir_block, parent_c_var, "obj", effective_context, NULL);
                }
            }
        }
    }

    bool node_type_can_have_with = (widget_def && (widget_def->create || widget_def->init_func)) ||
                                 (strcmp(type_str, "style") == 0) ||
                                 (strcmp(type_str, "obj") == 0) ||
                                 forced_c_var_name;

    if (c_var_name_for_node && node_type_can_have_with) {
        cJSON* with_prop = cJSON_GetObjectItem(node_json, "with");
        if (with_prop) {
            process_single_with_block(ctx, with_prop, current_node_ir_block, effective_context);
        }
    }

    if (allocated_c_var_name) {
        free(allocated_c_var_name);
    }

    if (own_effective_context) {
        cJSON_Delete(effective_context);
    }
}

static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context) {
    if (!cJSON_IsObject(with_node)) {
        fprintf(stderr, "Error: 'with' block item must be an object.\n");
        return;
    }
    cJSON* obj_json = cJSON_GetObjectItem(with_node, "obj");
    cJSON* do_json = cJSON_GetObjectItem(with_node, "do");
    if (!obj_json) {
        fprintf(stderr, "Error: 'with' block missing 'obj' key.\n");
        return;
    }
    if (!cJSON_IsObject(do_json)) {
        fprintf(stderr, "Error: 'with' block missing 'do' object or 'do' is not an object.\n");
        return;
    }
    IRExpr* obj_expr = unmarshal_value(ctx, obj_json, ui_context);
    if (!obj_expr) {
        fprintf(stderr, "Error: Failed to unmarshal 'obj' in 'with' block.\n");
        return;
    }
    const char* target_c_var_name = NULL;
    char* generated_var_name_to_free = NULL;
    const char* obj_type_for_props = "obj";
    char* temp_var_c_type = "lv_obj_t*";

    if (obj_expr->type == IR_EXPR_VARIABLE) {
        target_c_var_name = strdup(((IRExprVariable*)obj_expr)->name);
        generated_var_name_to_free = (char*)target_c_var_name;

        if (strstr(target_c_var_name, "style") != NULL || strncmp(target_c_var_name, "s_", 2) == 0) {
            obj_type_for_props = "style";
        } else if (strstr(target_c_var_name, "label") != NULL || strncmp(target_c_var_name, "l_", 2) == 0) {
            obj_type_for_props = "label";
        }
        ir_free((IRNode*)obj_expr);
    } else if (obj_expr->type == IR_EXPR_FUNC_CALL || obj_expr->type == IR_EXPR_ADDRESS_OF || obj_expr->type == IR_EXPR_LITERAL) {
        generated_var_name_to_free = generate_unique_var_name(ctx, "with_target");
        target_c_var_name = generated_var_name_to_free;

        if (obj_expr->type == IR_EXPR_ADDRESS_OF) {
            IRExprAddressOf* addr_of = (IRExprAddressOf*)obj_expr;
            if (addr_of->expr && addr_of->expr->type == IR_EXPR_VARIABLE) {
                if (strstr(((IRExprVariable*)addr_of->expr)->name, "style") != NULL || strncmp(((IRExprVariable*)addr_of->expr)->name, "s_", 2) == 0) {
                    temp_var_c_type = "lv_style_t*";
                    obj_type_for_props = "style";
                }
            }
        }
        IRStmt* var_decl_stmt = ir_new_var_decl(temp_var_c_type, target_c_var_name, obj_expr);
        ir_block_add_stmt(parent_ir_block, var_decl_stmt);
    } else {
        fprintf(stderr, "Error: 'obj' expression in 'with' block yielded an unexpected IR type: %d.\n", obj_expr->type);
        ir_free((IRNode*)obj_expr);
        return;
    }

    process_properties(ctx, do_json, target_c_var_name, parent_ir_block, obj_type_for_props, ui_context);

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
    if (!ui_spec_root) {
        fprintf(stderr, "Error: UI Spec root is NULL in generate_ir_from_ui_spec.\n");
        return NULL;
    }
    if (!api_spec) {
        fprintf(stderr, "Error: API Spec is NULL in generate_ir_from_ui_spec.\n");
        return NULL;
    }
    if (!cJSON_IsArray(ui_spec_root)) {
        fprintf(stderr, "Error: UI Spec root must be an array of definitions.\n");
        return NULL;
    }

    GenContext ctx;
    ctx.api_spec = api_spec;
    ctx.registry = registry_create();
    ctx.var_counter = 0;
    if (!ctx.registry) {
        fprintf(stderr, "Error: Failed to create registry in generate_ir_from_ui_spec.\n");
        return NULL;
    }
    IRStmtBlock* root_ir_block = ir_new_block();
    if (!root_ir_block) {
        fprintf(stderr, "Error: Failed to create root IR block.\n");
        registry_free(ctx.registry);
        return NULL;
    }
    ctx.current_global_block = root_ir_block;

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
            registry_add_component(ctx.registry, id_str + 1, component_body_json);
        }
    }

    cJSON_ArrayForEach(item_json, ui_spec_root) {
        cJSON* type_node = cJSON_GetObjectItem(item_json, "type");
        const char* type_str = type_node ? cJSON_GetStringValue(type_node) : NULL;

        if (type_str && strcmp(type_str, "component") == 0) {
            continue;
        }

        process_node(&ctx, item_json, root_ir_block, "parent", type_str ? type_str : "obj", NULL);
    }

    registry_free(ctx.registry);
    return root_ir_block;
}

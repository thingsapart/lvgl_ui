#include "generator.h"
#include "ir.h"
#include "registry.h"
#include "api_spec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h> // For bool type if used

// --- Context for Generation ---
typedef struct {
    Registry* registry;
    const ApiSpec* api_spec; // Changed to const ApiSpec*
    int var_counter; // For generating unique variable names
    IRStmtBlock* current_global_block; // For global statements like style declarations
} GenContext;

#include <ctype.h> // For isalnum, isdigit
// stdbool.h is already included

// Forward declarations
static void process_node(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context);
static void process_properties(GenContext* ctx, cJSON* props_json, const char* target_c_var_name, IRStmtBlock* current_block, const char* obj_type_for_api_lookup, cJSON* ui_context);
static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);


// --- Utility Functions ---

// Helper function to sanitize a string to be a valid C identifier.
// Output is a newly allocated string that must be freed by the caller.
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

// Generates a unique C variable name (e.g., "button_0", "style_1")
static char* generate_unique_var_name(GenContext* ctx, const char* base_type) {
    char buf[128];
    snprintf(buf, sizeof(buf), "%s_%d", base_type, ctx->var_counter++);
    return strdup(buf);
}

// --- Core Processing Functions ---

static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context) {
    if (!value) return ir_new_literal("NULL");
    if (cJSON_IsString(value)) {
        const char* s_orig = value->valuestring;
        char* s_duped = strdup(s_orig);
        char* s = s_duped;
        size_t len = strlen(s);
        if (len >= 2 && s[0] == s[len - 1]) {
            char boundary_char = s[0];
            if (boundary_char == '$' || boundary_char == '!' || boundary_char == '@' || boundary_char == '#' || boundary_char == '%') {
                if (boundary_char == '%' && len > 1 && s[len-2] == '%') {
                    s[len - 1] = '\0';
                } else if (boundary_char != '%') {
                    s[len - 1] = '\0';
                    memmove(s, s + 1, len - 1);
                    IRExpr* lit = ir_new_literal_string(s);
                    free(s_duped);
                    return lit;
                }
            }
        }
        if (s[0] == '$') {
            if (ui_context) {
                cJSON* ctx_val = cJSON_GetObjectItem(ui_context, s + 1);
                if (ctx_val) {
                    free(s_duped);
                    return unmarshal_value(ctx, ctx_val, ui_context);
                } else {
                    fprintf(stderr, "Warning: Context variable '%s' not found.\n", s + 1);
                    free(s_duped);
                    return ir_new_literal("NULL");
                }
            } else {
                 fprintf(stderr, "Warning: Attempted to access context variable '%s' with NULL context.\n", s + 1);
                 free(s_duped);
                 return ir_new_literal("NULL");
            }
        }
        if (s[0] == '@') {
            const char* registered_c_var = registry_get_generated_var(ctx->registry, s + 1);
            if (registered_c_var) {
                if (strstr(registered_c_var, "style") != NULL) {
                    IRExpr* var_expr = ir_new_variable(registered_c_var);
                    IRExpr* addr_expr = ir_new_address_of(var_expr);
                    free(s_duped);
                    return addr_expr;
                }
                IRExpr* var_expr = ir_new_variable(registered_c_var);
                free(s_duped);
                return var_expr;
            }
            IRExpr* var_expr = ir_new_variable(s + 1);
            free(s_duped);
            return var_expr;
        }
        if (s[0] == '#') {
            long hex_val = strtol(s + 1, NULL, 16);
            char hex_str_arg[32];
            snprintf(hex_str_arg, sizeof(hex_str_arg), "0x%06lX", hex_val);
            IRExprNode* args = ir_new_expr_node(ir_new_literal(hex_str_arg));
            IRExpr* call_expr = ir_new_func_call_expr("lv_color_hex", args);
            free(s_duped);
            return call_expr;
        }
        if (s[0] == '!') {
            IRExpr* lit_expr = ir_new_literal_string(s + 1);
            free(s_duped);
            return lit_expr;
        }
        len = strlen(s);
        if (len > 0 && s[len - 1] == '%') {
            char* temp_s = strdup(s);
            temp_s[len - 1] = '\0';
            char* endptr;
            long num_val = strtol(temp_s, &endptr, 10);
            if (*endptr == '\0') {
                char num_str_arg[32];
                snprintf(num_str_arg, sizeof(num_str_arg), "%ld", num_val);
                IRExprNode* args = ir_new_expr_node(ir_new_literal(num_str_arg));
                IRExpr* call_expr = ir_new_func_call_expr("lv_pct", args);
                free(temp_s);
                free(s_duped);
                return call_expr;
            }
            free(temp_s);
        }
        const cJSON* constants = api_spec_get_constants(ctx->api_spec);
        if (constants && cJSON_GetObjectItem(constants, s)) {
            IRExpr* lit_expr = ir_new_literal(s);
            free(s_duped);
            return lit_expr;
        }
        const cJSON* enums = api_spec_get_enums(ctx->api_spec);
        if (enums && cJSON_GetObjectItem(enums, s)) {
            IRExpr* lit_expr = ir_new_literal(s);
            free(s_duped);
            return lit_expr;
        }
        IRExpr* lit_expr = ir_new_literal_string(s);
        free(s_duped);
        return lit_expr;
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
    } else if (cJSON_IsNull(value)) {
        return ir_new_literal("NULL");
    }
    fprintf(stderr, "Warning: Unhandled JSON type in unmarshal_value or invalid structure. Returning NULL literal.\n");
    return ir_new_literal("NULL");
}

static void process_properties(GenContext* ctx, cJSON* props_json, const char* target_c_var_name, IRStmtBlock* current_block, const char* obj_type_for_api_lookup, cJSON* ui_context) {
    if (!props_json) return;
    cJSON* prop = NULL;
    for (prop = props_json->child; prop != NULL; prop = prop->next) {
        const char* prop_name = prop->string;
        const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, obj_type_for_api_lookup, prop_name);
        if (!prop_def) {
            if (strcmp(obj_type_for_api_lookup, "obj") != 0) {
                prop_def = api_spec_find_property(ctx->api_spec, "obj", prop_name);
            }
            if (!prop_def) {
                 if (strncmp(prop_name, "style_", 6) == 0 && strcmp(obj_type_for_api_lookup, "style") != 0) {
                    fprintf(stderr, "Info: Property '%s' on obj_type '%s' looks like a style property. Ensure it's applied to a style object or handled by add_style.\n", prop_name, obj_type_for_api_lookup);
                 } else if (strcmp(obj_type_for_api_lookup, "style") == 0 && api_spec_find_property(ctx->api_spec, "obj", prop_name)) {
                    fprintf(stderr, "Warning: Property '%s' applied to a style object '%s' is an object property, not a style property. This might be incorrect.\n", prop_name, target_c_var_name);
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
            if (strcmp(obj_type_for_api_lookup, "style") == 0) {
                 snprintf(constructed_setter, sizeof(constructed_setter), "lv_style_set_%s", prop_name);
            } else {
                 snprintf(constructed_setter, sizeof(constructed_setter), "lv_%s_set_%s",
                         prop_def->widget_type_hint ? prop_def->widget_type_hint : obj_type_for_api_lookup,
                         prop_name);
            }
            actual_setter_name_allocated = strdup(constructed_setter);
            actual_setter_name_const = actual_setter_name_allocated;
            fprintf(stderr, "Warning: Setter for '%s' on type '%s' constructed as '%s'. API spec should provide this.\n", prop_name, obj_type_for_api_lookup, actual_setter_name_const);
        }
        IRExprNode* args_list = ir_new_expr_node(ir_new_variable(target_c_var_name));
        bool is_complex_style_prop = false;
        if (prop_def->num_style_args > 0) {
            is_complex_style_prop = true;
        }
        cJSON* value_to_unmarshal = prop;
        const char* part_str = prop_def->style_part_default;
        const char* state_str = prop_def->style_state_default;
        if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value")) {
            cJSON* part_json = cJSON_GetObjectItem(prop, "part");
            cJSON* state_json = cJSON_GetObjectItem(prop, "state");
            cJSON* value_json = cJSON_GetObjectItem(prop, "value");
            if (cJSON_IsString(part_json)) part_str = part_json->valuestring;
            if (cJSON_IsString(state_json)) state_str = state_json->valuestring;
            value_to_unmarshal = value_json;
        }
        if (is_complex_style_prop) {
            if (strcmp(obj_type_for_api_lookup, "style") != 0) {
                 ir_expr_list_add(&args_list, ir_new_literal((char*)part_str));
            }
            ir_expr_list_add(&args_list, ir_new_literal((char*)state_str));
        }
        IRExpr* val_expr = unmarshal_value(ctx, value_to_unmarshal, ui_context);
        ir_expr_list_add(&args_list, val_expr);
        IRStmt* call_stmt = ir_new_func_call_stmt(actual_setter_name_const, args_list);
        ir_block_add_stmt(current_block, call_stmt);
        if (actual_setter_name_allocated) {
            free(actual_setter_name_allocated);
        }
    }
}

static void process_node(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context) {
    if (!cJSON_IsObject(node_json)) return;

    cJSON* type_item = cJSON_GetObjectItem(node_json, "type");
    const char* type_str = type_item ? cJSON_GetStringValue(type_item) : default_obj_type;

    if (!type_str || type_str[0] == '\0') {
         fprintf(stderr, "Error: Node missing valid 'type'. Skipping node processing.\n");
         return;
    }

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

    if (strcmp(type_str, "style") == 0) {
        cJSON* id_item_style = cJSON_GetObjectItem(node_json, "id");
        if (!cJSON_IsString(id_item_style) || !id_item_style->valuestring || id_item_style->valuestring[0] == '\0') {
            fprintf(stderr, "Error: Style node missing valid 'id' attribute. Skipping.\n");
            if (own_effective_context) cJSON_Delete(effective_context);
            return;
        }
        const char* json_id_style = id_item_style->valuestring;
        char* style_c_name = sanitize_c_identifier(json_id_style);

        const char* registry_key_style = (json_id_style[0] == '@') ? json_id_style + 1 : json_id_style;
        registry_add_generated_var(ctx->registry, registry_key_style, style_c_name);

        ir_block_add_stmt(current_node_ir_block, ir_new_var_decl("static lv_style_t", style_c_name, NULL));

        IRExpr* style_var_addr = ir_new_address_of(ir_new_variable(style_c_name));
        IRExprNode* init_args = ir_new_expr_node(style_var_addr);
        ir_block_add_stmt(current_node_ir_block, ir_new_func_call_stmt("lv_style_init", init_args));

        cJSON* props_to_process = cJSON_GetObjectItem(node_json, "properties");
        if (!props_to_process && cJSON_IsObject(node_json)) {
            props_to_process = node_json;
        }
        if(props_to_process){
             process_properties(ctx, props_to_process, style_c_name, current_node_ir_block, "style", effective_context);
        }
        free(style_c_name);

    } else if (strcmp(type_str, "use-view") == 0) {
        cJSON* component_ref_item = cJSON_GetObjectItem(node_json, "view_id");
        if (!cJSON_IsString(component_ref_item)) {
            component_ref_item = cJSON_GetObjectItem(node_json, "id");
            if (cJSON_IsString(component_ref_item)) {
                fprintf(stderr, "Warning: 'use-view' node is using 'id' ('%s') to specify component. Consider using 'view_id' for clarity.\n", component_ref_item->valuestring);
            }
        }

        if (!cJSON_IsString(component_ref_item) || !component_ref_item->valuestring || component_ref_item->valuestring[0] == '\0') {
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "Error: 'use-view' node missing 'view_id' (or valid 'id') attribute.");
            fprintf(stderr, "%s Node: %s\n", err_buf, cJSON_PrintUnformatted(node_json));
            ir_block_add_stmt(current_node_ir_block, ir_new_comment(err_buf));
            if (own_effective_context) cJSON_Delete(effective_context);
            return;
        }
        const char* component_def_id_from_json = component_ref_item->valuestring;

        if (component_def_id_from_json[0] != '@') {
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "Error: 'use-view' component reference '%s' must start with '@'.", component_def_id_from_json);
            fprintf(stderr, "%s\n", err_buf);
            ir_block_add_stmt(current_node_ir_block, ir_new_comment(err_buf));
            if (own_effective_context) cJSON_Delete(effective_context);
            return;
        }

        const cJSON* component_root_json = registry_get_component(ctx->registry, component_def_id_from_json + 1);

        if (!component_root_json) {
            char err_buf[256];
            snprintf(err_buf, sizeof(err_buf), "Error: Component definition '%s' not found in registry for 'use-view'.", component_def_id_from_json);
            fprintf(stderr, "%s\n", err_buf);
            ir_block_add_stmt(current_node_ir_block, ir_new_comment(err_buf));
            if (own_effective_context) cJSON_Delete(effective_context);
            return;
        }

        process_node(ctx, (cJSON*)component_root_json, current_node_ir_block, parent_c_var,
                     NULL,
                     effective_context);

        cJSON* use_view_prop_iter = node_json->child;
        bool has_override_props = false;
        while(use_view_prop_iter){
            const char* key = use_view_prop_iter->string;
            if(key && strcmp(key, "type") != 0 && strcmp(key, "view_id") != 0 && strcmp(key, "id") != 0 &&
               strcmp(key, "context") != 0 && strcmp(key, "named") != 0 && strcmp(key, "children") != 0 ) {
                has_override_props = true;
                break;
            }
            use_view_prop_iter = use_view_prop_iter->next;
        }
        if(has_override_props){
            char comment_buf[256];
            snprintf(comment_buf, sizeof(comment_buf), "Info: Properties on 'use-view' node ('%s') itself are currently ignored. Apply overrides within component or use 'with'.", component_def_id_from_json);
            ir_block_add_stmt(current_node_ir_block, ir_new_comment(comment_buf));
        }

    } else {
        char* current_widget_c_var = NULL;
        cJSON* named_item = cJSON_GetObjectItem(node_json, "named");
        cJSON* id_item = cJSON_GetObjectItem(node_json, "id");

        if (cJSON_IsString(named_item) && named_item->valuestring[0] != '\0') {
            current_widget_c_var = sanitize_c_identifier(named_item->valuestring);
            const char* registry_key_named = (named_item->valuestring[0] == '@') ? named_item->valuestring + 1 : named_item->valuestring;
            registry_add_generated_var(ctx->registry, registry_key_named, current_widget_c_var);
            if (cJSON_IsString(id_item) && id_item->valuestring[0] != '\0' && strcmp(id_item->valuestring, named_item->valuestring) != 0) {
                 const char* registry_key_id = (id_item->valuestring[0] == '@') ? id_item->valuestring + 1 : id_item->valuestring;
                 registry_add_generated_var(ctx->registry, registry_key_id, current_widget_c_var);
            }
        } else if (cJSON_IsString(id_item) && id_item->valuestring[0] != '\0') {
            current_widget_c_var = sanitize_c_identifier(id_item->valuestring);
            const char* registry_key_id = (id_item->valuestring[0] == '@') ? id_item->valuestring + 1 : id_item->valuestring;
            registry_add_generated_var(ctx->registry, registry_key_id, current_widget_c_var);
        } else {
            current_widget_c_var = generate_unique_var_name(ctx, type_str ? type_str : "widget");
        }

        cJSON* effective_properties = NULL;
        bool own_effective_properties = false;
        const char* final_widget_type_for_create = type_str;
        cJSON* component_definition = NULL;

        if (type_str[0] == '@') {
            component_definition = (cJSON*)registry_get_component(ctx->registry, type_str + 1);
            if (!component_definition) {
                fprintf(stderr, "Error: Component '%s' not found in registry. Skipping node.\n", type_str);
                if (own_effective_context) cJSON_Delete(effective_context);
                free(current_widget_c_var);
                return;
            }
            cJSON* comp_type_item = cJSON_GetObjectItem(component_definition, "type");
            if (!cJSON_IsString(comp_type_item)) {
                 fprintf(stderr, "Error: Component definition '%s' is missing 'type'. Skipping node.\n", type_str);
                 if (own_effective_context) cJSON_Delete(effective_context);
                 free(current_widget_c_var);
                 return;
            }
            final_widget_type_for_create = cJSON_GetStringValue(comp_type_item);

            cJSON* component_props = cJSON_GetObjectItem(component_definition, "properties");
            cJSON* instance_props = cJSON_GetObjectItem(node_json, "properties");

            if (component_props) {
                effective_properties = cJSON_Duplicate(component_props, true);
                own_effective_properties = true;
                if (instance_props) {
                    cJSON* prop_iter;
                    for (prop_iter = instance_props->child; prop_iter != NULL; prop_iter = prop_iter->next) {
                        if (cJSON_GetObjectItem(effective_properties, prop_iter->string)) {
                            cJSON_ReplaceItemInObject(effective_properties, prop_iter->string, cJSON_Duplicate(prop_iter, true));
                        } else {
                            cJSON_AddItemToObject(effective_properties, prop_iter->string, cJSON_Duplicate(prop_iter, true));
                        }
                    }
                }
            } else if (instance_props) {
                effective_properties = instance_props;
                own_effective_properties = false;
            }
        } else {
            final_widget_type_for_create = type_str;
            effective_properties = cJSON_GetObjectItem(node_json, "properties");
            own_effective_properties = false;
        }

        char parent_c_var_for_create[128];
        if (parent_c_var) {
            snprintf(parent_c_var_for_create, sizeof(parent_c_var_for_create), "%s", parent_c_var);
        } else {
            snprintf(parent_c_var_for_create, sizeof(parent_c_var_for_create), "lv_scr_act()");
        }

        char create_func_name[128];
        snprintf(create_func_name, sizeof(create_func_name), "lv_%s_create", final_widget_type_for_create);
        IRExprNode* create_args = ir_new_expr_node(ir_new_variable(parent_c_var_for_create));
        IRStmt* create_stmt = ir_new_var_decl("lv_obj_t*", current_widget_c_var, ir_new_func_call_expr(create_func_name, create_args));
        ir_block_add_stmt(current_node_ir_block, create_stmt);

        if (effective_properties) {
            process_properties(ctx, effective_properties, current_widget_c_var, current_node_ir_block, final_widget_type_for_create, effective_context);
        }

        cJSON* children_json = cJSON_GetObjectItem(component_definition ? component_definition : node_json, "children");
        if (cJSON_IsArray(children_json)) {
            cJSON* child_node_json;
            cJSON_ArrayForEach(child_node_json, children_json) {
                cJSON* child_type_item = cJSON_GetObjectItem(child_node_json, "type");
                const char* child_default_type = child_type_item ? cJSON_GetStringValue(child_type_item) : "obj";
                if(child_type_item && child_type_item->valuestring[0] == '@'){
                     const cJSON* child_comp_def = registry_get_component(ctx->registry, child_type_item->valuestring + 1);
                     if(child_comp_def) {
                        cJSON* child_comp_type_item = cJSON_GetObjectItem(child_comp_def, "type");
                        if(child_comp_type_item) child_default_type = cJSON_GetStringValue(child_comp_type_item);
                     }
                }
                process_node(ctx, child_node_json, current_node_ir_block, current_widget_c_var, child_default_type, effective_context);
            }
        }

        cJSON* with_prop = cJSON_GetObjectItem(node_json, "with");
        if (with_prop) {
            cJSON* current_with_item = NULL;
            if (cJSON_IsArray(with_prop)) {
                cJSON_ArrayForEach(current_with_item, with_prop) {
                    process_single_with_block(ctx, current_with_item, current_node_ir_block, effective_context);
                }
            } else if (cJSON_IsObject(with_prop)) {
                process_single_with_block(ctx, with_prop, current_node_ir_block, effective_context);
            }
        }

        if (own_effective_properties) {
            cJSON_Delete(effective_properties);
        }
        free(current_widget_c_var);
    }

    if (own_effective_context) {
        cJSON_Delete(effective_context);
    }
}

// New helper function for "with" blocks:
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
    char* generated_var_name_to_free = NULL; // Stores any name that was strdup'd and needs freeing locally

    const char* obj_type_for_props = "obj"; // Default type for properties lookup, e.g. "obj", "style"
    char* temp_var_c_type = "lv_obj_t*"; // Default C type for temporary variable, e.g. "lv_obj_t*", "lv_style_t*"


    if (obj_expr->type == IR_EXPR_VARIABLE) {
        // If obj is already a variable, use its name.
        // Duplicate the name because obj_expr will be freed.
        target_c_var_name = strdup(((IRExprVariable*)obj_expr)->name);
        generated_var_name_to_free = (char*)target_c_var_name; // Mark for freeing

        // Determine type for properties based on variable name heuristic
        if (strstr(target_c_var_name, "style") != NULL) {
            obj_type_for_props = "style";
        }
        ir_free((IRNode*)obj_expr); // Free the temporary IRExprVariable, we only needed its name.
    } else if (obj_expr->type == IR_EXPR_FUNC_CALL || obj_expr->type == IR_EXPR_ADDRESS_OF || obj_expr->type == IR_EXPR_LITERAL) {
        // If obj is a function call or other expression, create a temporary C variable.
        generated_var_name_to_free = generate_unique_var_name(ctx, "with_target"); // This is strdup'd
        target_c_var_name = generated_var_name_to_free;

        // Try to determine the C type and property type more accurately
        if (obj_expr->type == IR_EXPR_ADDRESS_OF) {
            IRExprAddressOf* addr_of = (IRExprAddressOf*)obj_expr;
            if (addr_of->expr && addr_of->expr->type == IR_EXPR_VARIABLE) {
                if (strstr(((IRExprVariable*)addr_of->expr)->name, "style") != NULL) {
                    temp_var_c_type = "lv_style_t*";
                    obj_type_for_props = "style";
                }
            }
        } else if (obj_expr->type == IR_EXPR_FUNC_CALL) {
            // TODO: Infer C type and property type from function return type in API spec
            // For now, default to lv_obj_t* and "obj"
        }

        IRStmt* var_decl_stmt = ir_new_var_decl(temp_var_c_type, target_c_var_name, obj_expr);
        // obj_expr is now owned by var_decl_stmt and will be freed with the IR tree.
        ir_block_add_stmt(parent_ir_block, var_decl_stmt);
    } else {
        fprintf(stderr, "Error: 'obj' expression in 'with' block yielded an unexpected IR type: %d.\n", obj_expr->type);
        ir_free((IRNode*)obj_expr); // Free if not used
        return;
    }

    // Process the "do" block, applying properties to target_c_var_name
    // The existing process_properties needs to be called here.
    // It currently takes: GenContext* ctx, cJSON* props_json, const char* widget_var_name, IRStmtBlock* current_block, cJSON* ui_context
    // We need to adapt it or make a new one that takes obj_type_for_props.
    // Let's assume we'll modify process_properties to accept obj_type_for_props.
    process_properties(ctx, do_json, target_c_var_name, parent_ir_block, obj_type_for_props, ui_context);
    process_properties(ctx, do_json, target_c_var_name, parent_ir_block, obj_type_for_props, ui_context);


    if (generated_var_name_to_free) {
        // This was either from strdup(var->name) or generate_unique_var_name.
        // The name string itself is copied by ir_new_variable or ir_new_var_decl.
        // So, this local copy can be freed.
        free(generated_var_name_to_free);
    }
}


// Process "styles" block from the UI spec
static void process_styles(GenContext* ctx, cJSON* styles_json, IRStmtBlock* global_block) {
    if (!cJSON_IsObject(styles_json)) return;

    cJSON* style_item = NULL;
    for (style_item = styles_json->child; style_item != NULL; style_item = style_item->next) {
        const char* style_name_json = style_item->string; // e.g., "style_main_button"
        if (!cJSON_IsObject(style_item)) {
            fprintf(stderr, "Warning: Style '%s' is not a JSON object. Skipping.\n", style_name_json);
            continue;
        }

        char* style_c_var = generate_unique_var_name(ctx, "style");
        registry_add_generated_var(ctx->registry, style_name_json, style_c_var); // Register "style_main_button" -> "style_0"

        // Create style variable: lv_style_t style_0;
        ir_block_add_stmt(global_block, ir_new_var_decl("lv_style_t", style_c_var, NULL));

        // Initialize style: lv_style_init(&style_0);
        IRExprNode* init_args = ir_new_expr_node(ir_new_address_of(ir_new_variable(style_c_var)));
        ir_block_add_stmt(global_block, ir_new_func_call_stmt("lv_style_init", init_args));

        // Process style properties
        cJSON* prop = NULL;
        for (prop = style_item->child; prop != NULL; prop = prop->next) {
            const char* prop_name = prop->string; // e.g., "bg_color"
            // For style definitions, we look up the property as a "style" type property
            const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, "style", prop_name);

            if (!prop_def || !prop_def->is_style_prop) { // Ensure it's marked as a style property
                 fprintf(stderr, "Warning: Property '%s' in style definition '%s' is not a recognized style property. Skipping.\n", prop_name, style_name_json);
                 continue;
            }

            // Construct setter: lv_style_set_PROP(&style_0, LV_STATE_DEFAULT, value)
            // Or for specific states: lv_style_set_bg_color(&style_0, LV_STATE_PRESSED, value)
            // The JSON for styles needs to support states. For now, assume LV_STATE_DEFAULT.
            // Example: "bg_color": "#FF0000" -> lv_style_set_bg_color(&style_0, LV_STATE_DEFAULT, lv_color_hex(0xFF0000))
            // If value is an object: "text_font": { "state": "PRESSED", "value": "@font_large" }

            char setter_name[128];
            // Most style setters are lv_style_set_property_name
            // The api_spec should ideally confirm this or provide the exact setter.
            // Example: prop_name "radius" -> "lv_style_set_radius"
            // prop_name "bg_color" -> "lv_style_set_bg_color"
            snprintf(setter_name, sizeof(setter_name), "lv_style_set_%s", prop_name);


            IRExprNode* style_setter_args = ir_new_expr_node(ir_new_address_of(ir_new_variable(style_c_var)));

            IRExpr* value_expr = NULL; // Initialize to NULL
            const char* state_str = "LV_STATE_DEFAULT"; // Default state

            if (cJSON_IsObject(prop)) { // Check for explicit state: "prop_name": { "state": "...", "value": "..." }
                cJSON* state_json = cJSON_GetObjectItem(prop, "state");
                cJSON* value_json = cJSON_GetObjectItem(prop, "value");
                if (cJSON_IsString(state_json) && value_json) {
                    // TODO: map state string (e.g., "PRESSED", "FOCUSED") to LVGL enum/macro string
                    // For now, assume the string itself is the macro e.g. "LV_STATE_PRESSED"
                    state_str = state_json->valuestring;
                    value_expr = unmarshal_value(ctx, value_json, NULL); // No specific ui_context for global styles generally
                } else {
                    fprintf(stderr, "Warning: Style property '%s' for style '%s' is an object but not in expected format {state:..., value:...}. Skipping.\n", prop_name, style_name_json);
                    free(value_expr); // if partially assigned
                    continue;
                }
            } else { // Simple value, use default state
                value_expr = unmarshal_value(ctx, prop, NULL);
            }

            ir_expr_list_add(&style_setter_args, ir_new_literal((char*)state_str)); // State argument
            ir_expr_list_add(&style_setter_args, value_expr);    // Value argument

            ir_block_add_stmt(global_block, ir_new_func_call_stmt(setter_name, style_setter_args));
        }
        free(style_c_var);
    }
}


// Main function to generate IR from UI specification
IRStmtBlock* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
    if (!ui_spec_root) {
        fprintf(stderr, "Error: UI Spec root is NULL in generate_ir_from_ui_spec.\n");
        return NULL;
    }
    if (!api_spec) {
        fprintf(stderr, "Error: API Spec is NULL in generate_ir_from_ui_spec.\n");
        return NULL;
    }
    if (!ui_spec_root) {
        fprintf(stderr, "Error: UI Spec root is NULL in generate_ir_from_ui_spec.\n");
        return NULL;
    }
    if (!api_spec) {
        fprintf(stderr, "Error: API Spec is NULL in generate_ir_from_ui_spec.\n");
        return NULL;
    }

    // EXPECT ui_spec_root TO BE AN ARRAY of definitions
    if (!cJSON_IsArray(ui_spec_root)) {
        fprintf(stderr, "Error: UI Spec root must be an array of definitions.\n");
        return NULL;
    }

    GenContext ctx;
    ctx.api_spec = api_spec;
    ctx.registry = registry_create();
    ctx.var_counter = 0;
    // ctx.current_global_block is not used by this function directly, but by helpers if needed.
    // It's typically set to the root_ir_block if styles or other global IR needs to be added by helpers.
    // For now, not setting it here as process_node takes the parent_block directly.

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
    // Set current_global_block if process_styles or other similar free-floating IR emitters are called.
    // Since process_styles is removed, this might not be needed unless process_node uses it.
    // For now, let's assume process_node appends directly to the passed parent_block.
    ctx.current_global_block = root_ir_block;


    cJSON* item_json = NULL;

    // First pass: register all components
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        cJSON* type_node = cJSON_GetObjectItem(item_json, "type");
        if (cJSON_IsString(type_node) && strcmp(type_node->valuestring, "component") == 0) {
            const char* id_str = NULL;
            cJSON* id_json = cJSON_GetObjectItem(item_json, "id");
            if (cJSON_IsString(id_json) && id_json->valuestring != NULL) {
                id_str = id_json->valuestring;
            } else {
                fprintf(stderr, "Warning: Component missing string 'id'. Skipping component registration.\n");
                continue;
            }

            cJSON* root_node_json = cJSON_GetObjectItem(item_json, "root");
            if (!root_node_json) {
                fprintf(stderr, "Warning: Component '%s' missing 'root' definition. Skipping component registration.\n", id_str);
                continue;
            }

            if (id_str[0] == '@') {
                registry_add_component(ctx.registry, id_str + 1, root_node_json);
            } else {
                fprintf(stderr, "Warning: Component id '%s' does not start with '@'. Skipping component registration.\n", id_str);
            }
        }
    }

    // Second pass: process all top-level nodes (styles, widgets, use-view)
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        cJSON* type_node = cJSON_GetObjectItem(item_json, "type");
        const char* type_str = type_node ? cJSON_GetStringValue(type_node) : NULL;

        if (type_str && strcmp(type_str, "component") == 0) {
            // Skip component definitions in this pass, they are handled by process_node when it encounters a "@component_id" type.
            continue;
        }

        if (type_str && strcmp(type_str, "style") == 0) {
            // If style definitions are items in the root array, they need to be processed.
            // The old process_styles iterated a "styles" object.
            // For now, assuming process_node is NOT equipped to handle style definitions directly to lv_style_t.
            // This will require process_node to be enhanced or a specific style processing call here.
            // For this subtask, we are just changing the iteration.
            // If process_styles is needed, it must be adapted to find style items in this flat array.
            // Or, we can call process_styles here if item_json is a style definition.
            // Let's assume process_node will just skip it if it's not a widget type it recognizes.
            // A more robust solution would be:
            // if (type_str && strcmp(type_str, "style") == 0) {
            //     process_single_style_definition(&ctx, item_json, root_ir_block); // New function
            // } else {
            //     process_node(&ctx, item_json, root_ir_block, "parent", "obj", NULL);
            // }
            // For now, per instruction, just call process_node for non-components.
            // This means styles defined as top-level items will be passed to process_node.
            // process_node's current logic will likely treat them as unknown widget types if "type": "style"
            // doesn't map to a create function like lv_style_create. This is a known limitation of current process_node.
             process_node(&ctx, item_json, root_ir_block, "parent", "obj", NULL);
        } else {
            // For other top-level items (widgets, use-view instances)
            process_node(&ctx, item_json, root_ir_block, "parent", "obj", NULL);
        }
    }

    registry_free(ctx.registry);
    return root_ir_block;
}

[end of generator.c]

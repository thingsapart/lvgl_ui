#include "generator.h"
#include "ir.h"
#include "api_spec.h"
#include "registry.h"
#include "debug_log.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// --- Generation Context ---
typedef struct {
    const ApiSpec* api_spec;
    Registry* registry;
    int var_counter;
} GenContext;

// --- Forward Declarations ---
static IRObject* parse_object(GenContext* ctx, cJSON* obj_json, const char* parent_c_name, const cJSON* ui_context);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* expected_c_type);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);
static bool types_compatible(const char* expected, const char* actual);
static void check_function_args(GenContext* ctx, const char* func_name, IRExprNode* args_list);

// --- Main Entry Point ---

IRRoot* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
    if (!ui_spec_root || !cJSON_IsArray(ui_spec_root)) {
        DEBUG_LOG(LOG_MODULE_GENERATOR, "Error: UI spec root is not a valid JSON array.");
        return NULL;
    }
    if (!api_spec) {
        DEBUG_LOG(LOG_MODULE_GENERATOR, "Error: API spec is NULL.");
        return NULL;
    }

    IRRoot* ir_root = ir_new_root();
    if (!ir_root) render_abort("Failed to create IR Root.");

    GenContext ctx = { .api_spec = api_spec, .registry = registry_create(), .var_counter = 0 };
    if (!ctx.registry) {
        ir_free((IRNode*)ir_root);
        render_abort("Failed to create registry.");
    }

    const char* root_parent_name = "parent";
    // Register the implicit parent so its type can be looked up.
    registry_add_generated_var(ctx.registry, root_parent_name, root_parent_name, "lv_obj_t*");


    cJSON* obj_json = NULL;
    cJSON_ArrayForEach(obj_json, ui_spec_root) {
        if (cJSON_IsObject(obj_json)) {
            IRObject* new_obj = parse_object(&ctx, obj_json, root_parent_name, NULL);
            if (new_obj) ir_object_list_add(&ir_root->root_objects, new_obj);
        }
    }

    registry_free(ctx.registry);
    return ir_root;
}

// --- Core Object Parser ---

static IRObject* parse_object(GenContext* ctx, cJSON* obj_json, const char* parent_c_name, const cJSON* ui_context) {
    if (!cJSON_IsObject(obj_json)) return NULL;

    const char* json_type_str = "obj";
    const char* object_c_type = "lv_obj_t*";
    const char* registered_id = NULL;

    cJSON* type_item = cJSON_GetObjectItem(obj_json, "type");
    cJSON* init_item = cJSON_GetObjectItem(obj_json, "init");
    cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
    if (!id_item) id_item = cJSON_GetObjectItem(obj_json, "name");

    if (type_item && cJSON_IsString(type_item)) json_type_str = type_item->valuestring;
    if (id_item && cJSON_IsString(id_item)) registered_id = id_item->valuestring;

    char* c_name = generate_unique_var_name(ctx, registered_id ? registered_id : json_type_str);

    // Determine the C type of the object we are creating.
    // Every object in the JSON tree corresponds to a new widget/object instance.
    // The "proxy object" concept is removed; if 'type' is missing, it's 'obj'.
    const WidgetDefinition* widget_def = api_spec_find_widget(ctx->api_spec, json_type_str);
    if (init_item && cJSON_IsObject(init_item) && init_item->child) {
        object_c_type = api_spec_get_function_return_type(ctx->api_spec, init_item->child->string);
    } else if (widget_def) {
        if (widget_def->create) {
            object_c_type = api_spec_get_function_return_type(ctx->api_spec, widget_def->create);
        } else if (widget_def->c_type) {
            object_c_type = widget_def->c_type;
        }
    }

    IRObject* ir_obj = ir_new_object(c_name, json_type_str, object_c_type, registered_id);
    free(c_name);
    if (registered_id) registry_add_generated_var(ctx->registry, registered_id, ir_obj->c_name, ir_obj->c_type);

    // Since every JSON object creates a new C object, a constructor/initializer is always needed.
    const char* target_c_name_for_calls = ir_obj->c_name;
    if (init_item) {
        // `init` takes precedence for creating the object.
        ir_obj->constructor_expr = unmarshal_value(ctx, init_item, ui_context, object_c_type);
    } else {
        // `type` or typeless (defaulting to 'obj') case.
        const char* create_func = NULL;
        if (widget_def && widget_def->create) {
            create_func = widget_def->create;
        } else if (strcmp(json_type_str, "obj") == 0) {
            // FIX: Hardcode the create function for the base 'obj' type if not in spec.
            create_func = "lv_obj_create";
        }

        if (create_func) {
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_registry_ref(parent_c_name, registry_get_c_type_for_id(ctx->registry, parent_c_name)));
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, create_func);
            ir_obj->constructor_expr = ir_new_expr_func_call(create_func, args, ret_type);
            check_function_args(ctx, create_func, args);
        } else if (widget_def && widget_def->init_func) {
            // For objects like styles that are initialized, not constructed via return value.
            IRExprNode* init_args = NULL;
            ir_expr_list_add(&init_args, ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type));
            ir_expr_list_add(&ir_obj->setup_calls, ir_new_expr_func_call(widget_def->init_func, init_args, "void"));
            check_function_args(ctx, widget_def->init_func, init_args);
        }
    }

    // Process all other keys as properties or methods.
    cJSON* prop_item = NULL;
    cJSON_ArrayForEach(prop_item, obj_json) {
        const char* key = prop_item->string;
        if (strcmp(key, "type") == 0 || strcmp(key, "init") == 0 || strcmp(key, "id") == 0 ||
            strcmp(key, "name") == 0 || strcmp(key, "children") == 0) continue;

        const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, ir_obj->json_type, key);
        const char* func_name = (prop_def && prop_def->setter) ? prop_def->setter : (api_spec_has_function(ctx->api_spec, key) ? key : NULL);
        if (!func_name) {
            DEBUG_LOG(LOG_MODULE_GENERATOR, "Warning: Could not resolve property/method '%s' for type '%s'.", key, ir_obj->json_type);
            continue;
        }

        const FunctionArg* expected_args = api_spec_get_function_args_by_name(ctx->api_spec, func_name);
        bool takes_implicit_obj = (expected_args && expected_args->type && (strstr(expected_args->type, "_t*")));

        IRExprNode* final_args = NULL;
        const FunctionArg* current_expected_arg = expected_args;

        if (takes_implicit_obj) {
            const char* target_c_type = registry_get_c_type_for_id(ctx->registry, target_c_name_for_calls);
            ir_expr_list_add(&final_args, ir_new_expr_registry_ref(target_c_name_for_calls, target_c_type));
            if(current_expected_arg) current_expected_arg = current_expected_arg->next; // Advance to next expected arg
        }

        cJSON* prop_val_array = cJSON_IsArray(prop_item) ? prop_item : NULL;
        if (prop_val_array) {
            cJSON* val_item = NULL;
            cJSON_ArrayForEach(val_item, prop_val_array) {
                const char* expected_type = current_expected_arg ? current_expected_arg->type : "unknown";
                IRExpr* arg_expr = unmarshal_value(ctx, val_item, ui_context, expected_type);
                if (arg_expr) ir_expr_list_add(&final_args, arg_expr);
                if (current_expected_arg) current_expected_arg = current_expected_arg->next;
            }
        } else {
            const char* expected_type = current_expected_arg ? current_expected_arg->type : "unknown";
            IRExpr* arg_expr = unmarshal_value(ctx, prop_item, ui_context, expected_type);
            if (arg_expr) ir_expr_list_add(&final_args, arg_expr);
        }

        const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
        ir_expr_list_add(&ir_obj->setup_calls, ir_new_expr_func_call(func_name, final_args, ret_type));
        check_function_args(ctx, func_name, final_args);
    }

    cJSON* children_item = cJSON_GetObjectItem(obj_json, "children");
    if (children_item && cJSON_IsArray(children_item)) {
        const char* child_parent_c_name = ir_obj->c_name;
        cJSON* child_json = NULL;
        cJSON_ArrayForEach(child_json, children_item) {
            ir_object_list_add(&ir_obj->children, parse_object(ctx, child_json, child_parent_c_name, ui_context));
        }
    }

    return ir_obj;
}


// --- Value Unmarshaler ---

static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* expected_c_type) {
    if (!value) return ir_new_expr_literal("NULL", "void*");
    if (cJSON_IsNull(value)) return ir_new_expr_literal("NULL", "void*");

    if (cJSON_IsString(value)) {
        const char* s = value->valuestring;
        size_t len = strlen(s);

        if (s[0] == '$') return ir_new_expr_context_var(s + 1, "unknown");
        if (s[0] == '@') {
            const char* type = registry_get_c_type_for_id(ctx->registry, s);
            return ir_new_expr_registry_ref(s, type ? type : "unknown");
        }
        if (s[0] == '!') return ir_new_expr_static_string(s + 1);

        // --- BUG FIX STARTS HERE ---
        if (s[0] == '#') { // Hex color
            long hex_val = strtol(s + 1, NULL, 16);
            char hex_str_arg[32];
            snprintf(hex_str_arg, sizeof(hex_str_arg), "0x%06lX", hex_val);

            // Correctly build the argument list for lv_color_hex
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_literal(hex_str_arg, "uint32_t"));

            return ir_new_expr_func_call("lv_color_hex", args, "lv_color_t");
        }
        if (len > 0 && s[len - 1] == '%') { // Percentage
            char* temp_s = strdup(s);
            if (!temp_s) return NULL;
            temp_s[len - 1] = '\0';

            // Correctly build the argument list for lv_pct
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_literal(temp_s, "int32_t"));
            free(temp_s);

            return ir_new_expr_func_call("lv_pct", args, "lv_coord_t");
        }
        // --- BUG FIX ENDS HERE ---

        if (api_spec_is_enum_member(ctx->api_spec, expected_c_type, s)) {
            return ir_new_expr_enum(s, 0, expected_c_type);
        }
        if (api_spec_is_global_enum_member(ctx->api_spec, s)) {
            return ir_new_expr_enum(s, 0, "enum");
        }

        return ir_new_expr_literal_string(s);
    }
    if (cJSON_IsNumber(value)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", value->valuedouble);
        // Coerce to expected type if it's a number-like type, otherwise default to int
        const char* type_str = (expected_c_type && (strstr(expected_c_type, "int") || strstr(expected_c_type, "coord"))) ? expected_c_type : "int";
        return ir_new_expr_literal(buf, type_str);
    }
    if (cJSON_IsBool(value)) {
        return ir_new_expr_literal(cJSON_IsTrue(value) ? "true" : "false", "bool");
    }
    if (cJSON_IsArray(value)) {
        IRExprNode* elements = NULL;
        cJSON* elem_json;
        cJSON_ArrayForEach(elem_json, value) {
            ir_expr_list_add(&elements, unmarshal_value(ctx, elem_json, ui_context, "unknown"));
        }
        return ir_new_expr_array(elements, "array");
    }
    if (cJSON_IsObject(value)) {
        cJSON* func_item = value->child;
        if (func_item && func_item->string) {
            const char* func_name = func_item->string;
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
            IRExprNode* args_list = NULL;
            if (cJSON_IsArray(func_item)) {
                cJSON* arg_json;
                cJSON_ArrayForEach(arg_json, func_item) {
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, arg_json, ui_context, "unknown"));
                }
            } else {
                ir_expr_list_add(&args_list, unmarshal_value(ctx, func_item, ui_context, "unknown"));
            }
            check_function_args(ctx, func_name, args_list);
            return ir_new_expr_func_call(func_name, args_list, ret_type);
        }
    }
    return ir_new_expr_literal("NULL", "unknown");
}

// --- Type Checking & Helpers ---

static bool types_compatible(const char* expected, const char* actual) {
    if (!expected || !actual) return false;
    if (strcmp(expected, "unknown") == 0 || strcmp(actual, "unknown") == 0) return true;
    if (strcmp(expected, actual) == 0) return true;

    if ((strcmp(expected, "lv_coord_t") == 0 || strcmp(expected, "int32_t") == 0) &&
        (strcmp(actual, "int") == 0 || strcmp(actual, "int32_t") == 0)) return true;
    if (strcmp(expected, "void*") == 0 && strchr(actual, '*') != NULL) return true;

    char clean_expected[128];
    snprintf(clean_expected, sizeof(clean_expected), "const %s", actual);
    if(strcmp(clean_expected, expected) == 0) return true;

    return false;
}

static void check_function_args(GenContext* ctx, const char* func_name, IRExprNode* args_list) {
    const FunctionArg* expected_arg = api_spec_get_function_args_by_name(ctx->api_spec, func_name);
    IRExprNode* actual_arg_node = args_list;
    int arg_idx = 0;

    while (expected_arg && actual_arg_node) {
        IRExpr* actual_expr = actual_arg_node->expr;
        if (!types_compatible(expected_arg->type, actual_expr->c_type)) {
            DEBUG_LOG(LOG_MODULE_GENERATOR, "Type Mismatch: Arg %d of '%s'. Expected '%s', but got '%s'.",
                arg_idx, func_name, expected_arg->type, actual_expr->c_type);
        }
        expected_arg = expected_arg->next;
        actual_arg_node = actual_arg_node->next;
        arg_idx++;
    }

    if (expected_arg && !actual_arg_node) {
        DEBUG_LOG(LOG_MODULE_GENERATOR, "Type Warning: Too few arguments for '%s'. Expected more args starting with type '%s'.", func_name, expected_arg->type);
    } else if (!expected_arg && actual_arg_node) {
         DEBUG_LOG(LOG_MODULE_GENERATOR, "Type Warning: Too many arguments for '%s'.", func_name);
    }
}

static char* sanitize_c_identifier(const char* input_name) {
    if (!input_name || *input_name == '\0') return strdup("unnamed_var");
    size_t len = strlen(input_name);
    char* sanitized = malloc(len + 2);
    if (!sanitized) return strdup("oom_var");
    char* s_ptr = sanitized;
    const char* i_ptr = input_name;
    if (*i_ptr == '@') i_ptr++;
    if (!isalpha((unsigned char)*i_ptr) && *i_ptr != '_') *s_ptr++ = '_';
    while (*i_ptr) {
        *s_ptr++ = (isalnum((unsigned char)*i_ptr) || *i_ptr == '_') ? *i_ptr : '_';
        i_ptr++;
    }
    *s_ptr = '\0';
    return sanitized;
}

static char* generate_unique_var_name(GenContext* ctx, const char* base_name) {
    char sanitized_base[256];
    char* temp_sanitized = sanitize_c_identifier(base_name);
    strncpy(sanitized_base, temp_sanitized, sizeof(sanitized_base) - 1);
    sanitized_base[sizeof(sanitized_base) - 1] = '\0';
    free(temp_sanitized);

    char* final_name = malloc(strlen(sanitized_base) + 16);
    if (!final_name) render_abort("Failed to allocate memory for variable name.");
    snprintf(final_name, strlen(sanitized_base) + 16, "%s_%d", sanitized_base, ctx->var_counter++);
    return final_name;
}

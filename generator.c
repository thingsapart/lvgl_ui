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
#include <stdarg.h>

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
static void process_and_validate_call(GenContext* ctx, const char* func_name, IRExprNode** args_list_ptr);
static void merge_json_objects(cJSON* dest, const cJSON* source);

// --- Warning & Error Reporting ---
static void generator_warning(const char* format, ...) {
    fprintf(stderr, "Generator Warning: ");
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

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

    // Pre-pass to find and register all components
    cJSON* item_json = NULL;
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        if (cJSON_IsObject(item_json)) {
            cJSON* type_item = cJSON_GetObjectItemCaseSensitive(item_json, "type");
            if (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "component") == 0) {
                cJSON* id_item = cJSON_GetObjectItemCaseSensitive(item_json, "id");
                cJSON* content_item = cJSON_GetObjectItemCaseSensitive(item_json, "content");
                if (id_item && cJSON_IsString(id_item) && content_item && cJSON_IsObject(content_item)) {
                    registry_add_component(ctx.registry, id_item->valuestring, content_item);
                    DEBUG_LOG(LOG_MODULE_GENERATOR, "Registered component: %s", id_item->valuestring);
                } else {
                    generator_warning("Found 'component' with missing 'id' or 'content'.");
                }
            }
        }
    }


    const char* root_parent_name = "parent";
    registry_add_generated_var(ctx.registry, root_parent_name, root_parent_name, "lv_obj_t*");


    cJSON* obj_json = NULL;
    cJSON_ArrayForEach(obj_json, ui_spec_root) {
        if (cJSON_IsObject(obj_json)) {
            cJSON* type_item = cJSON_GetObjectItemCaseSensitive(obj_json, "type");
            if (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "component") == 0) {
                continue;
            }

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
    const char* registered_id_from_json = NULL;

    cJSON* type_item = cJSON_GetObjectItem(obj_json, "type");
    if (type_item && cJSON_IsString(type_item)) json_type_str = type_item->valuestring;

    if (strcmp(json_type_str, "use-view") == 0) {
        cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
        if (!id_item || !cJSON_IsString(id_item)) {
            generator_warning("'use-view' requires a string 'id'.");
            return NULL;
        }

        const cJSON* component_content = registry_get_component(ctx->registry, id_item->valuestring);
        if (!component_content) {
            generator_warning("Component '%s' not found for 'use-view'.", id_item->valuestring);
            return NULL;
        }

        cJSON* merged_json = cJSON_Duplicate(component_content, true);
        cJSON* prop_item = NULL;
        cJSON_ArrayForEach(prop_item, obj_json) {
            const char* key = prop_item->string;
            if (strncmp(key, "//", 2) == 0) continue;
            if (strcmp(key, "type") == 0 || strcmp(key, "id") == 0 || strcmp(key, "context") == 0) continue;
            if (cJSON_HasObjectItem(merged_json, key)) {
                cJSON_ReplaceItemInObject(merged_json, key, cJSON_Duplicate(prop_item, true));
            } else {
                cJSON_AddItemToObject(merged_json, key, cJSON_Duplicate(prop_item, true));
            }
        }

        cJSON* new_context = cJSON_CreateObject();
        if (ui_context) merge_json_objects(new_context, ui_context);

        cJSON* local_context = cJSON_GetObjectItem(obj_json, "context");
        if (local_context) merge_json_objects(new_context, local_context);

        // Add parent reference to the context for use-view
        cJSON_AddItemToObject(new_context, "$parent", cJSON_CreateString(parent_c_name));

        IRObject* generated_obj = parse_object(ctx, merged_json, parent_c_name, new_context);

        cJSON_Delete(merged_json);
        cJSON_Delete(new_context);

        return generated_obj;
    }

    cJSON* new_scope_context = cJSON_CreateObject();
    if(ui_context) merge_json_objects(new_scope_context, ui_context);

    cJSON* local_context = cJSON_GetObjectItem(obj_json, "context");
    if (local_context && cJSON_IsObject(local_context)) {
        merge_json_objects(new_scope_context, local_context);
    }
    // Always add the parent to the context
    cJSON_AddItemToObject(new_scope_context, "$parent", cJSON_CreateString(parent_c_name));

    cJSON* init_item = cJSON_GetObjectItem(obj_json, "init");
    cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
    if (!id_item) id_item = cJSON_GetObjectItem(obj_json, "name");

    if (id_item && cJSON_IsString(id_item)) registered_id_from_json = id_item->valuestring;

    char* c_name = generate_unique_var_name(ctx, registered_id_from_json ? registered_id_from_json : json_type_str);
    cJSON_AddItemToObject(new_scope_context, "$current_obj", cJSON_CreateString(c_name));


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

    const char* clean_id = registered_id_from_json;
    if (clean_id && clean_id[0] == '@') {
        clean_id++;
    }
    IRObject* ir_obj = ir_new_object(c_name, json_type_str, object_c_type, clean_id);

    if (registered_id_from_json) {
        registry_add_generated_var(ctx->registry, registered_id_from_json, ir_obj->c_name, ir_obj->c_type);
        const char* id_for_runtime = (registered_id_from_json[0] == '@') ? registered_id_from_json + 1 : registered_id_from_json;
        IRExpr* obj_ref_expr = ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type);
        IRExpr* reg_call_expr = ir_new_expr_runtime_reg_add(id_for_runtime, obj_ref_expr);
        ir_expr_list_add(&ir_obj->setup_calls, reg_call_expr);
    }

    registry_add_generated_var(ctx->registry, c_name, ir_obj->c_name, ir_obj->c_type);

    if (init_item) {
        // This is not a constructor, it's an initializer that *receives* the parent
        // and returns a new object.
        IRExpr* init_expr = unmarshal_value(ctx, init_item, new_scope_context, object_c_type);

        // Check if the first argument of the init function is an object, and if so,
        // implicitly pass the parent.
        if (init_expr->base.type == IR_EXPR_FUNCTION_CALL) {
             IRExprFunctionCall* call = (IRExprFunctionCall*)init_expr;
             const FunctionDefinition* init_func_def = api_spec_find_function(ctx->api_spec, call->func_name);
             if (init_func_def && init_func_def->args_head) {
                 const char* first_arg_type = init_func_def->args_head->type;
                 if (first_arg_type && strstr(first_arg_type, "_t*")) {
                     IRExprNode* parent_arg_node = calloc(1, sizeof(IRExprNode));
                     parent_arg_node->expr = ir_new_expr_registry_ref(parent_c_name, first_arg_type);
                     parent_arg_node->next = call->args;
                     call->args = parent_arg_node;
                 }
             }
        }
        ir_obj->constructor_expr = init_expr;

    } else {
        const char* create_func = NULL;
        if (widget_def && widget_def->create) create_func = widget_def->create;
        else if (strcmp(json_type_str, "obj") == 0) create_func = "lv_obj_create";

        if (create_func) {
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_registry_ref(parent_c_name, "lv_obj_t*"));
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, create_func);
            ir_obj->constructor_expr = ir_new_expr_func_call(create_func, args, ret_type);
            process_and_validate_call(ctx, create_func, &((IRExprFunctionCall*)ir_obj->constructor_expr)->args);
        } else if (widget_def && widget_def->init_func) {
            // This is for non-widget objects like styles, which are not parented
            IRExprNode* init_args = NULL;
            // The c_code_printer handles passing by reference for non-pointers automatically
            ir_expr_list_add(&init_args, ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type));
            ir_expr_list_add(&ir_obj->setup_calls, ir_new_expr_func_call(widget_def->init_func, init_args, "void"));
            process_and_validate_call(ctx, widget_def->init_func, &init_args);
        }
    }

    cJSON* prop_item = NULL;
    cJSON_ArrayForEach(prop_item, obj_json) {
        const char* key = prop_item->string;
        if (strncmp(key, "//", 2) == 0) continue;
        if (strcmp(key, "type") == 0 || strcmp(key, "init") == 0 || strcmp(key, "id") == 0 ||
            strcmp(key, "name") == 0 || strcmp(key, "children") == 0 || strcmp(key, "context") == 0) continue;

        const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, ir_obj->json_type, key);
        const char* func_name = (prop_def && prop_def->setter) ? prop_def->setter : (api_spec_has_function(ctx->api_spec, key) ? key : NULL);
        if (!func_name) {
            generator_warning("Could not resolve property/method '%s' for type '%s'.", key, ir_obj->json_type);
            continue;
        }

        const FunctionDefinition* func_def = api_spec_find_function(ctx->api_spec, func_name);
        if (!func_def) {
            generator_warning("Could not find function definition for '%s'.", func_name);
            continue;
        }

        IRExprNode* final_args = NULL;
        const FunctionArg* expected_arg = func_def->args_head;

        bool takes_implicit_obj = (expected_arg && expected_arg->type && (strstr(expected_arg->type, "_t*")));
        if (takes_implicit_obj) {
            ir_expr_list_add(&final_args, ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type));
            expected_arg = expected_arg->next;
        }

        if (cJSON_IsArray(prop_item)) {
            cJSON* val_item = NULL;
            cJSON_ArrayForEach(val_item, prop_item) {
                const char* expected_type = expected_arg ? expected_arg->type : "unknown";
                IRExpr* arg_expr = unmarshal_value(ctx, val_item, new_scope_context, expected_type);
                ir_expr_list_add(&final_args, arg_expr);
                if (expected_arg) expected_arg = expected_arg->next;
            }
        } else {
            const char* expected_type = expected_arg ? expected_arg->type : "unknown";
            IRExpr* arg_expr = unmarshal_value(ctx, prop_item, new_scope_context, expected_type);
            ir_expr_list_add(&final_args, arg_expr);
        }

        process_and_validate_call(ctx, func_name, &final_args);

        const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
        ir_expr_list_add(&ir_obj->setup_calls, ir_new_expr_func_call(func_name, final_args, ret_type));
    }

    cJSON* children_item = cJSON_GetObjectItem(obj_json, "children");
    if (children_item && cJSON_IsArray(children_item)) {
        cJSON* child_json = NULL;
        cJSON_ArrayForEach(child_json, children_item) {
            ir_object_list_add(&ir_obj->children, parse_object(ctx, child_json, ir_obj->c_name, new_scope_context));
        }
    }

    free(c_name);
    cJSON_Delete(new_scope_context);

    return ir_obj;
}


// --- Value Unmarshaler ---

static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* expected_c_type) {
    if (!value) return ir_new_expr_literal("NULL", "void*");
    if (cJSON_IsNull(value)) return ir_new_expr_literal("NULL", "void*");

    if (cJSON_IsString(value)) {
        const char* s = value->valuestring;

        // Check for bitwise OR operator
        if (strchr(s, '|')) {
            long final_val = 0;
            char* temp_str = strdup(s);
            if (!temp_str) render_abort("Failed to duplicate string for OR-parsing");

            char* rest = temp_str;
            char* token;
            bool error = false;

            // Use strtok to split the string by '|'
            token = strtok(rest, "|");
            while (token != NULL) {
                // Trim whitespace from the current token
                while (isspace((unsigned char)*token)) token++;
                char* end = token + strlen(token) - 1;
                while (end > token && isspace((unsigned char)*end)) *end-- = '\0';

                if (strlen(token) > 0) {
                    long part_val = 0;
                    bool found = false;

                    // Try to resolve token as enum or constant.
                    if (expected_c_type && strcmp(expected_c_type, "unknown") != 0 && api_spec_is_enum_member(ctx->api_spec, expected_c_type, token)) {
                        if (api_spec_find_enum_value(ctx->api_spec, expected_c_type, token, &part_val)) {
                            found = true;
                        }
                    }
                    if (!found) {
                        const char* inferred_enum_type = api_spec_find_global_enum_type(ctx->api_spec, token);
                        if (inferred_enum_type) {
                            if (api_spec_find_enum_value(ctx->api_spec, inferred_enum_type, token, &part_val)) {
                                found = true;
                            }
                        }
                    }
                    if (!found) {
                        if (api_spec_find_constant_value(ctx->api_spec, token, &part_val)) {
                            found = true;
                        }
                    }

                    if (found) {
                        final_val |= part_val;
                    } else {
                        generator_warning("Could not resolve part '%s' of OR-expression '%s'", token, s);
                        error = true;
                        break;
                    }
                }
                // Get the next token
                token = strtok(NULL, "|");
            }

            free(temp_str);

            if (!error) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%ld", final_val);
                const char* final_type = (expected_c_type && strcmp(expected_c_type, "unknown") != 0) ? expected_c_type : "int";
                return ir_new_expr_literal(buf, final_type);
            }
            // If an error occurred, fall through to default string handling which will probably fail.
        }

        // Check for string constant like LV_SYMBOL_*
        char* const_str_val = api_spec_find_constant_string(ctx->api_spec, s);
        if (const_str_val) {
            // It's a string constant like a symbol. The generator should treat it as a static string.
            IRExpr* expr = ir_new_expr_static_string(const_str_val);
            free(const_str_val); // Free the string returned by api_spec_find_constant_string
            return expr;
        }

        size_t len = strlen(s);

        if (s[0] == '$') {
            const char* var_name = s + 1;
            // Handle special context variables
            if (strcmp(var_name, "parent") == 0) {
                 const cJSON* parent_name_json = cJSON_GetObjectItem(ui_context, "$parent");
                 if(parent_name_json && cJSON_IsString(parent_name_json)) {
                    const char* parent_c_name = parent_name_json->valuestring;
                    return ir_new_expr_registry_ref(parent_c_name, registry_get_c_type_for_id(ctx->registry, parent_c_name));
                 }
            }
            if (strcmp(var_name, "current_obj") == 0) {
                 const cJSON* current_name_json = cJSON_GetObjectItem(ui_context, "$current_obj");
                 if(current_name_json && cJSON_IsString(current_name_json)) {
                    const char* current_c_name = current_name_json->valuestring;
                    return ir_new_expr_registry_ref(current_c_name, registry_get_c_type_for_id(ctx->registry, current_c_name));
                 }
            }

            // Fallback to user-defined context
            if (ui_context && cJSON_IsObject(ui_context)) {
                const cJSON* context_val = cJSON_GetObjectItem(ui_context, var_name);
                if (context_val) {
                    return unmarshal_value(ctx, (cJSON*)context_val, ui_context, expected_c_type);
                }
            }
            generator_warning("Context variable '$%s' not found.", var_name);
            return ir_new_expr_context_var(var_name, "unknown");
        }
        if (s[0] == '@') {
            return ir_new_expr_registry_ref(s, registry_get_c_type_for_id(ctx->registry, s));
        }
        if (s[0] == '!') return ir_new_expr_static_string(s + 1);
        if (s[0] == '#') {
            long hex_val = strtol(s + 1, NULL, 16);
            char hex_str_arg[32];
            snprintf(hex_str_arg, sizeof(hex_str_arg), "0x%06lX", hex_val);
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_literal(hex_str_arg, "uint32_t"));
            return ir_new_expr_func_call("lv_color_hex", args, "lv_color_t");
        }
        if (len > 0 && s[len - 1] == '%') {
            char* temp_s = strdup(s);
            if (!temp_s) return NULL;
            temp_s[len - 1] = '\0';
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_literal(temp_s, "int32_t"));
            free(temp_s);
            return ir_new_expr_func_call("lv_pct", args, "lv_coord_t");
        }

        long const_val;
        if (api_spec_find_constant_value(ctx->api_spec, s, &const_val)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", const_val);
            return ir_new_expr_literal(buf, "int");
        }

        if (expected_c_type && api_spec_is_enum_member(ctx->api_spec, expected_c_type, s)) {
            long enum_val;
            api_spec_find_enum_value(ctx->api_spec, expected_c_type, s, &enum_val);
            return ir_new_expr_enum(s, enum_val, (char*)expected_c_type);
        }

        const char* inferred_enum_type = api_spec_find_global_enum_type(ctx->api_spec, s);
        if (inferred_enum_type) {
             long enum_val;
            api_spec_find_enum_value(ctx->api_spec, inferred_enum_type, s, &enum_val);
            return ir_new_expr_enum(s, enum_val, (char*)inferred_enum_type);
        }

        return ir_new_expr_literal_string(s);
    }
    if (cJSON_IsNumber(value)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", value->valuedouble);
        const char* type_str = "int";
        if (expected_c_type) {
            if (strstr(expected_c_type, "int") || strstr(expected_c_type, "coord") || strstr(expected_c_type, "opa_t") || strstr(expected_c_type, "selector")) {
                type_str = expected_c_type;
            }
        }
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
                const FunctionArg* expected_arg_list = api_spec_get_function_args_by_name(ctx->api_spec, func_name);
                cJSON_ArrayForEach(arg_json, func_item) {
                    const char* expected_type = "unknown";
                    if (expected_arg_list) {
                        expected_type = expected_arg_list->type;
                        expected_arg_list = expected_arg_list->next;
                    }
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, arg_json, ui_context, expected_type));
                }
            } else {
                 ir_expr_list_add(&args_list, unmarshal_value(ctx, func_item, ui_context, "unknown"));
            }
            process_and_validate_call(ctx, func_name, &args_list);
            return ir_new_expr_func_call(func_name, args_list, ret_type);
        }
    }
    return ir_new_expr_literal("NULL", "unknown");
}

// --- Type Checking & Helpers ---

static void merge_json_objects(cJSON* dest, const cJSON* source) {
    if (!cJSON_IsObject(dest) || !cJSON_IsObject(source)) return;
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, source) {
        if (cJSON_HasObjectItem(dest, item->string)) {
            cJSON_ReplaceItemInObject(dest, item->string, cJSON_Duplicate(item, true));
        } else {
            cJSON_AddItemToObject(dest, item->string, cJSON_Duplicate(item, true));
        }
    }
}

static bool types_compatible(const char* expected, const char* actual) {
    if (!expected || !actual) return false;
    if (strcmp(expected, "unknown") == 0 || strcmp(actual, "unknown") == 0) return true;
    if (strcmp(expected, "enum") == 0 && strstr(actual, "_t")) return true;
    if (strcmp(expected, actual) == 0) return true;

    // Handle const char* vs char*
    if ((strcmp(expected, "const char*") == 0 && strcmp(actual, "char*") == 0) ||
        (strcmp(expected, "char*") == 0 && strcmp(actual, "const char*") == 0)) return true;

    // More lenient check for style_t vs style_t*
    if (strcmp(expected, "lv_style_t*") == 0 && strcmp(actual, "lv_style_t") == 0) return true;


    // Handle integer type variations
    const char* int_types[] = {"int", "int32_t", "uint32_t", "lv_coord_t", "lv_style_selector_t", "lv_opa_t"};
    int num_int_types = sizeof(int_types) / sizeof(char*);
    bool expected_is_int = false;
    bool actual_is_int = false;
    for(int i=0; i < num_int_types; i++) {
        if(strcmp(expected, int_types[i]) == 0) expected_is_int = true;
        if(strcmp(actual, int_types[i]) == 0) actual_is_int = true;
    }
    if (expected_is_int && actual_is_int) return true;


    // Allow any pointer type to be passed as void*
    if (strcmp(expected, "void*") == 0 && strchr(actual, '*') != NULL) return true;

    return false;
}

static void process_and_validate_call(GenContext* ctx, const char* func_name, IRExprNode** args_list_ptr) {
    const FunctionDefinition* func_def = api_spec_find_function(ctx->api_spec, func_name);
    if (!func_def) {
        generator_warning("Cannot validate call to unknown function '%s'.", func_name);
        return;
    }

    int actual_argc = 0;
    for (IRExprNode* n = *args_list_ptr; n; n = n->next) actual_argc++;

    int expected_argc = 0;
    const FunctionArg* last_expected_arg = NULL;
    for (const FunctionArg* a = func_def->args_head; a; a = a->next) {
        // Skip void placeholder
        if(a->type && strcmp(a->type, "void") == 0) continue;
        expected_argc++;
        last_expected_arg = a;
    }

    // Special case for style setters missing the selector argument
    if (strncmp(func_name, "lv_obj_set_style_", 17) == 0 && actual_argc == expected_argc - 1) {
        if (last_expected_arg && strcmp(last_expected_arg->type, "lv_style_selector_t") == 0) {
            ir_expr_list_add(args_list_ptr, ir_new_expr_literal("0", "lv_style_selector_t"));
            actual_argc++; // Update count after adding the default
        }
    }

    if (actual_argc != expected_argc) {
        generator_warning("Argument count mismatch for '%s'. Expected %d, got %d.", func_name, expected_argc, actual_argc);
        return;
    }

    IRExprNode* actual_arg_node = *args_list_ptr;
    const FunctionArg* expected_arg = func_def->args_head;
    int i = 0;
    while (actual_arg_node && expected_arg) {
        if(expected_arg->type && strcmp(expected_arg->type, "void") == 0) {
             expected_arg = expected_arg->next;
             continue;
        }
        if (!types_compatible(expected_arg->type, actual_arg_node->expr->c_type)) {
            generator_warning("Type mismatch for argument %d of '%s'. Expected '%s', but got '%s'.",
                i, func_name, expected_arg->type, actual_arg_node->expr->c_type);
        }
        actual_arg_node = actual_arg_node->next;
        expected_arg = expected_arg->next;
        i++;
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

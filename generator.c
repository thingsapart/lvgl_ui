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

// --- Global Configuration (from main.c) ---
extern bool g_strict_mode;

// --- Generation Context ---
typedef struct {
    const ApiSpec* api_spec;
    Registry* registry;
    int var_counter;
} GenContext;

// --- Forward Declarations ---
static IRObject* parse_object(GenContext* ctx, cJSON* obj_json, const char* parent_c_name, const cJSON* ui_context);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* expected_c_type, const char* parent_c_name, const char* target_c_name, IRObject* ir_obj_for_warnings);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);
static void process_and_validate_call(GenContext* ctx, const char* func_name, IRExprNode** args_list_ptr);
static void merge_json_objects(cJSON* dest, const cJSON* source);
static int count_cjson_array(cJSON* array_json);
static int count_function_args(const FunctionArg* head);
static bool types_compatible(const char* expected, const char* actual);


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
                    // This is a generator-level problem, so a simple warning is fine.
                    print_warning("Found 'component' with missing 'id' or 'content'.");
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

    // --- Pass 1: Handle `use-view` and get essential properties for object creation ---
    const char* json_type_str = "obj";
    const char* registered_id_from_json = NULL;

    cJSON* type_item = cJSON_GetObjectItem(obj_json, "type");
    if (type_item && cJSON_IsString(type_item)) {
        json_type_str = type_item->valuestring;
    }

    if (strcmp(json_type_str, "use-view") == 0) {
        cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
        if (!id_item || !cJSON_IsString(id_item)) {
            print_warning("'use-view' requires a string 'id'.");
            return NULL;
        }
        const cJSON* component_content = registry_get_component(ctx->registry, id_item->valuestring);
        if (!component_content) {
            print_warning("Component '%s' not found for 'use-view'.", id_item->valuestring);
            return NULL;
        }

        cJSON* merged_json = cJSON_Duplicate(component_content, true);
        cJSON* prop_item = NULL;
        cJSON_ArrayForEach(prop_item, obj_json) {
            const char* key = prop_item->string;
            if (strncmp(key, "//", 2) == 0) continue;

            // --- NEW: Handle `children` as a special case for `use-view` ---
            if (strcmp(key, "children") == 0) {
                if (cJSON_IsArray(prop_item)) {
                    // Find or create the children array in the component's content
                    cJSON* merged_children = cJSON_GetObjectItem(merged_json, "children");
                    if (!merged_children) {
                        merged_children = cJSON_CreateArray();
                        cJSON_AddItemToObject(merged_json, "children", merged_children);
                    }
                    if (cJSON_IsArray(merged_children)) {
                        // Append the use-view's children to the component's children
                        cJSON* child_to_add;
                        cJSON_ArrayForEach(child_to_add, prop_item) {
                            cJSON_AddItemToArray(merged_children, cJSON_Duplicate(child_to_add, true));
                        }
                    }
                }
                continue; // Skip the default property-merging logic
            }
            
            if (strcmp(key, "type") == 0 || strcmp(key, "id") == 0 || strcmp(key, "context") == 0) continue;
            
            // Default behavior: override property in the component
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

        // This recursive call handles the component instantiation.
        // It passes the correct parent and the newly created context.
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

    cJSON* init_item = cJSON_GetObjectItem(obj_json, "init");
    cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
    if (!id_item) id_item = cJSON_GetObjectItem(obj_json, "name");
    if (id_item && cJSON_IsString(id_item)) registered_id_from_json = id_item->valuestring;

    char* c_name = generate_unique_var_name(ctx, registered_id_from_json ? registered_id_from_json : json_type_str);

    const char* object_c_type = "lv_obj_t*";
    const WidgetDefinition* widget_def = api_spec_find_widget(ctx->api_spec, json_type_str);
    char derived_c_type[256] = {0}; // Buffer for pointer type

    if (init_item && cJSON_IsObject(init_item) && init_item->child) {
        object_c_type = api_spec_get_function_return_type(ctx->api_spec, init_item->child->string);
    } else if (widget_def) {
        if (widget_def->create) {
            object_c_type = api_spec_get_function_return_type(ctx->api_spec, widget_def->create);
        } else if (widget_def->c_type) {
            // Check if it's an object with an init function (and not a create function)
            bool is_init_object = (widget_def->init_func != NULL && widget_def->create == NULL);
            if (is_init_object && strchr(widget_def->c_type, '*') == NULL) {
                // It's a struct type like "lv_style_t", and it has an init func.
                // We need to treat it as a pointer because we will heap-allocate it.
                snprintf(derived_c_type, sizeof(derived_c_type), "%s*", widget_def->c_type);
                object_c_type = derived_c_type;
            } else {
                object_c_type = widget_def->c_type;
            }
        }
    }

    const char* clean_id = registered_id_from_json;
    if (clean_id && clean_id[0] == '@') clean_id++;
    IRObject* ir_obj = ir_new_object(c_name, json_type_str, object_c_type, clean_id);

    // Register the variable name now so it can be self-referenced by @_parent
    registry_add_generated_var(ctx->registry, ir_obj->c_name, ir_obj->c_name, ir_obj->c_type);

    if (init_item) {
        if (cJSON_IsObject(init_item) && init_item->child) {
            const char* func_name = init_item->child->string;
            cJSON* user_args_json = init_item->child;

            const FunctionDefinition* func_def = api_spec_find_function(ctx->api_spec, func_name);
            if (!func_def) {
                char err_buf[256];
                snprintf(err_buf, sizeof(err_buf), "In 'init' block for '%s', could not find function definition for '%s'.", ir_obj->c_name, func_name);
                render_abort(err_buf);
            }

            const FunctionArg* first_expected_arg = func_def->args_head;
            bool func_expects_target = (first_expected_arg && first_expected_arg->type && strstr(first_expected_arg->type, "_t*"));
            int expected_argc = count_function_args(func_def->args_head);
            int user_argc = cJSON_IsArray(user_args_json) ? count_cjson_array(user_args_json) : (cJSON_IsNull(user_args_json) ? 0 : 1);

            bool prepend_target = (func_expects_target && user_argc == expected_argc - 1);

            IRExprNode* final_args = NULL;
            const FunctionArg* expected_arg_list_for_user = func_def->args_head;

            if (prepend_target) {
                ir_expr_list_add(&final_args, ir_new_expr_registry_ref(parent_c_name, first_expected_arg->type));
                if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
            }

            if (cJSON_IsArray(user_args_json)) {
                cJSON* val_item = user_args_json->child;
                while(val_item) {
                    const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                    ir_expr_list_add(&final_args, unmarshal_value(ctx, val_item, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj));
                    if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
                    val_item = val_item->next;
                }
            } else if (!cJSON_IsNull(user_args_json)) {
                const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                ir_expr_list_add(&final_args, unmarshal_value(ctx, user_args_json, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj));
            }

            process_and_validate_call(ctx, func_name, &final_args);
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
            ir_obj->constructor_expr = ir_new_expr_func_call(func_name, final_args, ret_type);

        } else {
            char err_buf[512];
            snprintf(err_buf, sizeof(err_buf), "The 'init' property for object with type '%s' and id '%s' must be a map containing a function call (e.g., { lv_obj_create: [parent] }), but a different type was found.", json_type_str, registered_id_from_json ? registered_id_from_json : "N/A");
            render_abort(err_buf);
        }
    } else {
        // No 'init' block; use the default create/init function for the type.
        const char* create_func = NULL;
        if (widget_def && widget_def->create) create_func = widget_def->create;
        else if (strcmp(json_type_str, "obj") == 0) create_func = "lv_obj_create";

        if (create_func) {
            // Implicit constructors always take the parent as the first argument.
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_registry_ref(parent_c_name, "lv_obj_t*"));
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, create_func);
            ir_obj->constructor_expr = ir_new_expr_func_call(create_func, args, ret_type);
            process_and_validate_call(ctx, create_func, &((IRExprFunctionCall*)ir_obj->constructor_expr)->args);
        } else if (widget_def && widget_def->init_func) {
            // It's an init-style object (e.g., style). We must heap-allocate it.
            // 1. The constructor is a malloc call.
            char sizeof_arg_buf[256];
            char* base_type = get_array_base_type(ir_obj->c_type); // e.g. "lv_style_t*" -> "lv_style_t"
            if(base_type) {
                snprintf(sizeof_arg_buf, sizeof(sizeof_arg_buf), "sizeof(%s)", base_type);
                free(base_type);
            } else {
                snprintf(sizeof_arg_buf, sizeof(sizeof_arg_buf), "0 /* Error: could not get base type for %s */", ir_obj->c_type);
            }

            IRExprNode* malloc_args = NULL;
            ir_expr_list_add(&malloc_args, ir_new_expr_literal(sizeof_arg_buf, "size_t"));
            // The C printer will need to handle the cast from void*
            ir_obj->constructor_expr = ir_new_expr_func_call("malloc", malloc_args, ir_obj->c_type);

            // 2. The lv_style_init call becomes a regular operation.
            IRExprNode* init_args = NULL;
            ir_expr_list_add(&init_args, ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type));
            IRExpr* init_call = ir_new_expr_func_call(widget_def->init_func, init_args, "void");
            process_and_validate_call(ctx, widget_def->init_func, &init_args);
            ir_operation_list_add(&ir_obj->operations, (IRNode*)init_call);
        }
    }

    if (registered_id_from_json) {
        registry_add_generated_var(ctx->registry, registered_id_from_json, ir_obj->c_name, ir_obj->c_type);
        const char* id_for_runtime = (registered_id_from_json[0] == '@') ? registered_id_from_json + 1 : registered_id_from_json;
        IRExpr* obj_ref_expr = ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type);
        IRExpr* reg_call_expr = ir_new_expr_runtime_reg_add(id_for_runtime, obj_ref_expr);
        ir_operation_list_add(&ir_obj->operations, (IRNode*)reg_call_expr);
    }
    

    cJSON* item = obj_json->child;
    while(item) {
        const char* key = item->string;
        if (strncmp(key, "//", 2) == 0 || strcmp(key, "type") == 0 || strcmp(key, "init") == 0 ||
            strcmp(key, "id") == 0 || strcmp(key, "name") == 0 || strcmp(key, "context") == 0) {
            item = item->next;
            continue;
        }

        if (strcmp(key, "children") == 0) {
            if (cJSON_IsArray(item)) {
                cJSON* child_json = NULL;
                cJSON_ArrayForEach(child_json, item) {
                    IRObject* child_obj = parse_object(ctx, child_json, ir_obj->c_name, new_scope_context);
                    if(child_obj) ir_operation_list_add(&ir_obj->operations, (IRNode*)child_obj);
                }
            }
        } else if (strcmp(key, "observes") == 0) {
            if (cJSON_IsObject(item)) {
                cJSON* obs_item;
                cJSON_ArrayForEach(obs_item, item) {
                    const char* state_name = obs_item->string;
                    char* format_str = NULL;
                    observer_update_type_t update_type;
                    if (strcmp(ir_obj->json_type, "label") == 0) update_type = OBSERVER_TYPE_LABEL_TEXT;
                    else if (strcmp(ir_obj->json_type, "switch") == 0) update_type = OBSERVER_TYPE_SWITCH_STATE;
                    else if (strcmp(ir_obj->json_type, "slider") == 0) update_type = OBSERVER_TYPE_SLIDER_VALUE;
                    else {
                        print_warning("Widget type '%s' not supported for 'observes'.", ir_obj->json_type);
                        continue;
                    }

                    if (cJSON_IsObject(obs_item) && obs_item->child) {
                        // Handles { float: "%8.3f" } or { bool: null }
                        if (cJSON_IsNull(obs_item->child)) {
                            format_str = NULL;
                        } else {
                            format_str = cJSON_GetStringValue(obs_item->child);
                        }
                    } else if (cJSON_IsString(obs_item)) {
                        format_str = obs_item->valuestring;
                    }
                    
                    ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_observer(state_name, update_type, format_str));
                }
            }
        } else if (strcmp(key, "action") == 0) {
            if (cJSON_IsObject(item)) {
                cJSON* act_item;
                cJSON_ArrayForEach(act_item, item) {
                    const char* action_name = act_item->string;
                    action_type_t action_type;
                    IRExpr* data_expr = NULL;

                    if (cJSON_IsString(act_item)) {
                        if (strcmp(act_item->valuestring, "trigger") == 0) action_type = ACTION_TYPE_TRIGGER;
                        else if (strcmp(act_item->valuestring, "toggle") == 0) action_type = ACTION_TYPE_TOGGLE;
                        else {
                             print_warning("Unknown action type string '%s' for action '%s'.", act_item->valuestring, action_name);
                             continue;
                        }
                    } else if (cJSON_IsArray(act_item)) {
                        action_type = ACTION_TYPE_CYCLE;
                        data_expr = unmarshal_value(ctx, act_item, new_scope_context, "binding_value_t*", parent_c_name, ir_obj->c_name, ir_obj);
                    } else {
                         print_warning("Unsupported action config for action '%s'.", action_name);
                         continue;
                    }
                    ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_action(action_name, action_type, data_expr));
                }
            }
        } else {
            const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, ir_obj->json_type, key);
            const char* func_name = (prop_def && prop_def->setter) ? prop_def->setter : (api_spec_has_function(ctx->api_spec, key) ? key : NULL);
            if (!func_name) {
                char warning_msg[256];
                snprintf(warning_msg, sizeof(warning_msg), "Could not resolve property/method '%s' for type '%s'.", key, ir_obj->json_type);
                ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_warning(warning_msg));
                item = item->next;
                continue;
            }

            const FunctionDefinition* func_def = api_spec_find_function(ctx->api_spec, func_name);
            if (!func_def) {
                 char warning_msg[256];
                snprintf(warning_msg, sizeof(warning_msg), "Could not find function definition for '%s'.", func_name);
                ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_warning(warning_msg));
                item = item->next;
                continue;
            }

            const FunctionArg* first_expected_arg = func_def->args_head;
            bool func_expects_target = (first_expected_arg && first_expected_arg->type && strstr(first_expected_arg->type, "_t*"));
            int expected_argc = count_function_args(func_def->args_head);
            const FunctionArg* single_arg_after_target = (func_expects_target && expected_argc == 2) ? func_def->args_head->next : ((!func_expects_target && expected_argc == 1) ? func_def->args_head : NULL);
            bool expects_single_array = single_arg_after_target && strstr(single_arg_after_target->type, "*");

            IRExprNode* final_args = NULL;
            const FunctionArg* expected_arg_list_for_user = func_def->args_head;
            
            if (func_expects_target) {
                ir_expr_list_add(&final_args, ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type));
                if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
            }

            if (cJSON_IsArray(item) && !expects_single_array) {
                // Case 1: JSON array is a list of multiple arguments
                cJSON* val_item = item->child;
                while(val_item) {
                    const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                    ir_expr_list_add(&final_args, unmarshal_value(ctx, val_item, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj));
                    if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
                    val_item = val_item->next;
                }
            } else {
                 // Case 2: JSON value is a single argument (which could be a JSON array if expects_single_array is true)
                 const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                 ir_expr_list_add(&final_args, unmarshal_value(ctx, item, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj));
            }

            process_and_validate_call(ctx, func_name, &final_args);
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
            ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_expr_func_call(func_name, final_args, ret_type));
        }
        item = item->next;
    }

    free(c_name);
    cJSON_Delete(new_scope_context);

    return ir_obj;
}


// --- Value Unmarshaler ---

static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* expected_c_type, const char* parent_c_name, const char* target_c_name, IRObject* ir_obj_for_warnings) {
    if (!value) return ir_new_expr_literal("NULL", "void*");
    if (cJSON_IsNull(value)) return ir_new_expr_literal("NULL", "void*");

    if (cJSON_IsString(value)) {
        const char* s = value->valuestring;

        if (strcmp(s, "@_target") == 0) {
            if (parent_c_name) {
                const char* parent_type = registry_get_c_type_for_id(ctx->registry, parent_c_name);
                return ir_new_expr_registry_ref(parent_c_name, parent_type ? parent_type : "lv_obj_t*");
            } else {
                if (ir_obj_for_warnings) {
                    ir_operation_list_add(&ir_obj_for_warnings->operations, (IRNode*)ir_new_warning("Using '@_target' in a context where the parent is not available."));
                }
                return ir_new_expr_literal("NULL", "void*");
            }
        }
        if (strcmp(s, "@_parent") == 0) {
            if (target_c_name) {
                const char* target_type = registry_get_c_type_for_id(ctx->registry, target_c_name);
                return ir_new_expr_registry_ref(target_c_name, target_type ? target_type : "lv_obj_t*");
            } else {
                if (ir_obj_for_warnings) {
                    ir_operation_list_add(&ir_obj_for_warnings->operations, (IRNode*)ir_new_warning("Using '@_parent' in a context where the current object is not yet defined."));
                }
                return ir_new_expr_literal("NULL", "void*");
            }
        }


        if (s[0] == '$') {
            const char* var_name = s + 1;
            const cJSON* context_val_json = NULL;

            if (ui_context && cJSON_IsObject(ui_context)) {
                 context_val_json = cJSON_GetObjectItem(ui_context, var_name);
            }

            if (context_val_json) {
                return unmarshal_value(ctx, (cJSON*)context_val_json, ui_context, expected_c_type, parent_c_name, target_c_name, ir_obj_for_warnings);
            }
            if (ir_obj_for_warnings) {
                char warning_msg[128];
                snprintf(warning_msg, sizeof(warning_msg), "Context variable '%s' not found.", s);
                ir_operation_list_add(&ir_obj_for_warnings->operations, (IRNode*)ir_new_warning(warning_msg));
            }
            return ir_new_expr_context_var(var_name, "unknown");
        }

        if (strchr(s, '|')) {
            long final_val = 0;
            char* temp_str = strdup(s);
            if (!temp_str) render_abort("Failed to duplicate string for OR-parsing");
            char* rest = temp_str;
            char* token;
            bool error = false;
            token = strtok(rest, "|");
            while (token != NULL) {
                while (isspace((unsigned char)*token)) token++;
                char* end = token + strlen(token) - 1;
                while (end > token && isspace((unsigned char)*end)) *end-- = '\0';
                if (strlen(token) > 0) {
                    long part_val = 0;
                    bool found = false;
                    if (expected_c_type && strcmp(expected_c_type, "unknown") != 0 && api_spec_is_enum_member(ctx->api_spec, expected_c_type, token)) {
                        if (api_spec_find_enum_value(ctx->api_spec, expected_c_type, token, &part_val)) found = true;
                    }
                    if (!found) {
                        const char* inferred_enum_type = api_spec_find_global_enum_type(ctx->api_spec, token);
                        if (inferred_enum_type) {
                            if (api_spec_find_enum_value(ctx->api_spec, inferred_enum_type, token, &part_val)) found = true;
                        }
                    }
                    if (!found) {
                        if (api_spec_find_constant_value(ctx->api_spec, token, &part_val)) found = true;
                    }
                    if (found) {
                        final_val |= part_val;
                    } else {
                        if (ir_obj_for_warnings) {
                            char warning_msg[256];
                            snprintf(warning_msg, sizeof(warning_msg), "Could not resolve part '%s' of OR-expression '%s'", token, s);
                            ir_operation_list_add(&ir_obj_for_warnings->operations, (IRNode*)ir_new_warning(warning_msg));
                        }
                        error = true;
                        break;
                    }
                }
                token = strtok(NULL, "|");
            }
            free(temp_str);
            if (!error) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%ld", final_val);
                const char* final_type = (expected_c_type && strcmp(expected_c_type, "unknown") != 0) ? expected_c_type : "int";
                return ir_new_expr_literal(buf, final_type);
            }
        }

        char* const_str_val = api_spec_find_constant_string(ctx->api_spec, s);
        if (const_str_val) {
            size_t unescaped_len = 0;
            char* unescaped_val = unescape_c_string(const_str_val, &unescaped_len);
            IRExpr* expr = ir_new_expr_static_string(unescaped_val, unescaped_len);
            free(const_str_val);
            free(unescaped_val);
            return expr;
        }

        long const_val;
        if (api_spec_find_constant_value(ctx->api_spec, s, &const_val)) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", const_val);
            return ir_new_expr_literal(buf, "int");
        }

        size_t len = strlen(s);

        if (s[0] == '@') {
            return ir_new_expr_registry_ref(s, registry_get_c_type_for_id(ctx->registry, s));
        }
        if (s[0] == '!') {
            size_t unescaped_len = 0;
            char* unescaped_val = unescape_c_string(s + 1, &unescaped_len);
            IRExpr* expr = ir_new_expr_static_string(unescaped_val, unescaped_len);
            free(unescaped_val);
            return expr;
        }
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

        size_t unescaped_len = 0;
        char* unescaped_val = unescape_c_string(s, &unescaped_len);
        IRExpr* expr = ir_new_expr_literal_string(unescaped_val, unescaped_len);
        free(unescaped_val);
        return expr;
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
        if (ir_obj_for_warnings && expected_c_type) {
            const cJSON* enum_type_json = cJSON_GetObjectItem(ctx->api_spec->enums, expected_c_type);
            if (enum_type_json) { // It's an enum type
                long int_val = (long)value->valuedouble;
                const char* symbol = api_spec_find_enum_symbol_by_value(ctx->api_spec, expected_c_type, int_val);
                if (symbol) {
                    char warning_msg[256];
                    snprintf(warning_msg, sizeof(warning_msg), "For a '%s' argument, you provided the integer '%ld'. For clarity, consider using the symbolic name '%s' instead.", expected_c_type, int_val, symbol);
                    ir_operation_list_add(&ir_obj_for_warnings->operations, (IRNode*)ir_new_warning(warning_msg));
                }
            }
        }
        return ir_new_expr_literal(buf, type_str);
    }
    if (cJSON_IsBool(value)) {
        return ir_new_expr_literal(cJSON_IsTrue(value) ? "true" : "false", "bool");
    }
    if (cJSON_IsArray(value)) {
        char* base_type = get_array_base_type(expected_c_type);
        IRExprNode* elements = NULL;
        cJSON* elem_json;
        cJSON_ArrayForEach(elem_json, value) {
            ir_expr_list_add(&elements, unmarshal_value(ctx, elem_json, ui_context, base_type, parent_c_name, target_c_name, ir_obj_for_warnings));
        }
        free(base_type);
        return ir_new_expr_array(elements, (char*)expected_c_type);
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
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, arg_json, ui_context, expected_type, parent_c_name, target_c_name, ir_obj_for_warnings));
                }
            } else {
                 ir_expr_list_add(&args_list, unmarshal_value(ctx, func_item, ui_context, "unknown", parent_c_name, target_c_name, ir_obj_for_warnings));
            }
            process_and_validate_call(ctx, func_name, &args_list);
            return ir_new_expr_func_call(func_name, args_list, ret_type);
        }
    }
    return ir_new_expr_literal("NULL", "unknown");
}

// --- Type Checking & Helpers ---

static int count_cjson_array(cJSON* array_json) {
    if (!cJSON_IsArray(array_json)) return 0;
    int count = 0;
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, array_json) {
        count++;
    }
    return count;
}

static int count_function_args(const FunctionArg* head) {
    int count = 0;
    for (const FunctionArg* arg = head; arg; arg = arg->next) {
        if (arg->type && strcmp(arg->type, "void") != 0) {
            count++;
        }
    }
    return count;
}

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
    const char* int_types[] = {"int", "int32_t", "uint32_t", "lv_coord_t", "lv_style_selector_t", "lv_opa_t", "bool", "lv_anim_enable_t"};
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
        // This should have been caught earlier, but as a safeguard.
        print_warning("Cannot validate call to unknown function '%s'.", func_name);
        return;
    }

    int actual_argc = 0;
    for (IRExprNode* n = *args_list_ptr; n; n = n->next) actual_argc++;

    int expected_argc = 0;
    const FunctionArg* last_expected_arg = NULL;
    for (const FunctionArg* a = func_def->args_head; a; a = a->next) {
        if(a->type && strcmp(a->type, "void") == 0) continue;
        expected_argc++;
        last_expected_arg = a;
    }

    // --- FIX 2A: Handle implicit style selector argument ---
    if (strncmp(func_name, "lv_obj_set_style_", 17) == 0 && actual_argc == expected_argc - 1) {
        if (last_expected_arg && strcmp(last_expected_arg->type, "lv_style_selector_t") == 0) {
            // Append the missing default selector '0' to the argument list
            ir_expr_list_add(args_list_ptr, ir_new_expr_literal("0", "lv_style_selector_t"));
            actual_argc++; // Update the count to reflect the added argument
        }
    }

    if (actual_argc != expected_argc) {
        if (g_strict_mode) {
             char err_buf[256];
             snprintf(err_buf, sizeof(err_buf), "Strict mode failure: Argument count mismatch for '%s'. Expected %d, got %d.", func_name, expected_argc, actual_argc);
             render_abort(err_buf);
        }
        // In non-strict mode, implicit argument handling is a feature, so no warning is issued.
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
            // Type mismatches are hints, not errors, as the unmarshaler can get it wrong.
            // print_warning("Type mismatch for argument %d of '%s'. Expected '%s', but got '%s'.",
            //     i, func_name, expected_arg->type, actual_arg_node->expr->c_type);
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


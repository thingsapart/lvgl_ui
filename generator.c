#include "generator.h"
#include "ir.h"
#include "api_spec.h"
#include "registry.h"
#include "debug_log.h"
#include "utils.h"
#include "yaml_parser.h"
#include "ui_sim.h" // ADDED: For UI-Sim processing
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

// --- Global Configuration (from main.c) ---
extern bool g_strict_mode;
extern bool g_strict_registry_mode;

// --- Generation Context ---
typedef struct {
    const ApiSpec* api_spec;
    Registry* registry;
    int var_counter;
    bool error_occurred; // Flag to stop processing on error
} GenContext;

// --- Forward Declarations ---
static IRObject* parse_object(GenContext* ctx, cJSON* obj_json, const char* parent_c_name, const cJSON* ui_context);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* expected_c_type, const char* parent_c_name, const char* target_c_name, IRObject* ir_obj_for_warnings);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);
static void process_and_validate_call(GenContext* ctx, const char* func_name, IRExprNode** args_list_ptr, IRObject* ir_obj_for_warnings);
static void merge_json_objects(cJSON* dest, const cJSON* source);
static int count_cjson_array(cJSON* array_json);
static int count_function_args(const FunctionArg* head);
static bool types_compatible(const char* expected, const char* actual);
static cJSON* process_context_keys_recursive(const cJSON* source_json, const cJSON* context);


// --- Main Entry Point ---

IRRoot* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
    if (!ui_spec_root || !cJSON_IsArray(ui_spec_root)) {
        render_abort("UI spec root must be a valid JSON array.");
        return NULL;
    }
    if (!api_spec) {
        render_abort("API spec is NULL.");
        return NULL;
    }

    IRRoot* ir_root = ir_new_root();
    if (!ir_root) {
        render_abort("Failed to create IR Root.");
        return NULL;
    }

    GenContext ctx = { .api_spec = api_spec, .registry = registry_create(), .var_counter = 0, .error_occurred = false };
    if (!ctx.registry) {
        ir_free((IRNode*)ir_root);
        render_abort("Failed to create registry.");
        return NULL;
    }

    // Pre-pass to find and register all components
    cJSON* item_json = NULL;
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        if (cJSON_IsObject(item_json)) {
            cJSON* type_item = cJSON_GetObjectItemCaseSensitive(item_json, "type");
            if (type_item && cJSON_IsString(type_item)) {
                if (strcmp(type_item->valuestring, "component") == 0) {
                    cJSON* id_item = cJSON_GetObjectItemCaseSensitive(item_json, "id");
                    cJSON* content_item = cJSON_GetObjectItemCaseSensitive(item_json, "content");
                    if (id_item && cJSON_IsString(id_item) && content_item && cJSON_IsObject(content_item)) {
                        registry_add_component(ctx.registry, id_item->valuestring, content_item);
                        DEBUG_LOG(LOG_MODULE_GENERATOR, "Registered component: %s", id_item->valuestring);
                    } else {
                      if (!id_item) {
                        print_warning("Found 'component' with missing 'id'.");
                      } else if (!cJSON_IsString(id_item)) {
                        print_warning("Found 'component' with 'id' that is not a string.");
                      }
                      if (!content_item) {
                        print_warning("Found 'component' with missing 'content'.");

                      } else if (!cJSON_IsObject(content_item)) {
                        print_warning("Found 'component' with 'content' that is not an 'object' (aka 'hash' or 'dict').");
                      }
                    }
                }
            }
        }
    }


    const char* root_parent_name = "parent";
    registry_add_generated_var(ctx.registry, root_parent_name, root_parent_name, "lv_obj_t*");


    cJSON* obj_json = NULL;
    cJSON_ArrayForEach(obj_json, ui_spec_root) {
        if (ctx.error_occurred) break; // Stop processing if an error was found in a previous iteration

        if (cJSON_IsObject(obj_json)) {
            cJSON* type_item = cJSON_GetObjectItemCaseSensitive(obj_json, "type");
            if (type_item && cJSON_IsString(type_item)) {
                // --- Handle special top-level block types ---
                if (strcmp(type_item->valuestring, "component") == 0) {
                    continue; // Skip component definitions in the main rendering pass
                }
                if (strcmp(type_item->valuestring, "data-binding") == 0) {
                    if (!ui_sim_process_node(obj_json)) {
                        // Error already reported by ui_sim
                        ctx.error_occurred = true;
                    }
                    continue; // This block is consumed by UI-Sim, not rendered as a widget
                }
            }

            IRObject* new_obj = parse_object(&ctx, obj_json, root_parent_name, NULL);
            if (ctx.error_occurred) {
                // parse_object already cleaned up after itself and reported the error.
                // We just need to stop and clean up the root.
                break;
            }
            if (new_obj) ir_object_list_add(&ir_root->root_objects, new_obj);
        }
    }

    registry_free(ctx.registry);

    if (ctx.error_occurred) {
        ir_free((IRNode*)ir_root);
        return NULL;
    }

    return ir_root;
}

IRRoot* generate_ir_from_string(const char* ui_spec_string, const ApiSpec* api_spec) {
    if (!ui_spec_string || strlen(ui_spec_string) == 0) {
        // Not an error, just an empty UI.
        return ir_new_root();
    }

    cJSON* ui_spec_json = NULL;
    char* error_msg = NULL;

    // Heuristic to detect if it's YAML or JSON. JSON usually starts with { or [.
    const char* p = ui_spec_string;
    while (*p && isspace((unsigned char)*p)) p++;

    bool tried_json = false;
    if (*p == '{' || *p == '[') {
        tried_json = true;
        ui_spec_json = cJSON_Parse(ui_spec_string);
        if (!ui_spec_json) {
            // It might be YAML that happens to start with { or [, so try YAML parser as a fallback.
            ui_spec_json = yaml_to_cjson(ui_spec_string, &error_msg);
        }
    } else {
        // Doesn't look like JSON, assume YAML.
        ui_spec_json = yaml_to_cjson(ui_spec_string, &error_msg);
    }

    if (error_msg) {
        render_abort(error_msg);
        free(error_msg);
        cJSON_Delete(ui_spec_json);
        return NULL;
    }

    if (!ui_spec_json) {
        if (tried_json) {
            render_abort(cJSON_GetErrorPtr());
        } else {
            render_abort("Failed to parse UI specification. Content is not valid YAML or JSON.");
        }
        return NULL;
    }

    IRRoot* ir_root = generate_ir_from_ui_spec(ui_spec_json, api_spec);

    cJSON_Delete(ui_spec_json);

    return ir_root;
}

IRRoot* generate_ir_from_file(const char* ui_spec_path, const ApiSpec* api_spec) {
    char* ui_spec_content = read_file(ui_spec_path);
    if (!ui_spec_content) {
        char err_buf[512];
        snprintf(err_buf, sizeof(err_buf), "Error reading UI spec file: %s", ui_spec_path);
        render_abort(err_buf);
        return NULL;
    }

    IRRoot* ir_root = generate_ir_from_string(ui_spec_content, api_spec);

    free(ui_spec_content);

    if (!ir_root) {
        // Error was already reported by generate_ir_from_string.
        DEBUG_LOG(LOG_MODULE_GENERATOR, "Failed to generate IR from the UI spec file '%s'.", ui_spec_path);
    }

    return ir_root;
}

// --- NEW ---
/**
 * @brief Recursively traverses a cJSON structure, creating a deep copy.
 * During the copy, it inspects every object key. If a key starts with '$',
 * it attempts to replace it with the corresponding string value from the context.
 *
 * @param source_json The cJSON structure to process.
 * @param context The cJSON object containing context variables.
 * @return A new, fully processed cJSON structure. The caller is responsible for deleting it.
 */
static cJSON* process_context_keys_recursive(const cJSON* source_json, const cJSON* context) {
    if (!source_json) {
        return NULL;
    }

    if (cJSON_IsObject(source_json)) {
        cJSON* new_obj = cJSON_CreateObject();
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, source_json) {
            const char* original_key = item->string;
            const char* final_key = original_key;

            if (original_key && original_key[0] == '$') {
                const char* var_name = original_key + 1;
                cJSON* context_val_item = cJSON_GetObjectItem(context, var_name);
                if (context_val_item && cJSON_IsString(context_val_item)) {
                    final_key = context_val_item->valuestring;
                }
            }

            cJSON* new_value = process_context_keys_recursive(item, context);
            cJSON_AddItemToObject(new_obj, final_key, new_value);
        }
        return new_obj;
    }

    if (cJSON_IsArray(source_json)) {
        cJSON* new_arr = cJSON_CreateArray();
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, source_json) {
            cJSON* new_item = process_context_keys_recursive(item, context);
            cJSON_AddItemToArray(new_arr, new_item);
        }
        return new_arr;
    }

    // For non-container types (string, number, bool, null), just duplicate.
    return cJSON_Duplicate(source_json, true);
}


// --- Core Object Parser ---

static IRObject* parse_object(GenContext* ctx, cJSON* obj_json, const char* parent_c_name, const cJSON* ui_context) {
    if (ctx->error_occurred) return NULL;
    if (!cJSON_IsObject(obj_json)) return NULL;

    const char* original_json_type_str = "obj";
    cJSON* type_item = cJSON_GetObjectItem(obj_json, "type");
    if (type_item && cJSON_IsString(type_item)) {
        original_json_type_str = type_item->valuestring;
    }

    // --- Handle `use-view` directive first, as it's not a real widget type ---
    if (strcmp(original_json_type_str, "use-view") == 0) {
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

        cJSON* new_context = cJSON_CreateObject();
        if (ui_context) merge_json_objects(new_context, ui_context);
        cJSON* local_context = cJSON_GetObjectItem(obj_json, "context");
        if (local_context) merge_json_objects(new_context, local_context);

        // NEW: Process the component template with the context to substitute keys
        cJSON* processed_template = process_context_keys_recursive(component_content, new_context);

        // The final JSON object we will parse. Start with the processed template.
        cJSON* final_json = processed_template;

        // Now, merge/override properties from the use-view block itself onto the processed template
        cJSON* prop_item = NULL;
        cJSON_ArrayForEach(prop_item, obj_json) {
            const char* key = prop_item->string;
            if (strncmp(key, "//", 2) == 0) continue;

            if (strcmp(key, "children") == 0) {
                if (cJSON_IsArray(prop_item)) {
                    cJSON* final_children = cJSON_GetObjectItem(final_json, "children");
                    if (!final_children) {
                        final_children = cJSON_CreateArray();
                        cJSON_AddItemToObject(final_json, "children", final_children);
                    }
                    if (cJSON_IsArray(final_children)) {
                        cJSON* child_to_add;
                        cJSON_ArrayForEach(child_to_add, prop_item) {
                            cJSON_AddItemToArray(final_children, cJSON_Duplicate(child_to_add, true));
                        }
                    }
                }
                continue;
            }

            if (strcmp(key, "type") == 0 || strcmp(key, "id") == 0 || strcmp(key, "context") == 0) continue;

            if (cJSON_HasObjectItem(final_json, key)) {
                cJSON_ReplaceItemInObject(final_json, key, cJSON_Duplicate(prop_item, true));
            } else {
                cJSON_AddItemToObject(final_json, key, cJSON_Duplicate(prop_item, true));
            }
        }

        IRObject* generated_obj = parse_object(ctx, final_json, parent_c_name, new_context);

        cJSON_Delete(final_json);
        cJSON_Delete(new_context);
        return generated_obj;
    }

    // --- It's a regular object. ---
    const WidgetDefinition* widget_def = api_spec_find_widget(ctx->api_spec, original_json_type_str);
    bool fallback_to_obj = false;
    if (!widget_def && strcmp(original_json_type_str, "obj") != 0) {
        fallback_to_obj = true;
        widget_def = api_spec_find_widget(ctx->api_spec, "obj");
        if (!widget_def) {
            render_abort("API Spec is missing the fundamental 'obj' definition.");
            ctx->error_occurred = true;
            return NULL;
        }
    }

    cJSON* new_scope_context = cJSON_CreateObject();
    if(ui_context) merge_json_objects(new_scope_context, ui_context);
    cJSON* local_context = cJSON_GetObjectItem(obj_json, "context");
    if (local_context && cJSON_IsObject(local_context)) merge_json_objects(new_scope_context, local_context);

    cJSON* init_item = cJSON_GetObjectItem(obj_json, "init");
    cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
    if (!id_item) id_item = cJSON_GetObjectItem(obj_json, "name");
    const char* registered_id_from_json = (id_item && cJSON_IsString(id_item)) ? id_item->valuestring : NULL;

    char* c_name = generate_unique_var_name(ctx, registered_id_from_json ? registered_id_from_json : original_json_type_str);
    if (!c_name) { // Error already reported by generate_unique_var_name
        cJSON_Delete(new_scope_context);
        return NULL;
    }

    const char* object_c_type = "lv_obj_t*";
    char derived_c_type[256] = {0};

    if (init_item && cJSON_IsObject(init_item) && init_item->child) {
        object_c_type = api_spec_get_function_return_type(ctx->api_spec, init_item->child->string);
    } else if (widget_def) {
        if (widget_def->create) object_c_type = api_spec_get_function_return_type(ctx->api_spec, widget_def->create);
        else if (widget_def->c_type) {
            bool is_init_object = (widget_def->init_func != NULL && widget_def->create == NULL);
            if (is_init_object && strchr(widget_def->c_type, '*') == NULL) {
                snprintf(derived_c_type, sizeof(derived_c_type), "%s*", widget_def->c_type);
                object_c_type = derived_c_type;
            } else object_c_type = widget_def->c_type;
        }
    }

    const char* clean_id = registered_id_from_json;
    if (clean_id && clean_id[0] == '@') clean_id++;
    IRObject* ir_obj = ir_new_object(c_name, original_json_type_str, object_c_type, clean_id);

#define PARSE_OBJECT_ERROR(msg_format, ...) do { \
    char err_buf[512]; \
    snprintf(err_buf, sizeof(err_buf), msg_format, ##__VA_ARGS__); \
    render_abort(err_buf); \
    ctx->error_occurred = true; \
    free(c_name); \
    cJSON_Delete(new_scope_context); \
    ir_free((IRNode*)ir_obj); \
    return NULL; \
} while (0)

    registry_add_generated_var(ctx->registry, ir_obj->c_name, ir_obj->c_name, ir_obj->c_type);

    if (fallback_to_obj) {
        char warning_msg[256];
        snprintf(warning_msg, sizeof(warning_msg), "Widget type '%s' not found in API spec. Falling back to a generic 'obj'.", original_json_type_str);
        ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_warning(warning_msg));
    }

    if (init_item) {
        if (cJSON_IsObject(init_item) && init_item->child) {
            const char* func_name = init_item->child->string;
            cJSON* user_args_json = init_item->child;

            if (!api_spec_find_function(ctx->api_spec, func_name)) PARSE_OBJECT_ERROR("In 'init' block for '%s', could not find function definition for '%s'.", ir_obj->c_name, func_name);
            const FunctionDefinition* func_def_init = api_spec_find_function(ctx->api_spec, func_name);

            const FunctionArg* first_expected_arg = func_def_init->args_head;
            bool func_expects_target = (first_expected_arg && first_expected_arg->type && strstr(first_expected_arg->type, "_t*"));
            int expected_argc = count_function_args(func_def_init->args_head);
            int user_argc = cJSON_IsArray(user_args_json) ? count_cjson_array(user_args_json) : (cJSON_IsNull(user_args_json) ? 0 : 1);
            bool prepend_target = (func_expects_target && user_argc == expected_argc - 1);
            IRExprNode* final_args = NULL;
            const FunctionArg* expected_arg_list_for_user = func_def_init->args_head;

            if (prepend_target) {
                ir_expr_list_add(&final_args, ir_new_expr_registry_ref(parent_c_name, first_expected_arg->type));
                if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
            }

            if (cJSON_IsArray(user_args_json)) {
                cJSON* val_item = user_args_json->child;
                while(val_item) {
                    const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                    IRExpr* expr = unmarshal_value(ctx, val_item, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj);
                    if (ctx->error_occurred) { ir_free((IRNode*)final_args); PARSE_OBJECT_ERROR("Error processing 'init' arguments for %s", ir_obj->c_name); }
                    ir_expr_list_add(&final_args, expr);
                    if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
                    val_item = val_item->next;
                }
            } else if (!cJSON_IsNull(user_args_json)) {
                const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                IRExpr* expr = unmarshal_value(ctx, user_args_json, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj);
                if (ctx->error_occurred) { PARSE_OBJECT_ERROR("Error processing 'init' argument for %s", ir_obj->c_name); }
                ir_expr_list_add(&final_args, expr);
            }

            process_and_validate_call(ctx, func_name, &final_args, ir_obj);
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
            ir_obj->constructor_expr = ir_new_expr_func_call(func_name, final_args, ret_type);

        } else PARSE_OBJECT_ERROR("The 'init' property for object '%s' must be a map with a single function call.", ir_obj->c_name);
    } else {
        const char* create_func = (widget_def && widget_def->create) ? widget_def->create : (strcmp(original_json_type_str, "obj") == 0 ? "lv_obj_create" : NULL);
        if (create_func) {
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_registry_ref(parent_c_name, "lv_obj_t*"));
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, create_func);
            ir_obj->constructor_expr = ir_new_expr_func_call(create_func, args, ret_type);
            process_and_validate_call(ctx, create_func, &((IRExprFunctionCall*)ir_obj->constructor_expr)->args, ir_obj);
        } else if (widget_def && widget_def->init_func) {
            char sizeof_arg_buf[256];
            char* base_type = get_array_base_type(ir_obj->c_type);
            if(base_type) { snprintf(sizeof_arg_buf, sizeof(sizeof_arg_buf), "sizeof(%s)", base_type); free(base_type); }
            else snprintf(sizeof_arg_buf, sizeof(sizeof_arg_buf), "0 /* Error: could not get base type for %s */", ir_obj->c_type);

            IRExprNode* malloc_args = NULL;
            ir_expr_list_add(&malloc_args, ir_new_expr_literal(sizeof_arg_buf, "size_t"));
            ir_obj->constructor_expr = ir_new_expr_func_call("malloc", malloc_args, ir_obj->c_type);
            IRExprNode* init_args = NULL;
            ir_expr_list_add(&init_args, ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type));
            IRExpr* init_call = ir_new_expr_func_call(widget_def->init_func, init_args, "void");
            process_and_validate_call(ctx, widget_def->init_func, &init_args, ir_obj);
            ir_operation_list_add(&ir_obj->operations, (IRNode*)init_call);
        }
    }

    if (ctx->error_occurred) { free(c_name); cJSON_Delete(new_scope_context); ir_free((IRNode*)ir_obj); return NULL; }

    if (registered_id_from_json) {
        registry_add_generated_var(ctx->registry, registered_id_from_json, ir_obj->c_name, ir_obj->c_type);
        const char* id_for_runtime = (registered_id_from_json[0] == '@') ? registered_id_from_json + 1 : registered_id_from_json;
        IRExpr* obj_ref_expr = ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type);
        IRExpr* reg_call_expr = ir_new_expr_runtime_reg_add(id_for_runtime, obj_ref_expr);
        ir_operation_list_add(&ir_obj->operations, (IRNode*)reg_call_expr);
    }

    for(cJSON* item = obj_json->child; item && !ctx->error_occurred; item = item->next) {
        const char* key = item->string;
        if (strncmp(key, "//", 2) == 0 || strcmp(key, "type") == 0 || strcmp(key, "init") == 0 ||
            strcmp(key, "id") == 0 || strcmp(key, "name") == 0 || strcmp(key, "context") == 0) continue;

        if (strcmp(key, "children") == 0) {
            if (cJSON_IsArray(item)) {
                cJSON* child_json = NULL;
                cJSON_ArrayForEach(child_json, item) {
                    if (ctx->error_occurred) break;
                    IRObject* child_obj = parse_object(ctx, child_json, ir_obj->c_name, new_scope_context);
                    if(ctx->error_occurred) break;
                    if(child_obj) ir_operation_list_add(&ir_obj->operations, (IRNode*)child_obj);
                }
            }
        } else if (strcmp(key, "observes") == 0) {
            if (cJSON_IsObject(item)) {
                cJSON* state_item;
                cJSON_ArrayForEach(state_item, item) {
                    const char* state_name = state_item->string;
                    // Observe value can be a string or an object. Let's handle both.
                    cJSON *bindings_obj = state_item;
                    if (cJSON_IsString(state_item)) {
                        bindings_obj = cJSON_CreateObject();
                        cJSON_AddItemToObject(bindings_obj, state_item->valuestring, cJSON_CreateNull());
                    } else if (!cJSON_IsObject(state_item)) {
                        print_warning("Value for observable '%s' must be an object or a string.", state_name);
                        continue;
                    }


                    cJSON* binding_item;
                    cJSON_ArrayForEach(binding_item, bindings_obj) {
                        const char* binding_key = binding_item->string;
                        observer_update_type_t update_type;

                        if (strcmp(binding_key, "text") == 0) update_type = OBSERVER_TYPE_TEXT;
                        else if (strcmp(binding_key, "style") == 0) update_type = OBSERVER_TYPE_STYLE;
                        else if (strcmp(binding_key, "visible") == 0) update_type = OBSERVER_TYPE_VISIBLE;
                        else if (strcmp(binding_key, "checked") == 0) update_type = OBSERVER_TYPE_CHECKED;
                        else if (strcmp(binding_key, "disabled") == 0) update_type = OBSERVER_TYPE_DISABLED;
                        else if (strcmp(binding_key, "value") == 0) update_type = OBSERVER_TYPE_VALUE;
                        else {
                            print_warning("Unknown binding type '%s' for observable '%s'.", binding_key, state_name);
                            continue;
                        }

                        IRExpr* config_expr = unmarshal_value(ctx, binding_item, new_scope_context, "unknown", parent_c_name, ir_obj->c_name, ir_obj);
                        ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_observer(state_name, update_type, config_expr));
                    }

                    if (cJSON_IsString(state_item)) {
                        cJSON_Delete(bindings_obj);
                    }
                }
            }
        } else if (strcmp(key, "action") == 0) {
            if (cJSON_IsObject(item)) {
                cJSON* act_item;
                cJSON_ArrayForEach(act_item, item) {
                    const char* action_name = act_item->string;
                    action_type_t action_type = ACTION_TYPE_TRIGGER; // Default
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
                    } else if (cJSON_IsObject(act_item)) {
                        cJSON* dialog_config = cJSON_GetObjectItemCaseSensitive(act_item, "numeric_input_dialog");
                        if (dialog_config) {
                            action_type = ACTION_TYPE_NUMERIC_DIALOG;
                            data_expr = unmarshal_value(ctx, dialog_config, new_scope_context, "void*", parent_c_name, ir_obj->c_name, ir_obj);
                        } else {
                            print_warning("Unsupported object-based action config for action '%s'.", action_name);
                            continue;
                        }
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
                api_spec_free_property(prop_def);
                continue;
            }
            if (!api_spec_find_function(ctx->api_spec, func_name)) {
                char warning_msg[256];
                snprintf(warning_msg, sizeof(warning_msg), "Could not find function definition for '%s'.", func_name);
                ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_warning(warning_msg));
                api_spec_free_property(prop_def);
                continue;
            }
            const FunctionDefinition* func_def = api_spec_find_function(ctx->api_spec, func_name);
            const FunctionArg* first_expected_arg = func_def->args_head;
            bool func_expects_target = (first_expected_arg && first_expected_arg->type && strstr(first_expected_arg->type, "_t*"));
            IRExprNode* final_args = NULL;
            const FunctionArg* expected_arg_list_for_user = func_def->args_head;

            if (func_expects_target) {
                ir_expr_list_add(&final_args, ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type));
                if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
            }

            if (cJSON_IsArray(item)) {
                cJSON* val_item = item->child;
                while(val_item) {
                    const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                    IRExpr* expr = unmarshal_value(ctx, val_item, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj);
                    if (ctx->error_occurred) { ir_free((IRNode*)final_args); PARSE_OBJECT_ERROR("Error processing arguments for '%s' on %s", func_name, ir_obj->c_name); }
                    ir_expr_list_add(&final_args, expr);
                    if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
                    val_item = val_item->next;
                }
            } else {
                 const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                 IRExpr* expr = unmarshal_value(ctx, item, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj);
                 if (ctx->error_occurred) { ir_free((IRNode*)final_args); PARSE_OBJECT_ERROR("Error processing argument for '%s' on %s", func_name, ir_obj->c_name); }
                 ir_expr_list_add(&final_args, expr);
            }

            process_and_validate_call(ctx, func_name, &final_args, ir_obj);
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
            ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_expr_func_call(func_name, final_args, ret_type));
            api_spec_free_property(prop_def);
        }
    }

    if (ctx->error_occurred) { free(c_name); cJSON_Delete(new_scope_context); ir_free((IRNode*)ir_obj); return NULL; }

    free(c_name);
    cJSON_Delete(new_scope_context);

    return ir_obj;
}


// --- Value Unmarshaler ---

static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* expected_c_type, const char* parent_c_name, const char* target_c_name, IRObject* ir_obj_for_warnings) {
    if (ctx->error_occurred) return NULL;
    if (!value || cJSON_IsNull(value)) return ir_new_expr_literal("NULL", "void*");

    if (cJSON_IsString(value)) {
        const char* s = value->valuestring;

        // @_target refers to the parent object passed to the constructor.
        if (strcmp(s, "@_target") == 0) {
            if (parent_c_name) {
                const char* parent_type = registry_get_c_type_for_id(ctx->registry, parent_c_name);
                return ir_new_expr_registry_ref(parent_c_name, parent_type ? parent_type : "lv_obj_t*");
            }
        }
        // @_parent or @self refers to the object currently being defined.
        if (strcmp(s, "@_parent") == 0 || strcmp(s, "@self") == 0) {
            if (target_c_name) {
                const char* target_type = registry_get_c_type_for_id(ctx->registry, target_c_name);
                return ir_new_expr_registry_ref(target_c_name, target_type ? target_type : "lv_obj_t*");
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
            // Fall through to treat as a literal string if not found
        }
        if (strchr(s, '|')) {
            long final_val = 0;
            char* temp_str = strdup(s);
            if (!temp_str) { render_abort("Failed to duplicate string for OR-parsing"); ctx->error_occurred = true; return NULL; }
            char* token = strtok(temp_str, "|");
            bool error = false;
            while (token != NULL) {
                char* trimmed_token = trim_whitespace(token);
                if (strlen(trimmed_token) > 0) {
                    long part_val = 0;
                    if (api_spec_find_enum_value(ctx->api_spec, expected_c_type, trimmed_token, &part_val) ||
                        api_spec_find_enum_value(ctx->api_spec, api_spec_find_global_enum_type(ctx->api_spec, trimmed_token), trimmed_token, &part_val) ||
                        api_spec_find_constant_value(ctx->api_spec, trimmed_token, &part_val)) {
                        final_val |= part_val;
                    } else {
                        if (ir_obj_for_warnings) print_warning("Could not resolve part '%s' of OR-expression '%s'", trimmed_token, s);
                        error = true; break;
                    }
                }
                token = strtok(NULL, "|");
            }
            free(temp_str);
            if (!error) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%ld", final_val);
                return ir_new_expr_literal(buf, (expected_c_type && strcmp(expected_c_type, "unknown") != 0) ? expected_c_type : "float");
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
            return ir_new_expr_literal(buf, "float");
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
            temp_s[len - 1] = '\0'; // remove the '%'

            char* trimmed_num_part = trim_whitespace(temp_s);
            char* endptr;
            strtol(trimmed_num_part, &endptr, 10); // Try to parse it as an integer

            if (*endptr == '\0' && endptr != trimmed_num_part) {
                IRExprNode* args = NULL;
                ir_expr_list_add(&args, ir_new_expr_literal(trimmed_num_part, "int32_t"));
                free(temp_s);
                return ir_new_expr_func_call("lv_pct", args, "lv_coord_t");
            }
            free(temp_s);
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
        char buf[32]; snprintf(buf, sizeof(buf), "%g", value->valuedouble);
        return ir_new_expr_literal(buf, "float");
    }
    if (cJSON_IsBool(value)) return ir_new_expr_literal(cJSON_IsTrue(value) ? "true" : "false", "bool");
    if (cJSON_IsArray(value)) {
        char* base_type = get_array_base_type(expected_c_type);
        IRExprNode* elements = NULL;
        cJSON* elem_json;
        cJSON_ArrayForEach(elem_json, value) {
            IRExpr* elem_expr = unmarshal_value(ctx, elem_json, ui_context, base_type, parent_c_name, target_c_name, ir_obj_for_warnings);
            if (ctx->error_occurred) { ir_free((IRNode*)elements); free(base_type); return NULL; }
            ir_expr_list_add(&elements, elem_expr);
        }
        free(base_type);
        return ir_new_expr_array(elements, (char*)expected_c_type);
    }
    if (cJSON_IsObject(value)) {
        cJSON* func_item = value->child;
        if (func_item && func_item->next == NULL && api_spec_has_function(ctx->api_spec, func_item->string)) {
            const char* func_name = func_item->string;
            const FunctionDefinition* func_def = api_spec_find_function(ctx->api_spec, func_name);
            IRExprNode* args_list = NULL;
            const FunctionArg* expected_args = func_def ? func_def->args_head : NULL;
            if (cJSON_IsArray(func_item)) {
                cJSON* arg_item;
                cJSON_ArrayForEach(arg_item, func_item) {
                    const char* expected_type = expected_args ? expected_args->type : "unknown";
                    IRExpr* expr = unmarshal_value(ctx, arg_item, ui_context, expected_type, parent_c_name, target_c_name, ir_obj_for_warnings);
                    if (ctx->error_occurred) { ir_free((IRNode*)args_list); return NULL; }
                    ir_expr_list_add(&args_list, expr);
                    if (expected_args) expected_args = expected_args->next;
                }
            } else if (!cJSON_IsNull(func_item)) {
                const char* expected_type = expected_args ? expected_args->type : "unknown";
                IRExpr* expr = unmarshal_value(ctx, func_item, ui_context, expected_type, parent_c_name, target_c_name, ir_obj_for_warnings);
                if (ctx->error_occurred) { return NULL; }
                ir_expr_list_add(&args_list, expr);
            }
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
            return ir_new_expr_func_call(func_name, args_list, ret_type);
        }
        IRExprNode* map_elements = NULL;
        cJSON* map_item;
        cJSON_ArrayForEach(map_item, value) {
            IRExprNode* pair_elements = NULL;
            char* key_str = map_item->string;
            if (strcmp(key_str, "true") == 0 || strcmp(key_str, "false") == 0) {
                 ir_expr_list_add(&pair_elements, ir_new_expr_literal(key_str, "bool"));
            } else {
                 char* endptr;
                 strtod(key_str, &endptr);
                 if (*endptr == '\0') {
                    ir_expr_list_add(&pair_elements, ir_new_expr_literal(key_str, "float"));
                 } else {
                    ir_expr_list_add(&pair_elements, ir_new_expr_literal_string(key_str, strlen(key_str)));
                 }
            }

            ir_expr_list_add(&pair_elements, unmarshal_value(ctx, map_item, ui_context, "unknown", parent_c_name, target_c_name, ir_obj_for_warnings));

            ir_expr_list_add(&map_elements, ir_new_expr_array(pair_elements, "void*[]"));
        }
        return ir_new_expr_array(map_elements, "void*[]");

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

    if ((strcmp(expected, "const char*") == 0 && strcmp(actual, "char*") == 0) ||
        (strcmp(expected, "char*") == 0 && strcmp(actual, "const char*") == 0)) return true;

    if (strcmp(expected, "lv_style_t*") == 0 && strcmp(actual, "lv_style_t") == 0) return true;

    const char* num_types[] = {"int", "int32_t", "uint32_t", "lv_coord_t", "lv_style_selector_t", "lv_opa_t", "bool", "lv_anim_enable_t", "float"};
    int num_num_types = sizeof(num_types) / sizeof(char*);
    bool expected_is_num = false;
    bool actual_is_num = false;
    for(int i=0; i < num_num_types; i++) {
        if(strcmp(expected, num_types[i]) == 0) expected_is_num = true;
        if(strcmp(actual, num_types[i]) == 0) actual_is_num = true;
    }
    if (expected_is_num && actual_is_num) return true;

    if (strcmp(expected, "void*") == 0 && strchr(actual, '*') != NULL) return true;

    return false;
}

static void process_and_validate_call(GenContext* ctx, const char* func_name, IRExprNode** args_list_ptr, IRObject* ir_obj_for_warnings) {
    const FunctionDefinition* func_def = api_spec_find_function(ctx->api_spec, func_name);
    if (!func_def) {
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

    bool func_expects_target = (func_def->args_head && func_def->args_head->type && strstr(func_def->args_head->type, "_t*"));
    int user_provided_argc = func_expects_target ? (actual_argc - 1) : actual_argc;

    if (expected_argc > 0 && func_expects_target && (expected_argc - 1 == 0) && user_provided_argc > 0) {
        IRExprNode* current = (*args_list_ptr)->next;
        (*args_list_ptr)->next = NULL;

        while (current) {
            IRExprNode* temp = current->next;
            ir_free((IRNode*)current->expr);
            free(current);
            current = temp;
        }
        actual_argc = 1;
    }

    if (strncmp(func_name, "lv_obj_set_style_", 17) == 0 && actual_argc == expected_argc - 1) {
        if (last_expected_arg && strcmp(last_expected_arg->type, "lv_style_selector_t") == 0) {
            ir_expr_list_add(args_list_ptr, ir_new_expr_literal("0", "lv_style_selector_t"));
            actual_argc++;
        }
    }

    if (actual_argc != expected_argc) {
        if (g_strict_mode) {
             char err_buf[256];
             snprintf(err_buf, sizeof(err_buf), "Strict mode failure: Argument count mismatch for '%s'. Expected %d, got %d.", func_name, expected_argc, actual_argc);
             render_abort(err_buf);
             ctx->error_occurred = true;
        } else if (ir_obj_for_warnings) {
            char warning_msg[256];
            snprintf(warning_msg, sizeof(warning_msg), "Argument count mismatch for function '%s'. Expected %d, but %d were provided.", func_name, expected_argc, actual_argc);
            ir_operation_list_add(&ir_obj_for_warnings->operations, (IRNode*)ir_new_warning(warning_msg));
        }
        return;
    }

    IRExprNode* actual_arg_node = *args_list_ptr;
    const FunctionArg* expected_arg = func_def->args_head;
    while (actual_arg_node && expected_arg) {
        if(expected_arg->type && strcmp(expected_arg->type, "void") == 0) {
             expected_arg = expected_arg->next;
             continue;
        }
        actual_arg_node = actual_arg_node->next;
        expected_arg = expected_arg->next;
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
    if (!temp_sanitized) {
        render_abort("Failed to allocate memory for sanitized name.");
        ctx->error_occurred = true;
        return NULL;
    }
    strncpy(sanitized_base, temp_sanitized, sizeof(sanitized_base) - 1);
    sanitized_base[sizeof(sanitized_base) - 1] = '\0';
    free(temp_sanitized);

    char* final_name = malloc(strlen(sanitized_base) + 16);
    if (!final_name) {
        render_abort("Failed to allocate memory for variable name.");
        ctx->error_occurred = true;
        return NULL;
    }
    snprintf(final_name, strlen(sanitized_base) + 16, "%s_%d", sanitized_base, ctx->var_counter++);
    return final_name;
}

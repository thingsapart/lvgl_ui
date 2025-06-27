#include "lvgl_renderer.h"
#include "c_gen/lvgl_dispatch.h" // For dynamic_lvgl_call_ir and obj_registry_*
#include "debug_log.h"
#include "utils.h"
#include <stdlib.h>

// --- Forward Declarations ---
static void render_object_list(ApiSpec* spec, IRObject* head);
static void* execute_expression(ApiSpec* spec, IRExpr* expr);

// --- Main Backend Entry Point ---

void lvgl_render_backend(IRRoot* root, ApiSpec* api_spec, lv_obj_t* parent) {
    if (!root || !api_spec || !parent) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Error: lvgl_render_backend called with NULL arguments.");
        return;
    }

    obj_registry_init();
    // Add the top-level parent widget to the registry so it can be referenced as "parent"
    obj_registry_add("parent", parent);

    DEBUG_LOG(LOG_MODULE_RENDERER, "Starting LVGL render backend.");
    render_object_list(api_spec, root->root_objects);
    DEBUG_LOG(LOG_MODULE_RENDERER, "LVGL render backend finished.");

    // The caller is responsible for deinit if they want the registry cleared.
    // obj_registry_deinit();
}

// --- Core Rendering Logic ---

static void render_object_list(ApiSpec* spec, IRObject* head) {
    for (IRObject* current_obj = head; current_obj; current_obj = current_obj->next) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Rendering object: c_name='%s', json_type='%s'", current_obj->c_name, current_obj->json_type);

        // 1. Create the object by executing its constructor expression
        void* c_obj = execute_expression(spec, current_obj->constructor_expr);

        // Handle non-pointer types like lv_style_t which must be heap-allocated for runtime use.
        if (!c_obj && current_obj->c_type && strchr(current_obj->c_type, '*') == NULL) {
            DEBUG_LOG(LOG_MODULE_RENDERER, "Allocating heap memory for non-pointer type '%s'", current_obj->c_type);
            if (strcmp(current_obj->c_type, "lv_style_t") == 0) {
                c_obj = malloc(sizeof(lv_style_t));
                if (!c_obj) render_abort("Failed to malloc lv_style_t");
            }
             // NOTE: Add other non-pointer types here if they become necessary
        }

        if (!c_obj && current_obj->constructor_expr) {
            DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: Constructor for '%s' returned NULL.", current_obj->c_name);
        }

        // Add the created C object to the runtime registry using its generated C name as the key.
        // This allows setup calls to find their target.
        obj_registry_add(current_obj->c_name, c_obj);

        // 2. Execute all setup calls for this object.
        // This includes the obj_registry_add call for the user-defined 'id'.
        for (IRExprNode* call_node = current_obj->setup_calls; call_node; call_node = call_node->next) {
            execute_expression(spec, call_node->expr);
        }

        // 3. Recursively render child objects
        if (current_obj->children) {
            render_object_list(spec, current_obj->children);
        }
    }
}

static void* execute_expression(ApiSpec* spec, IRExpr* expr) {
    if (!expr) return NULL;

    switch (expr->base.type) {
        case IR_EXPR_RUNTIME_REG_ADD: {
            IRExprRuntimeRegAdd* reg_add = (IRExprRuntimeRegAdd*)expr;
            // The object to register is given as a reference to its C-name.
            IRExprRegistryRef* obj_ref = (IRExprRegistryRef*)reg_add->object_expr;
            void* c_obj = obj_registry_get(obj_ref->name);
            if (c_obj) {
                obj_registry_add(reg_add->id, c_obj);
                DEBUG_LOG(LOG_MODULE_REGISTRY, "Runtime-registered ID '%s' to pointer %p", reg_add->id, c_obj);
            } else {
                DEBUG_LOG(LOG_MODULE_REGISTRY, "Error: Could not find object C-name '%s' to register with ID '%s'", obj_ref->name, reg_add->id);
            }
            return NULL; // This expression does not return a value
        }

        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;

            // 1. Count arguments and prepare IRNode** array for the dispatcher.
            int arg_count = 0;
            for (IRExprNode* n = call->args; n; n = n->next) arg_count++;

            IRNode** ir_args = arg_count > 0 ? calloc(arg_count, sizeof(IRNode*)) : NULL;
            if (arg_count > 0 && !ir_args) render_abort("Failed to allocate ir_args array");

            int i = 0;
            for (IRExprNode* n = call->args; n; n = n->next) {
                ir_args[i++] = (IRNode*)n->expr;
            }

            // 2. Determine the target object and the final arguments for the dispatcher.
            void* target_obj = NULL;
            IRNode** final_ir_args = NULL;
            int final_arg_count = 0;

            const FunctionArg* f_args = api_spec_get_function_args_by_name(spec, call->func_name);
            bool first_arg_is_target = f_args && f_args->type && (strstr(f_args->type, "_t*") != NULL);

            if (first_arg_is_target && arg_count > 0) {
                // The first argument is the target object. It MUST be a registry reference.
                if (ir_args[0]->type == IR_EXPR_REGISTRY_REF) {
                    IRExprRegistryRef* target_ref = (IRExprRegistryRef*)ir_args[0];
                    const char* target_id = target_ref->name;
                     // The ID could be a user-defined one ('@id') or a generated c_name ('label_0').
                    if (target_id[0] == '@') target_id++; // Strip prefix for lookup.
                    target_obj = obj_registry_get(target_id);
                } else {
                    DEBUG_LOG(LOG_MODULE_RENDERER, "Error: First argument to '%s' is not a registry ref, cannot determine target.", call->func_name);
                }
                final_ir_args = (arg_count > 1) ? &ir_args[1] : NULL;
                final_arg_count = arg_count - 1;
            } else {
                // No target object (e.g., lv_tick_inc())
                target_obj = NULL;
                final_ir_args = ir_args;
                final_arg_count = arg_count;
            }

            DEBUG_LOG(LOG_MODULE_DISPATCH, "Dispatching: %s", call->func_name);

            // 3. Dispatch the call via the generated function
            void* result = dynamic_lvgl_call_ir(call->func_name, target_obj, final_ir_args, final_arg_count, spec);

            // 4. Cleanup
            free(ir_args);

            return result;
        }

        default:
            // Other expressions like literals, enums, etc., are not directly executable.
            // They only have meaning as arguments to a function call, which are handled
            // by the dispatcher. This case should not be hit in a valid IR tree.
            DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: execute_expression called on non-executable node type %d", expr->base.type);
            return NULL;
    }
}

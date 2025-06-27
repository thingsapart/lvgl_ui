#include "lvgl_renderer.h"
#include "c_gen/lvgl_dispatch.h"
#include "debug_log.h"
#include "registry.h"
#include "utils.h"
#include <stdlib.h>


// --- Forward Declarations ---
static void render_object_list(ApiSpec* spec, IRObject* head, Registry* registry);
static void evaluate_expression(ApiSpec* spec, Registry* registry, IRExpr* expr, RenderValue* out_val);


// --- Main Backend Entry Point ---

void lvgl_render_backend(IRRoot* root, ApiSpec* api_spec, lv_obj_t* parent) {
    if (!root || !api_spec || !parent) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Error: lvgl_render_backend called with NULL arguments.");
        return;
    }

    // Use a single registry for the entire rendering process.
    Registry* registry = registry_create();
    obj_registry_init(); // Also init the C-side registry.

    // Add the top-level parent widget to the registry so it can be referenced as "parent"
    registry_add_pointer(registry, parent, "parent", "obj", "lv_obj_t*");
    obj_registry_add("parent", parent);

    DEBUG_LOG(LOG_MODULE_RENDERER, "Starting LVGL render backend.");
    render_object_list(api_spec, root->root_objects, registry);
    DEBUG_LOG(LOG_MODULE_RENDERER, "LVGL render backend finished.");

    registry_free(registry);
    // The C-side registry should be de-inited by the caller (e.g., main) after the loop ends.
}

// --- Core Rendering Logic ---

static void render_object_list(ApiSpec* spec, IRObject* head, Registry* registry) {
    for (IRObject* current_obj = head; current_obj; current_obj = current_obj->next) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Rendering object: c_name='%s', json_type='%s'", current_obj->c_name, current_obj->json_type);

        RenderValue constructor_result;
        constructor_result.type = RENDER_VAL_TYPE_NULL;
        constructor_result.as.p_val = NULL;

        void* c_obj = NULL;

        // 1. Create the object by executing its constructor expression
        if (current_obj->constructor_expr) {
            evaluate_expression(spec, registry, current_obj->constructor_expr, &constructor_result);
            if(constructor_result.type == RENDER_VAL_TYPE_POINTER) {
                c_obj = constructor_result.as.p_val;
            }
        }

        // Handle non-pointer types like lv_style_t which are stack-allocated by the C-backend
        // but must be heap-allocated for the dynamic renderer.
        if (!c_obj && current_obj->c_type && strchr(current_obj->c_type, '*') == NULL) {
            DEBUG_LOG(LOG_MODULE_RENDERER, "Allocating heap memory for non-pointer type '%s'", current_obj->c_type);
            if (strcmp(current_obj->c_type, "lv_style_t") == 0) {
                c_obj = malloc(sizeof(lv_style_t));
                if (!c_obj) render_abort("Failed to malloc lv_style_t");
            }
        }

        if (!c_obj && current_obj->constructor_expr) {
            DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: Constructor for '%s' returned NULL.", current_obj->c_name);
        }

        // Register the newly created object so it can be found by subsequent calls.
        registry_add_pointer(registry, c_obj, current_obj->c_name, current_obj->json_type, current_obj->c_type);
        if (current_obj->registered_id) {
             registry_add_pointer(registry, c_obj, current_obj->registered_id, current_obj->json_type, current_obj->c_type);
             obj_registry_add(current_obj->registered_id, c_obj);
             DEBUG_LOG(LOG_MODULE_REGISTRY, "Registered ID '%s' to pointer %p", current_obj->registered_id, c_obj);
        }

        // 2. Execute all setup calls for this object.
        for (IRExprNode* call_node = current_obj->setup_calls; call_node; call_node = call_node->next) {
            RenderValue ignored_result;
            evaluate_expression(spec, registry, call_node->expr, &ignored_result);
        }

        // 3. Recursively render child objects
        if (current_obj->children) {
            render_object_list(spec, current_obj->children, registry);
        }
    }
}

// --- Recursive Expression Evaluator ---

static void evaluate_expression(ApiSpec* spec, Registry* registry, IRExpr* expr, RenderValue* out_val) {
    if (!expr) {
        out_val->type = RENDER_VAL_TYPE_NULL;
        out_val->as.p_val = NULL;
        return;
    }

    switch (expr->base.type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)expr;
            if (lit->is_string) {
                out_val->type = RENDER_VAL_TYPE_STRING;
                out_val->as.s_val = lit->value;
            } else {
                if(strcmp(lit->value, "true") == 0) {
                    out_val->type = RENDER_VAL_TYPE_BOOL;
                    out_val->as.b_val = true;
                } else if (strcmp(lit->value, "false") == 0) {
                    out_val->type = RENDER_VAL_TYPE_BOOL;
                    out_val->as.b_val = false;
                } else {
                    out_val->type = RENDER_VAL_TYPE_INT;
                    out_val->as.i_val = strtol(lit->value, NULL, 0);
                }
            }
            return;
        }

        case IR_EXPR_ENUM: {
            // The dispatcher's helpers need the enum's C type, so we must pass the symbol.
            // We pass the symbol as a string and let the dispatcher resolve it.
            out_val->type = RENDER_VAL_TYPE_STRING;
            out_val->as.s_val = ((IRExprEnum*)expr)->symbol;
            return;
        }

        case IR_EXPR_REGISTRY_REF: {
            const char* name = ((IRExprRegistryRef*)expr)->name;
            out_val->type = RENDER_VAL_TYPE_POINTER;
            out_val->as.p_val = registry_get_pointer(registry, name, NULL);
            if (!out_val->as.p_val) {
                 DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: Registry reference '%s' resolved to NULL.", name);
            }
            return;
        }

        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;
            DEBUG_LOG(LOG_MODULE_DISPATCH, "Evaluating call: %s", call->func_name);

            // --- General-purpose dynamic dispatch ---

            // 1. Evaluate all arguments into concrete values.
            int arg_count = 0;
            for (IRExprNode* n = call->args; n; n = n->next) arg_count++;

            RenderValue* evaluated_args = arg_count > 0 ? calloc(arg_count, sizeof(RenderValue)) : NULL;
            if (arg_count > 0 && !evaluated_args) render_abort("Failed to allocate evaluated_args array");

            int i = 0;
            for (IRExprNode* n = call->args; n; n = n->next) {
                evaluate_expression(spec, registry, n->expr, &evaluated_args[i++]);
            }

            // 2. Repackage the concrete values into temporary IR_EXPR_LITERAL nodes
            //    that `dynamic_lvgl_call_ir` and its helpers can understand.
            IRNode** temp_ir_args = arg_count > 0 ? calloc(arg_count, sizeof(IRNode*)) : NULL;
            if (arg_count > 0 && !temp_ir_args) render_abort("Failed to alloc temp_ir_args");

            IRExprLiteral* literal_nodes = arg_count > 0 ? calloc(arg_count, sizeof(IRExprLiteral)) : NULL;
            if(arg_count > 0 && !literal_nodes) render_abort("Failed to alloc literal_nodes");

            char** str_buffers = calloc(arg_count, sizeof(char*));
            if(arg_count > 0 && !str_buffers) render_abort("Failed to alloc str_buffers");


            for (i = 0; i < arg_count; i++) {
                literal_nodes[i].base.base.type = IR_EXPR_LITERAL;
                temp_ir_args[i] = (IRNode*)&literal_nodes[i];
                str_buffers[i] = malloc(32); // Buffer for converting numbers to strings
                if(!str_buffers[i]) render_abort("Failed to alloc str_buffer");

                switch (evaluated_args[i].type) {
                    case RENDER_VAL_TYPE_INT:
                        literal_nodes[i].is_string = false;
                        snprintf(str_buffers[i], 32, "%ld", (long)evaluated_args[i].as.i_val);
                        literal_nodes[i].value = str_buffers[i];
                        break;
                    case RENDER_VAL_TYPE_BOOL:
                        literal_nodes[i].is_string = false;
                        snprintf(str_buffers[i], 32, "%d", evaluated_args[i].as.b_val);
                        literal_nodes[i].value = str_buffers[i];
                        break;
                    case RENDER_VAL_TYPE_COLOR: // Pass color as its integer representation
                        literal_nodes[i].is_string = false;
                        snprintf(str_buffers[i], 32, "%u", lv_color_to_u32(evaluated_args[i].as.color_val));
                        literal_nodes[i].value = str_buffers[i];
                        break;
                    case RENDER_VAL_TYPE_STRING:
                        literal_nodes[i].is_string = true;
                        literal_nodes[i].value = (char*)evaluated_args[i].as.s_val;
                        break;
                    case RENDER_VAL_TYPE_POINTER:
                    case RENDER_VAL_TYPE_NULL: {
                         if (evaluated_args[i].as.p_val == NULL) {
                            literal_nodes[i].is_string = true;
                            literal_nodes[i].value = "NULL";
                        } else {
                            // It's a pointer to an object. We need to find its ID to pass to the dispatcher.
                            const char* id = registry_get_id_from_pointer(registry, evaluated_args[i].as.p_val);
                            if (id) {
                                literal_nodes[i].is_string = true;
                                literal_nodes[i].value = (char*)id;
                            } else {
                                print_warning("Could not find registry ID for pointer arg %p in call to %s. Passing NULL.", evaluated_args[i].as.p_val, call->func_name);
                                literal_nodes[i].is_string = true;
                                literal_nodes[i].value = "NULL";
                            }
                        }
                        break;
                    }
                }
            }

            // 3. Determine target and final arguments for the dispatcher
            void* target_obj = NULL;
            IRNode** final_ir_args = NULL;
            int final_arg_count = 0;

            const FunctionArg* f_args = api_spec_get_function_args_by_name(spec, call->func_name);
            bool first_arg_is_target = f_args && f_args->type && (strstr(f_args->type, "_t*") != NULL);

            if (first_arg_is_target && arg_count > 0) {
                target_obj = evaluated_args[0].as.p_val;
                final_ir_args = (arg_count > 1) ? &temp_ir_args[1] : NULL;
                final_arg_count = arg_count - 1;
            } else {
                target_obj = NULL;
                final_ir_args = temp_ir_args;
                final_arg_count = arg_count;
            }

            // 4. Dispatch the call
            *out_val = dynamic_lvgl_call_ir(call->func_name, target_obj, final_ir_args, final_arg_count, spec);

            // 5. Cleanup
            free(evaluated_args);
            for(i=0; i<arg_count; i++) free(str_buffers[i]);
            free(str_buffers);
            free(literal_nodes);
            free(temp_ir_args);
            return;
        }

        default:
            DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: evaluate_expression called on un-evaluatable node type %d", expr->base.type);
            out_val->type = RENDER_VAL_TYPE_NULL;
            out_val->as.p_val = NULL;
            return;
    }
}

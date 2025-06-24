#include "lvgl_ui_renderer.h"
#include "lvgl_dispatch.h"
#include "ir.h"
#include "utils.h"
#include "api_spec.h" // For function definition lookups
#include "debug_log.h" // For DEBUG_LOG

#include <stdio.h>
#include <string.h>
#include <stdlib.h> // For malloc/free

#define MAX_RENDER_ARGS 16 // Maximum arguments for a function call

// Helper to determine if a function is a method call and expects an object instance as its first param
// This is a simplified check. A more robust check would involve detailed API spec parsing.
bool is_method_like(const char* func_name, struct ApiSpec* api_spec) {
    if (!api_spec || !func_name) return false;

    // First, check global functions (common case for creators, style init)
    FunctionMapNode* func_node = api_spec->functions;
    while (func_node) {
        if (strcmp(func_node->name, func_name) == 0) {
            // It's a known global function.
            // If it's something like lv_style_init, it's not a "method" in the typical sense of obj->method().
            // However, many global LVGL functions take lv_obj_t* as the first argument.
            // For simplicity, if the first argument is lv_obj_t* or similar, dynamic_lvgl_call_ir
            // will expect the first IRNode arg to resolve to that object.
            // Let dynamic_lvgl_call_ir handle this by attempting to resolve the first arg if it's a variable.
            return false; // Let's assume global functions are not "methods" for target_obj extraction here.
                          // dynamic_lvgl_call_ir will handle resolving the first arg if it's an object.
        }
        func_node = func_node->next;
    }

    // If not in global functions, check widget methods
    WidgetMapNode* widget_node = api_spec->widgets_list_head;
    while (widget_node) {
        if (widget_node->widget && widget_node->widget->methods) {
            FunctionMapNode* method_node = widget_node->widget->methods;
            while (method_node) {
                if (strcmp(method_node->name, func_name) == 0) {
                    return true; // Found in a widget's method list
                }
                method_node = method_node->next;
            }
        }
        widget_node = widget_node->next;
    }
    // Could also check style methods if api_spec had them structured this way.
    // For now, style functions like lv_style_set_... are treated as global functions
    // where the first argument is the style object.

    // Default: if not explicitly found as a method, assume it's a global function or creator.
    // dynamic_lvgl_call_ir will try to resolve the first argument if it's a variable.
    DEBUG_LOG(LOG_MODULE_RENDERER, "is_method_like: Function '%s' not found as a distinct method in api_spec. Assuming global or target is first arg.", func_name);
    return false; // Let dynamic_lvgl_call_ir figure it out based on the first argument.
}


void render_lvgl_ui_from_ir(IRStmtBlock* ir_block, lv_obj_t* parent_screen, struct ApiSpec* api_spec) {
    DEBUG_LOG(LOG_MODULE_RENDERER, "Entering render_lvgl_ui_from_ir. Parent screen: %p", (void*)parent_screen);
    if (!ir_block || !parent_screen) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Error: IR block or parent screen is NULL.");
        return;
    }
    if (ir_block->base.type != IR_STMT_BLOCK) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Error: Expected IR_STMT_BLOCK, got %d", ir_block->base.type);
        return;
    }
     if (!api_spec) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: ApiSpec is NULL. Method call resolution might be impaired.");
    }

    obj_registry_init(); // Ensure registry is ready
    DEBUG_LOG(LOG_MODULE_RENDERER, "Object registry initialized.");
    obj_registry_add("parent", parent_screen); // Register the main screen
    DEBUG_LOG(LOG_MODULE_RENDERER, "Registered main screen as 'parent' (%p).", (void*)parent_screen);

    IRStmtNode* current_stmt_list_node = ir_block->stmts;
    int stmt_idx = 0;

    while (current_stmt_list_node != NULL) {
        IRNode* stmt_node_base = (IRNode*)current_stmt_list_node->stmt;
        DEBUG_LOG(LOG_MODULE_RENDERER, "Processing statement index %d, node %p, type %d", stmt_idx, (void*)stmt_node_base, stmt_node_base ? stmt_node_base->type : -1);

        if (!stmt_node_base) {
            DEBUG_LOG(LOG_MODULE_RENDERER, "Statement %d is NULL.", stmt_idx);
            stmt_idx++;
            current_stmt_list_node = current_stmt_list_node->next;
            continue;
        }

        const char* func_name = NULL;
        void* target_obj = NULL; // For actual method calls where target is implicit
        IRNode* ir_args[MAX_RENDER_ARGS];
        int arg_count = 0;
        const char* id_to_set = NULL; // For objects created that need registration
        IRNode* temp_parent_node_for_freeing = NULL; // To manage temporary IRNode for "parent"

        if (stmt_node_base->type == IR_STMT_WIDGET_ALLOCATE) {
            IRStmtWidgetAllocate* stmt = (IRStmtWidgetAllocate*)stmt_node_base;
            func_name = stmt->create_func_name;
            id_to_set = stmt->c_var_name;
            target_obj = NULL; // Create functions don't have a target obj in this sense

            if (stmt->parent_expr) {
                ir_args[arg_count++] = (IRNode*)stmt->parent_expr;
            } else {
                // Default to "parent" screen if no parent_expr specified
                // This creates a temporary IRNode that we must manage.
                temp_parent_node_for_freeing = (IRNode*)ir_new_variable("parent");
                if (temp_parent_node_for_freeing) {
                    ir_args[arg_count++] = temp_parent_node_for_freeing;
                } else {
                    DEBUG_LOG(LOG_MODULE_RENDERER, "FATAL: Failed to allocate temporary parent IRNode for %s.", func_name);
                    // Error handling: skip this statement or abort?
                    stmt_idx++;
                    current_stmt_list_node = current_stmt_list_node->next;
                    continue;
                }
            }
            DEBUG_LOG(LOG_MODULE_RENDERER, "IR_STMT_WIDGET_ALLOCATE: func='%s', id_to_set='%s'", func_name, id_to_set);

        } else if (stmt_node_base->type == IR_STMT_VAR_DECL) {
            IRStmtVarDecl* stmt = (IRStmtVarDecl*)stmt_node_base;
            DEBUG_LOG(LOG_MODULE_RENDERER, "IR_STMT_VAR_DECL: var_name='%s', type_name='%s'", stmt->var_name, stmt->type_name);
            if (stmt->initializer && stmt->initializer->type == IR_EXPR_FUNC_CALL) {
                IRExprFuncCall* initializer_func_call = (IRExprFuncCall*)stmt->initializer;
                func_name = initializer_func_call->func_name;
                id_to_set = stmt->var_name; // The result of the func call will be assigned to this variable
                target_obj = NULL; // Initializer functions usually don't have a target object

                DEBUG_LOG(LOG_MODULE_RENDERER, "Initializer is FUNC_CALL: func='%s', result_id_to_set='%s'", func_name, id_to_set);

                IRExprNode* current_arg_node = initializer_func_call->args;
                while (current_arg_node != NULL && arg_count < MAX_RENDER_ARGS) {
                    if (current_arg_node->expr) {
                        ir_args[arg_count++] = (IRNode*)current_arg_node->expr;
                    }
                    current_arg_node = current_arg_node->next;
                }
                // Note: No target_obj extraction for initializer function calls, assuming they are global/static.
            } else if (stmt->initializer) {
                DEBUG_LOG(LOG_MODULE_RENDERER, "IR_STMT_VAR_DECL for '%s' has an initializer of type %d, not a function call. Value not directly used for rendering.", stmt->var_name, stmt->initializer->type);
                // For lvgl_ui mode, if a var is declared with a literal, it might not directly translate to an LVGL action
                // unless it's used later. For now, we only act on function call initializers that create objects.
                // No func_name means this will skip the dispatch logic later in the loop.
            } else {
                 DEBUG_LOG(LOG_MODULE_RENDERER, "IR_STMT_VAR_DECL for '%s' has no initializer.", stmt->var_name);
            }
             // If func_name got set (from initializer_func_call), it will be dispatched.
             // Otherwise, this IR_STMT_VAR_DECL (e.g. with literal init) is processed, but no dispatch occurs.

        } else if (stmt_node_base->type == IR_STMT_OBJECT_ALLOCATE) {
            IRStmtObjectAllocate* stmt = (IRStmtObjectAllocate*)stmt_node_base;
            DEBUG_LOG(LOG_MODULE_RENDERER, "IR_STMT_OBJECT_ALLOCATE: type '%s', id '%s', init_func '%s'",
                     stmt->object_c_type_name, stmt->c_var_name, stmt->init_func_name);

            if (strcmp(stmt->object_c_type_name, "lv_style_t") == 0) {
                lv_style_t* style_obj = (lv_style_t*)malloc(sizeof(lv_style_t));
                if (!style_obj) {
                    DEBUG_LOG(LOG_MODULE_RENDERER, "FATAL: Failed to malloc lv_style_t for '%s'", stmt->c_var_name);
                    stmt_idx++;
                    current_stmt_list_node = current_stmt_list_node->next;
                    continue;
                }

                if (stmt->init_func_name && strcmp(stmt->init_func_name, "lv_style_init") == 0) {
                    lv_style_init(style_obj); // Direct call
                    DEBUG_LOG(LOG_MODULE_RENDERER, "Initialized style '%s' at %p with lv_style_init.", stmt->c_var_name, (void*)style_obj);
                } else if (stmt->init_func_name) {
                     DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: Unhandled init_func_name '%s' for lv_style_t. Style memory allocated but not initialized by this function.", stmt->init_func_name);
                } else {
                     DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: No init_func_name for lv_style_t. Style memory allocated but not initialized.");
                }

                obj_registry_add(stmt->c_var_name, style_obj);
                DEBUG_LOG(LOG_MODULE_RENDERER, "Registered allocated style '%s' (%p).", stmt->c_var_name, (void*)style_obj);
            } else {
                DEBUG_LOG(LOG_MODULE_RENDERER, "Unhandled IR_STMT_OBJECT_ALLOCATE for C type '%s'. No action taken.", stmt->object_c_type_name);
            }
            // This statement type (as handled for styles) does not directly result in a dynamic_lvgl_call_ir.
            stmt_idx++;
            current_stmt_list_node = current_stmt_list_node->next;
            continue;

        } else if (stmt_node_base->type == IR_STMT_FUNC_CALL) {
            IRStmtFuncCall* stmt = (IRStmtFuncCall*)stmt_node_base;
            if (!stmt->call || !stmt->call->func_name) {
                DEBUG_LOG(LOG_MODULE_RENDERER, "IR_STMT_FUNC_CALL (idx %d) missing call or func_name.", stmt_idx);
                stmt_idx++;
                current_stmt_list_node = current_stmt_list_node->next;
                continue;
            }
            func_name = stmt->call->func_name;
            DEBUG_LOG(LOG_MODULE_RENDERER, "IR_STMT_FUNC_CALL: func='%s'", func_name);

            IRExprNode* current_arg_node = stmt->call->args;
            while (current_arg_node != NULL && arg_count < MAX_RENDER_ARGS) {
                if (current_arg_node->expr) {
                    ir_args[arg_count++] = (IRNode*)current_arg_node->expr;
                }
                current_arg_node = current_arg_node->next;
            }

            bool method_like = is_method_like(func_name, api_spec);
            if (method_like && arg_count > 0) {
                IRNode* first_arg_node = ir_args[0];
                if (first_arg_node->type == IR_EXPR_VARIABLE) {
                    IRExprVariable* var_node = (IRExprVariable*)first_arg_node;
                    target_obj = obj_registry_get(var_node->name);
                    if (!target_obj) {
                        DEBUG_LOG(LOG_MODULE_RENDERER, "Error: Method call to '%s': target object ID '%s' not found in registry.", func_name, var_node->name);
                        if (temp_parent_node_for_freeing) { // Clean up if we allocated it this iteration
                            ir_free(temp_parent_node_for_freeing);
                            temp_parent_node_for_freeing = NULL;
                        }
                        stmt_idx++;
                        current_stmt_list_node = current_stmt_list_node->next;
                        continue;
                    }
                    DEBUG_LOG(LOG_MODULE_RENDERER, "Method call: Extracted target_obj '%s' (%p) for func '%s'.", var_node->name, target_obj, func_name);
                    for (int i = 0; i < arg_count - 1; i++) {
                        ir_args[i] = ir_args[i+1];
                    }
                    arg_count--;
                } else {
                    DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: First argument for supposed method '%s' is not an IR_EXPR_VARIABLE. Treating as global call.", func_name);
                    target_obj = NULL;
                }
            } else {
                 DEBUG_LOG(LOG_MODULE_RENDERER, "Not a method call or no args for func '%s'. target_obj is NULL.", func_name);
                target_obj = NULL;
            }

        } else {
            DEBUG_LOG(LOG_MODULE_RENDERER, "Skipping unhandled statement type %d at index %d.", stmt_node_base->type, stmt_idx);
            if (temp_parent_node_for_freeing) { // Clean up if allocated and statement skipped
                ir_free(temp_parent_node_for_freeing);
                temp_parent_node_for_freeing = NULL;
            }
            stmt_idx++;
            current_stmt_list_node = current_stmt_list_node->next;
            continue;
        }

        if (func_name) {
            DEBUG_LOG(LOG_MODULE_RENDERER, "Dispatching to dynamic_lvgl_call_ir: func='%s', target_obj=%p, arg_count=%d",
                     func_name, target_obj, arg_count);
            for(int i=0; i<arg_count; ++i) {
                if (ir_args[i]) {
                    if(ir_args[i]->type == IR_EXPR_LITERAL) {
                        DEBUG_LOG(LOG_MODULE_RENDERER, "  Arg %d: type=LITERAL, val_str=%s", i, ((IRExprLiteral*)ir_args[i])->value);
                    } else if (ir_args[i]->type == IR_EXPR_VARIABLE) {
                        DEBUG_LOG(LOG_MODULE_RENDERER, "  Arg %d: type=VARIABLE, var_name=%s", i, ((IRExprVariable*)ir_args[i])->name);
                    } else {
                        DEBUG_LOG(LOG_MODULE_RENDERER, "  Arg %d: type=%d (complex)", i, ir_args[i]->type);
                    }
                } else {
                     DEBUG_LOG(LOG_MODULE_RENDERER, "  Arg %d: NULL", i);
                }
            }

            void* created_obj_ptr = dynamic_lvgl_call_ir(func_name, target_obj, ir_args, arg_count);

            if (id_to_set && created_obj_ptr) {
                obj_registry_add(id_to_set, created_obj_ptr);
                DEBUG_LOG(LOG_MODULE_RENDERER, "Registered object from dispatch: ID '%s' (%p).", id_to_set, created_obj_ptr);
            } else if (id_to_set && !created_obj_ptr) {
                DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: func '%s' was expected to create object for ID '%s', but returned NULL.", func_name, id_to_set);
            }
        } else {
            DEBUG_LOG(LOG_MODULE_RENDERER, "No func_name to dispatch for statement index %d.", stmt_idx);
        }

        if (temp_parent_node_for_freeing) {
            ir_free(temp_parent_node_for_freeing);
            temp_parent_node_for_freeing = NULL;
        }

        stmt_idx++;
        current_stmt_list_node = current_stmt_list_node->next;
    }

    // obj_registry_deinit(); // Deinit is typically handled by the caller that initialized.
    DEBUG_LOG(LOG_MODULE_RENDERER, "Finished render_lvgl_ui_from_ir.");
}

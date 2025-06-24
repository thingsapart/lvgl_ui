#include "lvgl_ui_renderer.h"
#include "lvgl_dispatch.h"
#include "ir.h"
#include "utils.h"
#include "api_spec.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h> // For malloc/free if we need to build arg arrays

#define MAX_ARGS 16 // Maximum arguments for a function call, adjust as needed

// Corrected function definition to match header
void render_lvgl_ui_from_ir(IRStmtBlock* ir_block, lv_obj_t* parent_screen, struct ApiSpec* api_spec) {
    _dprintf(stderr, "[RENDERER] Entering render_lvgl_ui_from_ir. Parent screen: %p\n", (void*)parent_screen);
    if (!ir_block || !parent_screen) {
        _dprintf(stderr, "[RENDERER] Error: IR block or parent screen is NULL.\n");
        return;
    }
    if (ir_block->base.type != IR_STMT_BLOCK) {
        _dprintf(stderr, "[RENDERER] Error: Expected IR_STMT_BLOCK, got %d\n", ir_block->base.type);
        return;
    }
    if (!api_spec) {
        _dprintf(stderr, "[RENDERER] Warning: ApiSpec is NULL. Method call resolution might be impaired.\n");
    }

    obj_registry_init();
    _dprintf(stderr, "[RENDERER] Object registry initialized.\n");
    obj_registry_add("parent", parent_screen);
    _dprintf(stderr, "[RENDERER] Registered main screen as 'parent' (%p).\n", (void*)parent_screen);

    IRStmtNode* current_stmt_list_node = ir_block->stmts;
    int stmt_idx = 0;
    while (current_stmt_list_node != NULL) {
        IRNode* stmt_node = (IRNode*)current_stmt_list_node->stmt;
        _dprintf(stderr, "[RENDERER] Processing statement index %d, node %p\n", stmt_idx, (void*)stmt_node);

        if (!stmt_node) {
            _dprintf(stderr, "[RENDERER] Statement %d is NULL.\n", stmt_idx);
            goto next_statement;
        }
        _dprintf(stderr, "[RENDERER] Statement %d type: %d\n", stmt_idx, stmt_node->type);

        const char* func_name_to_dispatch = NULL;
        void* target_obj_for_dispatch = NULL;
        IRNode* prepared_args_for_dispatch[MAX_ARGS];
        int arg_count_for_dispatch = 0;
        const char* id_for_created_object = NULL;
        void* object_to_register_manually = NULL; // For objects like styles not returned by a dispatchable func

        // Reset for each statement
        for(int i=0; i<MAX_ARGS; ++i) prepared_args_for_dispatch[i] = NULL;


        if (stmt_node->type == IR_STMT_OBJECT_ALLOCATE) {
            IRStmtObjectAllocate* stmt_obj_alloc = (IRStmtObjectAllocate*)stmt_node;
            _dprintf(stderr, "[RENDERER] IR_STMT_OBJECT_ALLOCATE: type '%s', id '%s', init_func '%s'\n",
                     stmt_obj_alloc->object_c_type_name, stmt_obj_alloc->c_var_name, stmt_obj_alloc->init_func_name);

            if (strcmp(stmt_obj_alloc->object_c_type_name, "lv_style_t") == 0 &&
                strcmp(stmt_obj_alloc->init_func_name, "lv_style_init") == 0) {

                lv_style_t* style_obj = (lv_style_t*)malloc(sizeof(lv_style_t));
                if (style_obj) {
                    lv_style_init(style_obj); // Direct LVGL call
                    id_for_created_object = stmt_obj_alloc->c_var_name;
                    object_to_register_manually = style_obj;
                    _dprintf(stderr, "[RENDERER] Allocated and initialized lv_style_t for ID '%s' at %p\n", id_for_created_object, object_to_register_manually);
                } else {
                    _dprintf(stderr, "[RENDERER] Error: Failed to malloc for lv_style_t with ID '%s'\n", stmt_obj_alloc->c_var_name);
                }
            } else {
                _dprintf(stderr, "[RENDERER] Unhandled IR_STMT_OBJECT_ALLOCATE for C type '%s'\n", stmt_obj_alloc->object_c_type_name);
            }
            // This IR statement type does not directly result in a dynamic_lvgl_call_ir itself.
            // Properties will be set by subsequent IR_STMT_FUNC_CALL.
            goto register_and_continue;
        }
        else if (stmt_node->type == IR_STMT_WIDGET_ALLOCATE) {
            IRStmtWidgetAllocate* stmt_widget_alloc = (IRStmtWidgetAllocate*)stmt_node;
            if (!stmt_widget_alloc->create_func_name || !stmt_widget_alloc->c_var_name) {
                _dprintf(stderr, "[RENDERER] IR_STMT_WIDGET_ALLOCATE (idx %d) missing create_func_name or c_var_name.\n", stmt_idx);
                goto next_statement;
            }
            func_name_to_dispatch = stmt_widget_alloc->create_func_name;
            id_for_created_object = stmt_widget_alloc->c_var_name;
            _dprintf(stderr, "[RENDERER] IR_STMT_WIDGET_ALLOCATE: func='%s', id_to_set='%s'\n", func_name_to_dispatch, id_for_created_object);

            if (stmt_widget_alloc->parent_expr && arg_count_for_dispatch < MAX_ARGS) {
                prepared_args_for_dispatch[arg_count_for_dispatch++] = (IRNode*)stmt_widget_alloc->parent_expr;
            } else if (!stmt_widget_alloc->parent_expr && arg_count_for_dispatch < MAX_ARGS) {
                IRExpr* parent_var_expr = ir_new_variable("parent");
                prepared_args_for_dispatch[arg_count_for_dispatch++] = (IRNode*)parent_var_expr;
                 _dprintf(stderr, "[RENDERER] Using 'parent' variable for widget allocation.\n");
            }
        }
        else if (stmt_node->type == IR_STMT_FUNC_CALL) {
            IRStmtFuncCall* stmt_func_call = (IRStmtFuncCall*)stmt_node;
            if (!stmt_func_call->call || !stmt_func_call->call->func_name) {
                 _dprintf(stderr, "[RENDERER] IR_STMT_FUNC_CALL (idx %d) missing call or func_name.\n", stmt_idx);
                 goto next_statement;
            }
            func_name_to_dispatch = stmt_func_call->call->func_name;
            _dprintf(stderr, "[RENDERER] IR_STMT_FUNC_CALL: func='%s'\n", func_name_to_dispatch);

            IRExprNode* current_arg_node = stmt_func_call->call->args;
            while (current_arg_node != NULL && arg_count_for_dispatch < MAX_ARGS) {
                if (current_arg_node->expr) {
                    prepared_args_for_dispatch[arg_count_for_dispatch++] = (IRNode*)current_arg_node->expr;
                } else {
                     _dprintf(stderr, "[RENDERER] Warning: NULL expr in arg list for %s\n", func_name_to_dispatch);
                }
                current_arg_node = current_arg_node->next;
            }
        }
        else {
            _dprintf(stderr, "[RENDERER] Skipping unhandled statement type %d at index %d.\n", stmt_node->type, stmt_idx);
            goto next_statement;
        }

        if (!func_name_to_dispatch) { // Should only happen if logic error above or unhandled path
            _dprintf(stderr, "[RENDERER] No function to dispatch for stmt index %d.\n", stmt_idx);
            goto register_and_continue; // Still try to register if object_to_register_manually was set
        }

        // --- Resolve function definition and target_obj for func_name_to_dispatch ---
        FunctionDefinition* func_def = NULL;
        bool is_method_call = false;
        if (api_spec) {
            FunctionMapNode* global_func_node = api_spec->functions;
            while (global_func_node) {
                if (global_func_node->name && strcmp(global_func_node->name, func_name_to_dispatch) == 0) {
                    func_def = global_func_node->func_def; is_method_call = false; break;
                }
                global_func_node = global_func_node->next;
            }
            if (!func_def) {
                WidgetMapNode* widget_list_node = api_spec->widgets_list_head;
                while (widget_list_node) {
                    WidgetDefinition* widget_def = widget_list_node->widget;
                    if (widget_def && widget_def->methods) {
                        FunctionMapNode* method_node = widget_def->methods;
                        while (method_node) {
                            if (method_node->name && strcmp(method_node->name, func_name_to_dispatch) == 0) {
                                func_def = method_node->func_def; is_method_call = true; goto found_func_def_dispatch;
                            }
                            method_node = method_node->next;
                        }
                    }
                    widget_list_node = widget_list_node->next;
                }
            }
        }
        found_func_def:; // Label for goto

        // Basic check if func_def is NULL (function not found in API spec)
        if (!func_def) {
             _dprintf(stderr, "Warning: Function/Method '%s' not found in API spec. Assuming global function or error.\n", func_name);
             is_method_call = false; // Can't be a method if we don't know its class
        } else {
            // Refined check for is_method_call based on func_def properties if needed,
            // but the search logic above already sets it.
            // For example, some global functions might take lv_obj_t* as first arg but are not "methods"
            // in the sense of belonging to a class's method list in api_spec.json.
            // The current is_method_call is true if found in a widget's method list.
        }


        if (is_method_call && arg_count > 0) {
            IRNode* first_arg_ir_node = prepared_args[0];
            // Assumption: first_arg_ir_node is an IRExprVariable if it's an ID.
            if (first_arg_ir_node && first_arg_ir_node->type == IR_EXPR_VARIABLE) {
                IRExprVariable* var_node = (IRExprVariable*)first_arg_ir_node;
                target_obj = obj_registry_get(var_node->name);
                if (!target_obj) {
                    _dprintf(stderr, "Error: Method call to %s failed. Target object ID '%s' not found.\n", func_name, var_node->name);
                    current_stmt_list_node = current_stmt_list_node->next;
                    if (stmt_node->type == IR_STMT_WIDGET_ALLOCATE && prepared_args[0]->type == IR_EXPR_VARIABLE) {
                        // If we created this ir_new_variable("parent"), we should free it if not used.
                        // However, dynamic_lvgl_call_ir is expected to handle/free these.
                        // For now, this is tricky. Let's assume they are not heap allocated if ir_new_variable returns a static node or similar.
                        // Best to rely on a proper IR free mechanism later.
                        // For the temporary "parent" node, if ir_new_variable heap allocates, this is a leak.
                        // ir_free((IRNode*)prepared_args[0]); // This is risky if not allocated
                    }
                    continue;
                }
                // Shift arguments
                for (int k = 0; k < arg_count - 1; ++k) {
                    prepared_args[k] = prepared_args[k+1];
                }
                arg_count--;
                 // Free the IRNode for "parent" if we allocated it and it was consumed.
                if (stmt_node->type == IR_STMT_WIDGET_ALLOCATE && first_arg_ir_node->type == IR_EXPR_VARIABLE && strcmp(((IRExprVariable*)first_arg_ir_node)->name, "parent")==0) {
                    // ir_free(first_arg_ir_node); // This should be done IF ir_new_variable allocates and dynamic_lvgl_call_ir doesn't free.
                }

            } else {
                 _dprintf(stderr, "Warning: First argument for method %s is not a variable (ID). Assuming global call or error.\n", func_name);
                 target_obj = NULL; // Treat as global call
            }
        }

        _dprintf(stderr, "Dispatching: %s (Target: %p, Args: %d)\n", func_name, target_obj, arg_count);
        for(int k=0; k < arg_count; ++k) {
            if (prepared_args[k]) {
                 // Note: IRNode doesn't have str_val or int_val directly.
                 // Need to cast to IRExprLiteral and access ->value for literals.
                 // Or rely on dynamic_lvgl_call_ir to interpret these IRNode* types.
                 if(prepared_args[k]->type == IR_EXPR_LITERAL) {
                    _dprintf(stderr, "  Arg %d: type=%d, val_str=%s\n", k, prepared_args[k]->type, ((IRExprLiteral*)prepared_args[k])->value);
                 } else if (prepared_args[k]->type == IR_EXPR_VARIABLE) {
                    _dprintf(stderr, "  Arg %d: type=%d, var_name=%s\n", k, prepared_args[k]->type, ((IRExprVariable*)prepared_args[k])->name);
                 } else {
                    _dprintf(stderr, "  Arg %d: type=%d (complex)\n", k, prepared_args[k]->type);
                 }
            } else {
                 _dprintf(stderr, "  Arg %d: NULL\n", k);
            }
        }

        lv_obj_t* created_obj = dynamic_lvgl_call_ir(func_name, target_obj, prepared_args, arg_count);

        if (id_to_set && created_obj) {
            obj_registry_add(id_to_set, created_obj);
            _dprintf(stderr, "Registered object with ID '%s' (%p) from widget allocation.\n", id_to_set, created_obj);
        }

        // Clean up the temporary "parent" IRNode if it was created for widget allocation
        if (stmt_node->type == IR_STMT_WIDGET_ALLOCATE) {
            IRStmtWidgetAllocate* widget_alloc = (IRStmtWidgetAllocate*)stmt_node;
            // If parent_expr was NULL, we created an ir_new_variable("parent").
            // This node needs to be freed if ir_new_variable heap-allocates it.
            // Assuming dynamic_lvgl_call_ir does NOT free the IRNode* it receives.
            if (!widget_alloc->parent_expr) {
                // The node passed was prepared_args[0] before potential shift.
                // If target_obj consumed it, it was `first_arg_ir_node`.
                // If not a method call, it's still `prepared_args[0]`.
                // This is tricky. The ir_new_variable("parent") should be freed.
                // For now, I'll assume ir_free handles this if called on the whole block later.
                // A specific free here is risky without knowing ir_new_variable's behavior.
                // Let's find the "parent" node in prepared_args if it was added.
                for(int k=0; k<arg_count; ++k) { // Check current args
                    if(prepared_args[k] && prepared_args[k]->type == IR_EXPR_VARIABLE && strcmp(((IRExprVariable*)prepared_args[k])->name, "parent")==0) {
                        // This is the one we made if parent_expr was NULL.
                        // ir_free(prepared_args[k]); // Problematic if it's shared or not allocated by ir_new_variable.
                        break;
                    }
                }
                 // Also check the first_arg_ir_node if it was consumed by target_obj logic
            }
        }


        current_stmt_list_node = current_stmt_list_node->next;
    }

    obj_registry_deinit();
}

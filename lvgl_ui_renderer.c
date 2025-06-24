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
    if (!ir_block || !parent_screen) {
        _dprintf(stderr, "Error: IR block or parent screen is NULL in render_lvgl_ui_from_ir.\n");
        return;
    }
    if (!api_spec) {
        // This is a critical issue for method resolution.
        // However, dynamic_lvgl_call_ir might still work for global functions if api_spec is NULL.
        _dprintf(stderr, "Warning: ApiSpec is NULL in render_lvgl_ui_from_ir. Method call resolution might be impaired.\n");
    }

    obj_registry_init();
    obj_registry_add("parent", parent_screen);

    IRStmtNode* current_stmt_list_node = ir_block->stmts;
    while (current_stmt_list_node != NULL) {
        IRNode* stmt_node = (IRNode*)current_stmt_list_node->stmt; // IRStmt is typedef IRNode
        if (!stmt_node) {
            current_stmt_list_node = current_stmt_list_node->next;
            continue;
        }

        const char* func_name = NULL;
        void* target_obj = NULL;
        IRNode* prepared_args[MAX_ARGS];
        int arg_count = 0;
        const char* id_to_set = NULL;

        if (stmt_node->type == IR_STMT_FUNC_CALL) {
            IRStmtFuncCall* stmt_func_call = (IRStmtFuncCall*)stmt_node;
            if (!stmt_func_call->call) {
                 _dprintf(stderr, "Warning: IR_STMT_FUNC_CALL has NULL call member.\n");
                 current_stmt_list_node = current_stmt_list_node->next;
                 continue;
            }
            func_name = stmt_func_call->call->func_name;

            IRExprNode* current_arg_node = stmt_func_call->call->args;
            while (current_arg_node != NULL && arg_count < MAX_ARGS) {
                prepared_args[arg_count++] = (IRNode*)current_arg_node->expr;
                current_arg_node = current_arg_node->next;
            }
        } else if (stmt_node->type == IR_STMT_WIDGET_ALLOCATE) {
            IRStmtWidgetAllocate* stmt_widget_alloc = (IRStmtWidgetAllocate*)stmt_node;
            func_name = stmt_widget_alloc->create_func_name;
            id_to_set = stmt_widget_alloc->c_var_name; // ID for the new widget

            // The parent_expr is the first argument to the create function
            if (stmt_widget_alloc->parent_expr && arg_count < MAX_ARGS) {
                prepared_args[arg_count++] = (IRNode*)stmt_widget_alloc->parent_expr;
            } else if (!stmt_widget_alloc->parent_expr && arg_count < MAX_ARGS) {
                // If parent_expr is NULL in IR, it means use lv_scr_act() or current default parent.
                // We need to pass an IRNode representing "parent" (the main screen) or have dynamic_lvgl_call_ir handle NULL.
                // For lv_obj_create(NULL), it's valid. For widget_create(NULL), also valid.
                // Let's create a temporary IRNode that dynamic_lvgl_call_ir can interpret as "use default/active screen"
                // or pass a variable that resolves to parent_screen.
                // Simplest: if parent_expr is NULL, it's like passing lv_scr_act().
                // dynamic_lvgl_call_ir might need to know that a NULL IRNode* means lv_scr_act() for parent args.
                // Or, we use the "parent" variable we registered.
                prepared_args[arg_count++] = (IRNode*)ir_new_variable("parent"); // This will be looked up by dynamic_lvgl_call_ir
            }
        } else {
            // Other statement types not handled for rendering (e.g., comments)
            current_stmt_list_node = current_stmt_list_node->next;
            continue;
        }

        if (!func_name) {
            current_stmt_list_node = current_stmt_list_node->next;
            continue;
        }

        FunctionDefinition* func_def = NULL;
        bool is_method_call = false;
        // const char* associated_widget_type = NULL; // To store the type if it's a method

        if (api_spec) {
            // 1. Search global functions
            FunctionMapNode* global_func_node = api_spec->functions;
            while (global_func_node) {
                if (global_func_node->name && strcmp(global_func_node->name, func_name) == 0) {
                    func_def = global_func_node->func_def;
                    is_method_call = false; // Global functions are not methods on specific instances here
                    break;
                }
                global_func_node = global_func_node->next;
            }

            // 2. If not found globally, search widget methods
            if (!func_def) {
                WidgetMapNode* widget_list_node = api_spec->widgets_list_head;
                while (widget_list_node) {
                    WidgetDefinition* widget_def = widget_list_node->widget;
                    if (widget_def && widget_def->methods) {
                        FunctionMapNode* method_node = widget_def->methods;
                        while (method_node) {
                            if (method_node->name && strcmp(method_node->name, func_name) == 0) {
                                func_def = method_node->func_def;
                                is_method_call = true;
                                // associated_widget_type = widget_def->name; // or widget_def->c_type
                                goto found_func_def; // Exit outer loop once method is found
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

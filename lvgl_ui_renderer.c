#include "lvgl_ui_renderer.h"
#include "lvgl_dispatch.h"
#include "ir.h"
#include "utils.h"
#include "api_spec.h"
#include "debug_log.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_RENDER_ARGS 16
#define CONTEXT_PARENT_ID "__context_parent"

// Forward declaration for the recursive helper
static void render_ir_block_recursive(IRStmtBlock* ir_block, lv_obj_t* current_parent, struct ApiSpec* api_spec);

void render_lvgl_ui_from_ir(IRStmtBlock* ir_block, lv_obj_t* parent_screen, struct ApiSpec* api_spec) {
    if (!ir_block || !parent_screen || !api_spec) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Error: render_lvgl_ui_from_ir called with NULL arguments.");
        return;
    }
    if (ir_block->base.type != IR_STMT_BLOCK) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Error: Root IR node is not a block.");
        return;
    }

    DEBUG_LOG(LOG_MODULE_RENDERER, "Starting LVGL UI rendering from IR.");
    obj_registry_init();

    // The name "parent" is a convention from the generator for the root object.
    obj_registry_add("parent", parent_screen);
    DEBUG_LOG(LOG_MODULE_RENDERER, "Registered initial screen %p as 'parent'.", (void*)parent_screen);

    render_ir_block_recursive(ir_block, parent_screen, api_spec);

    DEBUG_LOG(LOG_MODULE_RENDERER, "Finished LVGL UI rendering.");
    // Note: obj_registry_deinit() is not called here, as the created objects are live
    // on the screen. It should be called during application shutdown.
}

static void render_ir_block_recursive(IRStmtBlock* ir_block, lv_obj_t* current_parent, struct ApiSpec* api_spec) {
    IRStmtNode* current_stmt_node = ir_block->stmts;
    lv_obj_t* last_created_widget_in_scope = NULL; // Tracks the last widget created in this block
    int stmt_idx = 0;

    // Set the contextual parent for this scope using a special, temporary ID.
    // This allows widget creation to use this parent if no explicit parent is given.
    obj_registry_add(CONTEXT_PARENT_ID, current_parent);

    while (current_stmt_node) {
        IRNode* stmt_base = (IRNode*)current_stmt_node->stmt;
        stmt_idx++;
        if (!stmt_base) {
            current_stmt_node = current_stmt_node->next;
            continue;
        }

        DEBUG_LOG(LOG_MODULE_RENDERER, "Block (%p), ParentCtx (%p): Stmt %d, Type %d", (void*)ir_block, (void*)current_parent, stmt_idx, stmt_base->type);

        lv_obj_t* new_obj_this_iter = NULL;

        switch (stmt_base->type) {
            case IR_STMT_WIDGET_ALLOCATE: {
                IRStmtWidgetAllocate* stmt = (IRStmtWidgetAllocate*)stmt_base;
                DEBUG_LOG(LOG_MODULE_RENDERER, "  WIDGET_ALLOC: var='%s', func='%s'", stmt->c_var_name, stmt->create_func_name);

                lv_obj_t* parent_obj = NULL;

                if (stmt->parent_expr) {
                    if (stmt->parent_expr->type == IR_EXPR_VARIABLE) {
                        const char* parent_id = ir_node_get_string((IRNode*)stmt->parent_expr);
                        parent_obj = obj_registry_get(parent_id);
                        DEBUG_LOG(LOG_MODULE_RENDERER, "  Resolving explicit parent '%s' to %p", parent_id, (void*)parent_obj);
                    } else {
                        DEBUG_LOG(LOG_MODULE_RENDERER, "  Warning: Unhandled parent expression type %d for widget '%s'", stmt->parent_expr->type, stmt->c_var_name);
                    }
                } else {
                    DEBUG_LOG(LOG_MODULE_RENDERER, "  No explicit parent for '%s'. Using contextual parent %p.", stmt->c_var_name, (void*)current_parent);
                    parent_obj = current_parent;
                }

                if (!parent_obj) {
                    DEBUG_LOG(LOG_MODULE_RENDERER, "  Error: Could not resolve parent for widget '%s'. Aborting creation.", stmt->c_var_name);
                    break;
                }

                // Call the create function. The parent is passed as the `target_obj`.
                // Create functions typically have no other arguments, so args are NULL and count is 0.
                new_obj_this_iter = dynamic_lvgl_call_ir(stmt->create_func_name, parent_obj, NULL, 0);

                if (new_obj_this_iter) {
                    obj_registry_add(stmt->c_var_name, new_obj_this_iter);
                    DEBUG_LOG(LOG_MODULE_RENDERER, "  Created and registered widget '%s' at %p.", stmt->c_var_name, new_obj_this_iter);
                } else {
                    DEBUG_LOG(LOG_MODULE_RENDERER, "  Error: Widget creation for '%s' returned NULL.", stmt->c_var_name);
                }
                break;
            }

            case IR_STMT_OBJECT_ALLOCATE: {
                IRStmtObjectAllocate* stmt = (IRStmtObjectAllocate*)stmt_base;
                DEBUG_LOG(LOG_MODULE_RENDERER, "  OBJECT_ALLOC: var='%s', type='%s', init='%s'", stmt->c_var_name, stmt->object_c_type_name, stmt->init_func_name);

                void* new_obj = NULL;
                if (strcmp(stmt->object_c_type_name, "lv_style_t") == 0) {
                    new_obj = malloc(sizeof(lv_style_t));
                    if (new_obj) lv_style_init(new_obj);
                } else if (strcmp(stmt->object_c_type_name, "lv_anim_t") == 0) {
                    new_obj = malloc(sizeof(lv_anim_t));
                    if (new_obj) lv_anim_init(new_obj);
                }

                if (new_obj) {
                    obj_registry_add(stmt->c_var_name, new_obj);
                    DEBUG_LOG(LOG_MODULE_RENDERER, "  Allocated and registered object '%s' at %p.", stmt->c_var_name, new_obj);
                } else {
                    DEBUG_LOG(LOG_MODULE_RENDERER, "  Error: Failed to allocate/handle object type '%s'.", stmt->object_c_type_name);
                }
                break;
            }

            case IR_STMT_FUNC_CALL:
            case IR_STMT_VAR_DECL: {
                IRExprFuncCall* call = NULL;
                const char* var_to_assign = NULL;

                if (stmt_base->type == IR_STMT_FUNC_CALL) {
                    call = ((IRStmtFuncCall*)stmt_base)->call;
                } else { // IR_STMT_VAR_DECL
                    IRStmtVarDecl* decl_stmt = (IRStmtVarDecl*)stmt_base;
                    if (decl_stmt->initializer && decl_stmt->initializer->type == IR_EXPR_FUNC_CALL) {
                        call = (IRExprFuncCall*)decl_stmt->initializer;
                        var_to_assign = decl_stmt->var_name;
                    }
                }
                if (!call || !call->func_name) break;

                const char* func_name = call->func_name;
                DEBUG_LOG(LOG_MODULE_RENDERER, "  FUNC_CALL: func='%s'%s%s", func_name, var_to_assign ? ", assign_to='" : "", var_to_assign ? var_to_assign : "");

                void* target_obj = NULL;
                IRNode* ir_args[MAX_RENDER_ARGS];
                int arg_count = 0;

                IRExprNode* current_arg_node = call->args;
                while(current_arg_node && arg_count < MAX_RENDER_ARGS) {
                    ir_args[arg_count++] = (IRNode*)current_arg_node->expr;
                    current_arg_node = current_arg_node->next;
                }

                if (arg_count > 0 && ir_args[0]->type == IR_EXPR_VARIABLE) {
                    const char* target_id = ((IRExprVariable*)ir_args[0])->name;
                    void* potential_target = obj_registry_get(target_id);
                    if (potential_target) {
                        DEBUG_LOG(LOG_MODULE_RENDERER, "  Identified target object '%s' (%p).", target_id, potential_target);
                        target_obj = potential_target;
                        for(int i = 0; i < arg_count - 1; i++) ir_args[i] = ir_args[i+1];
                        arg_count--;
                    }
                }

                void* result = dynamic_lvgl_call_ir(func_name, target_obj, ir_args, arg_count);
                if (var_to_assign) {
                    if (result) {
                        obj_registry_add(var_to_assign, result);
                        new_obj_this_iter = result; // Track if it created a widget
                        DEBUG_LOG(LOG_MODULE_RENDERER, "  Assigned and registered '%s' at %p", var_to_assign, result);
                    } else {
                        DEBUG_LOG(LOG_MODULE_RENDERER, "  Warning: Func call for var '%s' returned NULL.", var_to_assign);
                    }
                }
                break;
            }

            case IR_STMT_BLOCK: {
                IRStmtBlock* nested_block = (IRStmtBlock*)stmt_base;
                lv_obj_t* parent_for_nested_block = last_created_widget_in_scope ? last_created_widget_in_scope : current_parent;
                DEBUG_LOG(LOG_MODULE_RENDERER, "  NESTED_BLOCK: Recursing with new parent context %p.", (void*)parent_for_nested_block);
                render_ir_block_recursive(nested_block, parent_for_nested_block, api_spec);
                break;
            }

            case IR_STMT_COMMENT: break; // Ignore
            default:
                DEBUG_LOG(LOG_MODULE_RENDERER, "  Warning: Unhandled IR statement type: %d", stmt_base->type);
                break;
        }

        // If a widget was created by this statement, it becomes the context for the next statement (e.g., a child block)
        if (new_obj_this_iter) {
            last_created_widget_in_scope = new_obj_this_iter;
        }

        current_stmt_node = current_stmt_node->next;
    }
}

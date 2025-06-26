#include "lvgl_ui_renderer.h"
#include "lvgl_dispatch.h"
#include "ir.h"
#include "utils.h" // For render_abort and other utils
#include "api_spec.h"
#include "debug_log.h"
#include <cJSON.h>

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define MAX_RENDER_ARGS 16

// --- Render Context ---
// Manages state during rendering, like the object registry and context stack for components.
typedef struct {
    ApiSpec* api_spec;
    IRRoot* ir_root;
    cJSON* context_stack; // An array of context objects for nested use-views
} RenderContext;


// --- Forward Declarations ---
static void render_object(RenderContext* ctx, IRObject* obj, void* parent_obj);
static IRExpr* resolve_expr(RenderContext* ctx, IRExpr* expr);
static void free_resolved_expr_list(IRExprNode* head);

// --- Main Entry Point ---
void render_lvgl_ui_from_ir(IRRoot* ir_root, lv_obj_t* parent_screen, ApiSpec* api_spec, cJSON* initial_context) {
    if (!ir_root || !parent_screen || !api_spec) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Error: render_lvgl_ui_from_ir called with NULL arguments.");
        return;
    }

    DEBUG_LOG(LOG_MODULE_RENDERER, "Starting LVGL UI rendering from new IR.");
    obj_registry_init();
    obj_registry_add("parent", parent_screen);
    DEBUG_LOG(LOG_MODULE_RENDERER, "Registered initial screen %p as 'parent'.", (void*)parent_screen);

    RenderContext ctx = {
        .api_spec = api_spec,
        .ir_root = ir_root,
        .context_stack = cJSON_CreateArray()
    };
    if (initial_context) {
        cJSON_AddItemToArray(ctx.context_stack, cJSON_Duplicate(initial_context, true));
    } else {
        cJSON_AddItemToArray(ctx.context_stack, cJSON_CreateObject());
    }

    IRObject* current_obj = ir_root->root_objects;
    while (current_obj) {
        render_object(&ctx, current_obj, parent_screen);
        current_obj = current_obj->next;
    }

    cJSON_Delete(ctx.context_stack);
    DEBUG_LOG(LOG_MODULE_RENDERER, "Finished LVGL UI rendering.");
}

static void render_properties(RenderContext* ctx, IRObject* ir_obj, void* native_obj) {
    IRProperty* prop = ir_obj->properties;
    while (prop) {
        const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, ir_obj->json_type, prop->name);
        if (!prop_def || !prop_def->setter) {
            DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: Setter for property '%s' on type '%s' not found. Skipping.", prop->name, ir_obj->json_type);
            prop = prop->next;
            continue;
        }

        IRExprNode* resolved_args = NULL;
        if (prop->value->type == IR_EXPR_ARRAY) {
            IRExprArray* arr = (IRExprArray*)prop->value;
            IRExprNode* elem = arr->elements;
            while(elem) {
                ir_expr_list_add(&resolved_args, resolve_expr(ctx, elem->expr));
                elem = elem->next;
            }
        } else {
            ir_expr_list_add(&resolved_args, resolve_expr(ctx, prop->value));
        }

        IRNode* arg_array[MAX_RENDER_ARGS];
        int arg_count = 0;
        IRExprNode* current_arg_node = resolved_args;
        while(current_arg_node && arg_count < MAX_RENDER_ARGS) {
            arg_array[arg_count++] = (IRNode*)current_arg_node->expr;
            current_arg_node = current_arg_node->next;
        }

        // Check for NULL lv_obj_t* or lv_style_t* arguments before calling
        if (prop_def->func_args && prop_def->func_args->next) { // func_args->next is the first actual value argument
            const FunctionArg* current_arg_spec = prop_def->func_args->next;
            for (int i = 0; i < arg_count; ++i) {
                if (current_arg_spec) {
                    const char* arg_type_str = current_arg_spec->type;
                    if (arg_type_str && (strcmp(arg_type_str, "lv_obj_t*") == 0 || strcmp(arg_type_str, "lv_style_t*") == 0)) {
                        const char* arg_name_str = current_arg_spec->name ? current_arg_spec->name : "unnamed_arg";
                        if (arg_array[i] == NULL) { // Case 1: resolve_expr returned NULL (e.g. malloc failure)
                             char err_msg[512];
                             snprintf(err_msg, sizeof(err_msg), "Renderer Error: IRNode for argument '%s' (property '%s' of object '%s') is NULL. Resolution failed.", arg_name_str, prop->name, ir_obj->c_name);
                             render_abort(err_msg);
                        }

                        void* ptr_val = NULL;
                        bool needs_runtime_null_check = true; // Assume we need to check, unless it's an unresolved registry ref

                        if (arg_array[i]->type == IR_EXPR_LITERAL) {
                            IRExprLiteral* lit = (IRExprLiteral*)arg_array[i];
                            // Check for "NULL" string or "0" string for pointers
                            if ((strcmp(lit->value, "NULL") == 0 && !lit->is_string) || strcmp(lit->value, "0") == 0) {
                                ptr_val = NULL;
                            } else {
                                // Attempt to parse if it's a hex address string (e.g. from a function call that returns a pointer)
                                if (lit->value[0] == '0' && (lit->value[1] == 'x' || lit->value[1] == 'X')) {
                                   sscanf(lit->value, "%p", &ptr_val);
                                } else {
                                    // Not "NULL", "0", or "0x...", so it's some other literal.
                                    // It's not a pointer we can directly check for NULL here.
                                    // It might be an integer that LVGL interprets, or an error.
                                    // For lv_obj_t* / lv_style_t*, this state is ambiguous without more context.
                                    // We rely on the dynamic_lvgl_call_ir to correctly cast/use or fail.
                                    // The key is that if it *was* supposed to be a pointer and is some other string,
                                    // it's not an explicit NULL we can catch here easily.
                                    needs_runtime_null_check = false; // Don't check this specific ptr_val for NULL as it's not a clear pointer representation
                                }
                            }
                        } else if (arg_array[i]->type == IR_EXPR_REGISTRY_REF) {
                            // For registry references, the actual obj_registry_get() call (which aborts on failure)
                            // happens inside dynamic_lvgl_call_ir's archetypes.
                            // So, we don't perform a NULL check on ptr_val here for this type.
                            needs_runtime_null_check = false;
                        }
                        // Other IR_EXPR types are not expected to directly represent lv_obj_t* or lv_style_t*
                        // that would be resolvable to NULL here. They would typically be part of a function call
                        // that itself returns a pointer, which would then be in an IR_EXPR_LITERAL.

                        if (needs_runtime_null_check && ptr_val == NULL) {
                            char err_msg[512];
                            snprintf(err_msg, sizeof(err_msg), "Renderer Error: Argument '%s' for function '%s' (property '%s' of object '%s') is NULL.", arg_name_str, prop_def->setter, prop->name, ir_obj->c_name);
                            render_abort(err_msg);
                        }
                    }
                    current_arg_spec = current_arg_spec->next;
                } else {
                    DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: Missing arg spec for arg %d of %s. Cannot perform type-specific NULL check.", i, prop_def->setter);
                }
            }
        }

        dynamic_lvgl_call_ir(prop_def->setter, native_obj, arg_array, arg_count, ctx->api_spec);
        free_resolved_expr_list(resolved_args);

        prop = prop->next;
    }
}

static void render_object(RenderContext* ctx, IRObject* obj, void* parent_obj) {
    if (!obj) return;

    // --- Handle 'use-view' ---
    if (obj->json_type && strcmp(obj->json_type, "use-view") == 0) {
        IRComponent* comp_def = ctx->ir_root->components;
        while(comp_def && strcmp(comp_def->id, obj->use_view_component_id) != 0) {
            comp_def = comp_def->next;
        }
        if (!comp_def) {
            fprintf(stderr, "Renderer Error: Component '%s' not found.\n", obj->use_view_component_id);
            return;
        }

        cJSON* new_context = cJSON_CreateObject();
        IRProperty* ctx_prop = obj->use_view_context;
        while(ctx_prop) {
            IRExpr* resolved_ctx_val = resolve_expr(ctx, ctx_prop->value);
            if (resolved_ctx_val && resolved_ctx_val->type == IR_EXPR_LITERAL) {
                IRExprLiteral* lit = (IRExprLiteral*)resolved_ctx_val;
                if (lit->is_string) cJSON_AddStringToObject(new_context, ctx_prop->name, lit->value);
                else cJSON_AddNumberToObject(new_context, ctx_prop->name, strtod(lit->value, NULL));
            }
            ir_free((IRNode*)resolved_ctx_val);
            ctx_prop = ctx_prop->next;
        }
        cJSON_AddItemToArray(ctx->context_stack, new_context);

        // This is a simplification; a proper implementation would clone and merge IR
        // before rendering. For now, we render the component's root directly and apply
        // overrides from the 'use-view' node's properties.
        render_object(ctx, comp_def->root_widget, parent_obj);

        cJSON_DeleteItemFromArray(ctx->context_stack, cJSON_GetArraySize(ctx->context_stack) - 1);
        return;
    }

    // --- Normal Object Creation ---
    void* created_native_obj = NULL;
    const WidgetDefinition* widget_def = api_spec_find_widget(ctx->api_spec, obj->json_type);

    if (widget_def && widget_def->create) {
        created_native_obj = dynamic_lvgl_call_ir(widget_def->create, parent_obj, NULL, 0, ctx->api_spec);
    } else if (widget_def && widget_def->init_func) {
        if (strcmp(widget_def->c_type, "lv_style_t") == 0) {
            lv_style_t* style = malloc(sizeof(lv_style_t));
            if (style) {
                dynamic_lvgl_call_ir(widget_def->init_func, style, NULL, 0, ctx->api_spec);
                created_native_obj = style;
            }
        }
    } else {
        created_native_obj = dynamic_lvgl_call_ir("lv_obj_create", parent_obj, NULL, 0, ctx->api_spec);
    }

    if (!created_native_obj) {
        char err_msg[256];
        snprintf(err_msg, sizeof(err_msg), "Renderer Error: Failed to create object '%s' (type: %s). dynamic_lvgl_call_ir returned NULL.", obj->c_name, obj->json_type);
        render_abort(err_msg); // This will exit
        return; // Should not be reached
    }

    obj_registry_add(obj->c_name, created_native_obj);
    if (obj->registered_id) {
        obj_registry_add(obj->registered_id, created_native_obj);
    }
    DEBUG_LOG(LOG_MODULE_RENDERER, "Created object '%s' (%s) at %p", obj->c_name, obj->json_type, created_native_obj);

    render_properties(ctx, obj, created_native_obj);

    IRWithBlock* wb = obj->with_blocks;
    while(wb) {
        void* with_block_target_for_do = NULL; // This will be the target for 'do' properties/children

        // Check if this 'with' block uses the "with.obj.call" structure.
        // We assume if target_expr is a function call, it's this new structure.
        if (wb->target_expr && wb->target_expr->type == IR_EXPR_FUNCTION_CALL) {
            IRExprFunctionCall* call_details = (IRExprFunctionCall*)wb->target_expr;

            IRExprNode* resolved_call_args_list = NULL;
            int call_arg_count = 0;
            IRExprNode* current_arg_expr_node = call_details->args;
            while(current_arg_expr_node) {
                ir_expr_list_add(&resolved_call_args_list, resolve_expr(ctx, current_arg_expr_node->expr));
                call_arg_count++;
                current_arg_expr_node = current_arg_expr_node->next;
            }

            IRNode* call_arg_array[MAX_RENDER_ARGS];
            IRExprNode* current_resolved_arg_node = resolved_call_args_list;
            for(int i = 0; i < call_arg_count && i < MAX_RENDER_ARGS; ++i) {
                call_arg_array[i] = (IRNode*)current_resolved_arg_node->expr;
                current_resolved_arg_node = current_resolved_arg_node->next;
            }

            // The target for THIS call is `created_native_obj` (the object this 'with' block is attached to)
            with_block_target_for_do = dynamic_lvgl_call_ir(call_details->func_name,
                                                            created_native_obj,
                                                            call_arg_array,
                                                            call_arg_count,
                                                            ctx->api_spec);
            free_resolved_expr_list(resolved_call_args_list);

        } else if (wb->target_expr) {
            // Old style "with": "target_id" (target_expr is likely IR_EXPR_REGISTRY_REF or literal string)
            IRExpr* resolved_target_ref = resolve_expr(ctx, wb->target_expr);
            if (resolved_target_ref) {
                const char* target_id_str = ir_node_get_string((IRNode*)resolved_target_ref);
                if (target_id_str) {
                    with_block_target_for_do = obj_registry_get(target_id_str);
                }
                ir_free((IRNode*)resolved_target_ref);
            }
        } else {
            // No "with.obj.call" and no "with.target_expr".
            // This implies the "do" block (if any) should apply to the current `created_native_obj`.
            DEBUG_LOG(LOG_MODULE_RENDERER, "Info: 'with' block for obj '%s' has no 'obj.call' or 'target_expr'. 'do' block will target current object.", obj->c_name);
            with_block_target_for_do = created_native_obj;
        }

        if (with_block_target_for_do) {
            // Process the "do" part: properties and/or children, using with_block_target_for_do as their target/parent.
            if (wb->properties) { // Properties from the "do" block
                // Determine the json_type of with_block_target_for_do for applying properties correctly.
                const char* json_type_for_do_properties = "obj"; // Default
                if (wb->target_expr && wb->target_expr->type == IR_EXPR_FUNCTION_CALL) {
                    const char* func_name = ((IRExprFunctionCall*)wb->target_expr)->func_name;
                    // Use the existing helper to get the C return type string.
                    const char* return_c_type = api_spec_get_function_return_type(ctx->api_spec, func_name);

                    if (return_c_type) {
                        // Basic heuristic: if the C return type contains "style_t", assume json_type "style".
                        if (strstr(return_c_type, "style_t") != NULL) {
                             json_type_for_do_properties = "style";
                        }
                        // TODO: Add more heuristics or a mapping from C return types to json_types in api_spec if needed.
                        // For example, if "lv_tileview_tile_t*", map to a hypothetical "tile" json_type.
                    }
                }

                // IRObject fake_do_scope_obj = {.json_type = (char*)json_type_for_do_properties, .properties = wb->properties};
                // render_properties(ctx, &fake_do_scope_obj, with_block_target_for_do);
                // Instead of calling render_properties, inline similar logic here to handle args correctly for 'with'
                IRProperty* with_prop = wb->properties;
                while(with_prop) {
                    const PropertyDefinition* current_with_prop_def = api_spec_find_property(ctx->api_spec, json_type_for_do_properties, with_prop->name);
                    if (!current_with_prop_def || !current_with_prop_def->setter) {
                        DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: Setter for property '%s' on type '%s' in 'with' block not found. Skipping.", with_prop->name, json_type_for_do_properties);
                        with_prop = with_prop->next;
                        continue;
                    }

                    IRExprNode* with_resolved_args = NULL;
                    if (with_prop->value->type == IR_EXPR_ARRAY) {
                        IRExprArray* arr = (IRExprArray*)with_prop->value;
                        IRExprNode* elem = arr->elements;
                        while(elem) {
                            ir_expr_list_add(&with_resolved_args, resolve_expr(ctx, elem->expr));
                            elem = elem->next;
                        }
                    } else {
                        ir_expr_list_add(&with_resolved_args, resolve_expr(ctx, with_prop->value));
                    }

                    IRNode* with_arg_array[MAX_RENDER_ARGS];
                    int with_arg_count = 0;
                    IRExprNode* current_with_arg_node = with_resolved_args;
                    while(current_with_arg_node && with_arg_count < MAX_RENDER_ARGS) {
                        with_arg_array[with_arg_count++] = (IRNode*)current_with_arg_node->expr;
                        current_with_arg_node = current_with_arg_node->next;
                    }

                    // Check for NULL lv_obj_t* or lv_style_t* arguments
                    if (current_with_prop_def->func_args && current_with_prop_def->func_args->next) {
                        const FunctionArg* current_with_arg_spec = current_with_prop_def->func_args->next;
                        for (int i = 0; i < with_arg_count; ++i) {
                            if (current_with_arg_spec) {
                                const char* arg_type_str = current_with_arg_spec->type;
                                const char* arg_name_str = current_with_arg_spec->name ? current_with_arg_spec->name : "unnamed_arg";
                                // const char* arg_name_str = current_with_arg_spec ? (current_with_arg_spec->name ? current_with_arg_spec->name : "unnamed_arg") : "unknown_arg_ λόγω_missing_spec"; // Redundant definition removed
                                if (arg_type_str && (strcmp(arg_type_str, "lv_obj_t*") == 0 || strcmp(arg_type_str, "lv_style_t*") == 0)) {
                                    if (with_arg_array[i] == NULL) { // Case 1: resolve_expr returned NULL
                                        char err_msg[512];
                                        snprintf(err_msg, sizeof(err_msg), "Renderer Error (with block): IRNode for argument '%s' (property '%s') is NULL. Resolution failed.", arg_name_str, with_prop->name);
                                        render_abort(err_msg);
                                    }
                                    void* ptr_val = NULL;
                                    bool needs_runtime_null_check_with = true;

                                    if (with_arg_array[i]->type == IR_EXPR_LITERAL) {
                                        IRExprLiteral* lit = (IRExprLiteral*)with_arg_array[i];
                                        if ((strcmp(lit->value, "NULL") == 0 && !lit->is_string) || strcmp(lit->value, "0") == 0) {
                                            ptr_val = NULL;
                                        } else {
                                           if (lit->value[0] == '0' && (lit->value[1] == 'x' || lit->value[1] == 'X')) {
                                               sscanf(lit->value, "%p", &ptr_val);
                                           } else {
                                               needs_runtime_null_check_with = false;
                                           }
                                        }
                                    } else if (with_arg_array[i]->type == IR_EXPR_REGISTRY_REF) {
                                        needs_runtime_null_check_with = false; // Resolved by obj_registry_get in archetype
                                    }

                                    if (needs_runtime_null_check_with && ptr_val == NULL) {
                                        char err_msg[512];
                                        snprintf(err_msg, sizeof(err_msg), "Renderer Error (with block): Argument '%s' for function '%s' (property '%s') is NULL.", arg_name_str, current_with_prop_def->setter, with_prop->name);
                                        render_abort(err_msg);
                                    }
                                }
                                if(current_with_arg_spec) current_with_arg_spec = current_with_arg_spec->next;
                            } else {
                                DEBUG_LOG(LOG_MODULE_RENDERER, "Warning (with_block): Missing arg spec for arg %d of %s. Cannot perform type-specific NULL check.", i, current_with_prop_def ? current_with_prop_def->setter : "unknown_setter");
                            }
                        }
                    }
                    dynamic_lvgl_call_ir(current_with_prop_def->setter, with_block_target_for_do, with_arg_array, with_arg_count, ctx->api_spec);
                    free_resolved_expr_list(with_resolved_args);
                    with_prop = with_prop->next;
                }
            }

            if (wb->children_root) { // Children from the "do" block
                IRObject* child_item = wb->children_root->children; // children_root is an IRObject wrapper for the list
                while(child_item) {
                    render_object(ctx, child_item, with_block_target_for_do);
                    child_item = child_item->next;
                }
            }
        } else {
            DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: Target for 'with' block associated with '%s' could not be determined or was NULL. 'do' block skipped.", obj->c_name);
        }
        wb = wb->next;
    }

    IRObject* child = obj->children;
    while (child) {
        render_object(ctx, child, created_native_obj);
        child = child->next;
    }
}


static IRExpr* resolve_expr(RenderContext* ctx, IRExpr* expr) {
    if (!expr) return NULL;

    switch (expr->type) {
        case IR_EXPR_CONTEXT_VAR: {
            cJSON* stack = ctx->context_stack;
            for (int i = cJSON_GetArraySize(stack) - 1; i >= 0; i--) {
                cJSON* current_context = cJSON_GetArrayItem(stack, i);
                cJSON* val_json = cJSON_GetObjectItem(current_context, ((IRExprContextVar*)expr)->name);
                if (val_json) {
                    if (cJSON_IsString(val_json)) return ir_new_expr_literal_string(val_json->valuestring);
                    if (cJSON_IsNumber(val_json)) {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%g", val_json->valuedouble);
                        return ir_new_expr_literal(buf);
                    }
                }
            }
            DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: Context var '%s' not found.", ((IRExprContextVar*)expr)->name);
            return ir_new_expr_literal("NULL");
        }

        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;
            IRExprNode* resolved_args = NULL;
            int arg_count = 0;
            for (IRExprNode* n = call->args; n != NULL; n = n->next) {
                ir_expr_list_add(&resolved_args, resolve_expr(ctx, n->expr));
                arg_count++;
            }

            IRNode* arg_array[MAX_RENDER_ARGS];
            IRExprNode* current_arg_node = resolved_args;
            for(int i = 0; i < arg_count; ++i) {
                arg_array[i] = (IRNode*)current_arg_node->expr;
                current_arg_node = current_arg_node->next;
            }

            // Pass ApiSpec for context-aware parsing if needed by the call
            void* result_ptr = dynamic_lvgl_call_ir(call->func_name, NULL, arg_array, arg_count, ctx->api_spec);
            free_resolved_expr_list(resolved_args);

            char buf[32];
            snprintf(buf, sizeof(buf), "%ld", (intptr_t)result_ptr);
            return ir_new_expr_literal(buf);
        }

        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)expr;
            return lit->is_string ? ir_new_expr_literal_string(lit->value) : ir_new_expr_literal(lit->value);
        }
        case IR_EXPR_ENUM:
            return ir_new_expr_enum(((IRExprEnum*)expr)->symbol, ((IRExprEnum*)expr)->value);
        case IR_EXPR_REGISTRY_REF:
            return ir_new_expr_registry_ref(((IRExprRegistryRef*)expr)->name);
        case IR_EXPR_STATIC_STRING:
            return ir_new_expr_static_string(((IRExprStaticString*)expr)->value);
        case IR_EXPR_ARRAY: {
            IRExprArray* arr = (IRExprArray*)expr;
            IRExprNode* resolved_elements = NULL;
            for(IRExprNode* n = arr->elements; n != NULL; n = n->next) {
                ir_expr_list_add(&resolved_elements, resolve_expr(ctx, n->expr));
            }
            return ir_new_expr_array(resolved_elements);
        }
        default:
             DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: cannot resolve unhandled expr type %d", expr->type);
            return NULL;
    }
}

static void free_resolved_expr_list(IRExprNode* head) {
    IRExprNode* current = head;
    while (current) {
        IRExprNode* next = current->next;
        ir_free((IRNode*)current->expr);
        free(current);
        current = next;
    }
}

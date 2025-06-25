#include "lvgl_ui_renderer.h"
#include "lvgl_dispatch.h"
#include "ir.h"
#include "utils.h"
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

        dynamic_lvgl_call_ir(prop_def->setter, native_obj, arg_array, arg_count);
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
        created_native_obj = dynamic_lvgl_call_ir(widget_def->create, parent_obj, NULL, 0);
    } else if (widget_def && widget_def->init_func) {
        if (strcmp(widget_def->c_type, "lv_style_t") == 0) {
            lv_style_t* style = malloc(sizeof(lv_style_t));
            if (style) {
                dynamic_lvgl_call_ir(widget_def->init_func, style, NULL, 0);
                created_native_obj = style;
            }
        }
    } else {
        created_native_obj = dynamic_lvgl_call_ir("lv_obj_create", parent_obj, NULL, 0);
    }

    if (!created_native_obj) {
        fprintf(stderr, "Renderer Error: Failed to create object of type '%s'.\n", obj->json_type);
        return;
    }

    obj_registry_add(obj->c_name, created_native_obj);
    if (obj->registered_id) {
        obj_registry_add(obj->registered_id, created_native_obj);
    }
    DEBUG_LOG(LOG_MODULE_RENDERER, "Created object '%s' (%s) at %p", obj->c_name, obj->json_type, created_native_obj);

    render_properties(ctx, obj, created_native_obj);

    IRWithBlock* wb = obj->with_blocks;
    while(wb) {
        IRExpr* resolved_target = resolve_expr(ctx, wb->target_expr);
        void* with_target_obj = obj_registry_get(ir_node_get_string((IRNode*)resolved_target));
        ir_free((IRNode*)resolved_target);
        if (with_target_obj) {
            // This is a simplification; we'd need to know the type of the 'with' target.
            // Assume obj for now.
            IRObject fake_ir_obj = {.json_type = "obj", .properties = wb->properties};
            render_properties(ctx, &fake_ir_obj, with_target_obj);
            if(wb->children_root) {
                IRObject* child = wb->children_root->children;
                while(child) {
                    render_object(ctx, child, with_target_obj);
                    child = child->next;
                }
            }
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

            void* result_ptr = dynamic_lvgl_call_ir(call->func_name, NULL, arg_array, arg_count);
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

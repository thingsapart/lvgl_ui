#include "lvgl_renderer.h"
#include "data_binding.h"
#include "debug_log.h"
#include "registry.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>
#include "viewer/view_inspector.h"


// --- Forward Declarations ---
static void render_object_list(ApiSpec* spec, IRObject* head, Registry* registry);
static void render_single_object(ApiSpec* spec, IRObject* current_obj, Registry* registry);
static void evaluate_expression(ApiSpec* spec, Registry* registry, IRExpr* expr, RenderValue* out_val);

// --- Main Backend Entry Point ---

void lvgl_render_backend(IRRoot* root, ApiSpec* api_spec, lv_obj_t* parent, Registry* registry) {
    if (!root || !api_spec || !parent || !registry) {
        DEBUG_LOG(LOG_MODULE_RENDERER, "Error: lvgl_render_backend called with NULL arguments.");
        return;
    }

    obj_registry_init();
    registry_add_pointer(registry, parent, "parent", "obj", "lv_obj_t*");
    obj_registry_add("parent", parent);

    DEBUG_LOG(LOG_MODULE_RENDERER, "Starting LVGL render backend.");
    render_object_list(api_spec, root->root_objects, registry);
    DEBUG_LOG(LOG_MODULE_RENDERER, "LVGL render backend finished.");

    lv_obj_update_layout(parent);
    DEBUG_LOG(LOG_MODULE_RENDERER, "Forcing layout update on parent container.");
}

// --- Core Rendering Logic ---
static binding_value_t* evaluate_binding_array_expr(ApiSpec* spec, Registry* registry, IRExprArray* arr, uint32_t* out_count);

static void render_single_object(ApiSpec* spec, IRObject* current_obj, Registry* registry) {
    DEBUG_LOG(LOG_MODULE_RENDERER, "Rendering object: c_name='%s', json_type='%s'", current_obj->c_name, current_obj->json_type);

    RenderValue constructor_result = { .type = RENDER_VAL_TYPE_NULL, .as.p_val = NULL };
    void* c_obj = NULL;

    if (current_obj->constructor_expr) {
        bool is_malloc = (current_obj->constructor_expr->base.type == IR_EXPR_FUNCTION_CALL &&
                          strcmp(((IRExprFunctionCall*)current_obj->constructor_expr)->func_name, "malloc") == 0);

        if (is_malloc) {
            if (strcmp(current_obj->c_type, "lv_style_t*") == 0) {
                c_obj = malloc(sizeof(lv_style_t));
            } else {
                 render_abort("Renderer Error: Don't know how to malloc this type");
            }
        } else {
            evaluate_expression(spec, registry, current_obj->constructor_expr, &constructor_result);
            if(constructor_result.type == RENDER_VAL_TYPE_POINTER) {
                c_obj = constructor_result.as.p_val;
            }
        }
    }

    if (c_obj) view_inspector_set_object_pointer((IRNode*)current_obj, c_obj);

    registry_add_pointer(registry, c_obj, current_obj->c_name, current_obj->json_type, current_obj->c_type);
    obj_registry_add(current_obj->c_name, c_obj);

    if (current_obj->registered_id) {
         registry_add_pointer(registry, c_obj, current_obj->registered_id, current_obj->json_type, current_obj->c_type);
         obj_registry_add(current_obj->registered_id, c_obj);
    }

    if (current_obj->operations) {
        for (IROperationNode* op_node = current_obj->operations; op_node; op_node = op_node->next) {
            IRNode* node = op_node->op_node;
            if (node->type == IR_NODE_OBJECT) {
                render_single_object(spec, (IRObject*)node, registry);
            } else if (node->type == IR_NODE_WARNING) {
                print_hint("%s", ((IRWarning*)node)->message);
            } else if (node->type == IR_NODE_OBSERVER) {
                IRObserver* obs = (IRObserver*)node;
                const void* config_ptr = NULL;
                size_t config_len = 0;
                const void* default_ptr = NULL;

                RenderValue val;
                evaluate_expression(spec, registry, obs->config_expr, &val);

                if (obs->config_expr->base.type == IR_EXPR_LITERAL) {
                    if (((IRExprLiteral*)obs->config_expr)->is_string) {
                        config_ptr = val.as.s_val; // Format string
                    } else {
                        config_ptr = &val.as.b_val; // bool for direct mapping
                    }
                } else if (obs->config_expr->base.type == IR_EXPR_ARRAY) { // Map
                    IRExprArray* map_arr = (IRExprArray*)obs->config_expr;
                    config_len = 0;
                    for (IRExprNode* n = map_arr->elements; n; n = n->next) config_len++;
                    binding_map_entry_t* map = calloc(config_len, sizeof(binding_map_entry_t));
                    
                    int i = 0;
                    int final_count = 0;
                    for (IRExprNode* n = map_arr->elements; n; n = n->next, i++) {
                        IRExprArray* pair = (IRExprArray*)n->expr;
                        RenderValue key, value;
                        evaluate_expression(spec, registry, pair->elements->expr, &key);
                        evaluate_expression(spec, registry, pair->elements->next->expr, &value);

                        if(key.type == RENDER_VAL_TYPE_STRING && strcmp(key.as.s_val, "default") == 0) {
                            if (obs->update_type == OBSERVER_TYPE_STYLE) default_ptr = value.as.p_val;
                            else default_ptr = &value.as.b_val;
                        } else {
                            if (key.type == RENDER_VAL_TYPE_STRING) map[final_count].key = (binding_value_t){.type=BINDING_TYPE_STRING, .as.s_val=key.as.s_val};
                            else if (key.type == RENDER_VAL_TYPE_BOOL) map[final_count].key = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=key.as.b_val};
                            else map[final_count].key = (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val=(float)key.as.i_val};
                            
                            if (obs->update_type == OBSERVER_TYPE_STYLE) map[final_count].value.p_val = value.as.p_val;
                            else map[final_count].value.b_val = value.as.b_val;
                            final_count++;
                        }
                    }
                    config_ptr = map;
                    config_len = final_count;
                }
                data_binding_add_observer(obs->state_name, c_obj, obs->update_type, config_ptr, config_len, default_ptr);
                if (config_len > 0) free((void*)config_ptr);
            } else if (node->type == IR_NODE_ACTION) {
                IRAction* act = (IRAction*)node;
                binding_value_t* cycle_values = NULL;
                uint32_t cycle_count = 0;
                if (act->action_type == ACTION_TYPE_CYCLE && act->data_expr && act->data_expr->base.type == IR_EXPR_ARRAY) {
                    cycle_values = evaluate_binding_array_expr(spec, registry, (IRExprArray*)act->data_expr, &cycle_count);
                }
                data_binding_add_action(c_obj, act->action_name, act->action_type, cycle_values, cycle_count);
                if (cycle_values) free(cycle_values);
            } else {
                RenderValue ignored;
                evaluate_expression(spec, registry, (IRExpr*)node, &ignored);
            }
        }
    }
}

static void render_object_list(ApiSpec* spec, IRObject* head, Registry* registry) {
    for (IRObject* current_obj = head; current_obj; current_obj = current_obj->next) {
        render_single_object(spec, current_obj, registry);
    }
}


// --- Recursive Expression Evaluator ---
static binding_value_t* evaluate_binding_array_expr(ApiSpec* spec, Registry* registry, IRExprArray* arr, uint32_t* out_count) {
    int count = 0;
    for (IRExprNode* n = arr->elements; n; n = n->next) count++;
    *out_count = count;
    if (count == 0) return NULL;

    binding_value_t* result_array = calloc(count, sizeof(binding_value_t));
    if (!result_array) render_abort("Failed to allocate binding_value_t array");

    int i = 0;
    for (IRExprNode* n = arr->elements; n; n = n->next, i++) {
        RenderValue val;
        evaluate_expression(spec, registry, n->expr, &val);
        switch(val.type) {
            case RENDER_VAL_TYPE_STRING: result_array[i] = (binding_value_t){.type=BINDING_TYPE_STRING, .as.s_val=val.as.s_val}; break;
            case RENDER_VAL_TYPE_BOOL: result_array[i] = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=val.as.b_val}; break;
            case RENDER_VAL_TYPE_INT: result_array[i] = (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val=(float)val.as.i_val}; break;
            default: result_array[i].type = BINDING_TYPE_NULL;
        }
    }
    return result_array;
}

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
                if(strcmp(lit->base.c_type, "bool") == 0) {
                    out_val->type = RENDER_VAL_TYPE_BOOL;
                    out_val->as.b_val = (strcmp(lit->value, "true") == 0);
                } else { // It's a number, treat as int internally for renderer
                    out_val->type = RENDER_VAL_TYPE_INT;
                    out_val->as.i_val = strtol(lit->value, NULL, 0);
                }
            }
            return;
        }

        case IR_EXPR_STATIC_STRING: {
            out_val->type = RENDER_VAL_TYPE_STRING;
            out_val->as.s_val = ((IRExprStaticString*)expr)->value;
            return;
        }

        case IR_EXPR_ENUM: {
            out_val->type = RENDER_VAL_TYPE_INT;
            out_val->as.i_val = ((IRExprEnum*)expr)->value;
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

        case IR_EXPR_ARRAY: {
            IRExprArray* arr = (IRExprArray*)expr;
            if (arr->static_array_ptr) {
                out_val->type = RENDER_VAL_TYPE_POINTER;
                out_val->as.p_val = arr->static_array_ptr;
                return;
            }

            int element_count = 0;
            for (IRExprNode* n = arr->elements; n; n=n->next) element_count++;
            
            char* base_type = get_array_base_type(arr->base.c_type);
            size_t element_size = 0;

            if(strcmp(base_type, "lv_coord_t") == 0) element_size = sizeof(lv_coord_t);
            else if(strcmp(base_type, "int32_t") == 0) element_size = sizeof(int32_t);
            else if(strcmp(base_type, "int") == 0) element_size = sizeof(int);
            else if(strcmp(base_type, "void*") == 0) element_size = sizeof(void*);
            else {
                render_abort("Unsupported array base type for renderer");
            }
            free(base_type);

            void* c_array = malloc(element_count * element_size);
            if (!c_array) render_abort("Failed to allocate memory for static array.");

            int i = 0;
            for (IRExprNode* n = arr->elements; n; n=n->next) {
                RenderValue elem_val;
                evaluate_expression(spec, registry, n->expr, &elem_val);
                if (elem_val.type == RENDER_VAL_TYPE_INT) {
                    if (element_size == sizeof(lv_coord_t)) ((lv_coord_t*)c_array)[i] = (lv_coord_t)elem_val.as.i_val;
                    else if (element_size == sizeof(int32_t)) ((int32_t*)c_array)[i] = (int32_t)elem_val.as.i_val;
                    else if (element_size == sizeof(int)) ((int*)c_array)[i] = (int)elem_val.as.i_val;
                } else if (elem_val.type == RENDER_VAL_TYPE_POINTER) {
                    if (element_size == sizeof(void*)) ((void**)c_array)[i] = elem_val.as.p_val;
                }
                i++;
            }
            
            arr->static_array_ptr = c_array;
            registry_add_static_array(registry, c_array);
            
            out_val->type = RENDER_VAL_TYPE_POINTER;
            out_val->as.p_val = c_array;
            return;
        }
        
        case IR_EXPR_RUNTIME_REG_ADD: {
            IRExprRuntimeRegAdd* reg = (IRExprRuntimeRegAdd*)expr;
            RenderValue obj_to_reg;
            evaluate_expression(spec, registry, reg->object_expr, &obj_to_reg);
            if (obj_to_reg.type == RENDER_VAL_TYPE_POINTER) {
                obj_registry_add(reg->id, obj_to_reg.as.p_val);
            }
            out_val->type = RENDER_VAL_TYPE_NULL;
            return;
        }

        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;
            DEBUG_LOG(LOG_MODULE_DISPATCH, "Evaluating call: %s", call->func_name);

            int arg_count = 0;
            for (IRExprNode* n = call->args; n; n = n->next) arg_count++;
            RenderValue* evaluated_args = arg_count > 0 ? calloc(arg_count, sizeof(RenderValue)) : NULL;
            if (arg_count > 0 && !evaluated_args) render_abort("Failed to allocate evaluated_args array");
            int i = 0;
            for (IRExprNode* n = call->args; n; n = n->next) {
                evaluate_expression(spec, registry, n->expr, &evaluated_args[i++]);
            }
            
            IRExprNode* temp_ir_args_head = NULL;
            for (i = 0; i < arg_count; i++) {
                IRExpr* temp_expr = NULL;
                switch (evaluated_args[i].type) {
                    case RENDER_VAL_TYPE_INT: {
                        char buf[32]; snprintf(buf, sizeof(buf), "%ld", (long)evaluated_args[i].as.i_val);
                        temp_expr = ir_new_expr_literal(buf, "int"); break;
                    }
                    case RENDER_VAL_TYPE_BOOL:
                        temp_expr = ir_new_expr_literal(evaluated_args[i].as.b_val ? "true" : "false", "bool"); break;
                    case RENDER_VAL_TYPE_COLOR: {
                         char buf[32]; snprintf(buf, sizeof(buf), "%u", lv_color_to_u32(evaluated_args[i].as.color_val));
                        temp_expr = ir_new_expr_literal(buf, "lv_color_t"); break;
                    }
                    case RENDER_VAL_TYPE_STRING:
                        temp_expr = ir_new_expr_literal(evaluated_args[i].as.s_val, "const char*");
                        ((IRExprLiteral*)temp_expr)->is_string = true; break;
                    case RENDER_VAL_TYPE_POINTER:
                    case RENDER_VAL_TYPE_NULL: {
                        void* ptr = evaluated_args[i].as.p_val;
                        const char* id = registry_get_id_from_pointer(registry, ptr);
                        if (id) {
                            temp_expr = ir_new_expr_literal(id, "void*");
                            ((IRExprLiteral*)temp_expr)->is_string = true;
                        } else {
                            temp_expr = ir_new_expr_raw_pointer(ptr, "void*");
                        }
                        break;
                    }
                }
                if (temp_expr) ir_expr_list_add(&temp_ir_args_head, temp_expr);
            }

            IRNode** final_ir_args_array = arg_count > 0 ? calloc(arg_count, sizeof(IRNode*)) : NULL;
            if (arg_count > 0 && !final_ir_args_array) render_abort("Failed to alloc final_ir_args_array");
            i = 0;
            for (IRExprNode* n = temp_ir_args_head; n; n = n->next) final_ir_args_array[i++] = (IRNode*)n->expr;
            
            void* target_obj = NULL;
            IRNode** dispatcher_args = NULL;
            int dispatcher_arg_count = 0;
            const FunctionArg* f_args = api_spec_get_function_args_by_name(spec, call->func_name);
            bool first_arg_is_target = f_args && f_args->type && (strstr(f_args->type, "_t*") != NULL);

            if (first_arg_is_target && arg_count > 0) {
                target_obj = evaluated_args[0].as.p_val;
                dispatcher_args = (arg_count > 1) ? &final_ir_args_array[1] : NULL;
                dispatcher_arg_count = arg_count - 1;
            } else {
                target_obj = NULL;
                dispatcher_args = final_ir_args_array;
                dispatcher_arg_count = arg_count;
            }

            *out_val = dynamic_lvgl_call_ir(call->func_name, target_obj, dispatcher_args, dispatcher_arg_count, spec);
            
            free(evaluated_args);
            ir_free((IRNode*)temp_ir_args_head);
            free(final_ir_args_array);
            return;
        }
        default:
            DEBUG_LOG(LOG_MODULE_RENDERER, "Warning: evaluate_expression called on un-evaluatable node type %d", expr->base.type);
            out_val->type = RENDER_VAL_TYPE_NULL;
            out_val->as.p_val = NULL;
            return;
    }
}

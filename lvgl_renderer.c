#include "lvgl_renderer.h"
#include "c_gen/lvgl_dispatch.h"
#include "debug_log.h"
#include "registry.h"
#include "utils.h"
#include <stdlib.h>


// --- Forward Declarations ---
static void render_object_list(ApiSpec* spec, IRObject* head, Registry* registry);
static void render_single_object(ApiSpec* spec, IRObject* current_obj, Registry* registry);
static void evaluate_expression(ApiSpec* spec, Registry* registry, IRExpr* expr, RenderValue* out_val);
static void debug_print_expr_as_c(IRExpr* expr, Registry* registry, FILE* stream);


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

// --- Debugging Helper ---

static void debug_print_expr_as_c(IRExpr* expr, Registry* registry, FILE* stream) {
    if (!expr) {
        fprintf(stream, "NULL");
        return;
    }

    switch (expr->base.type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)expr;
            if (lit->is_string) {
                // A full C-escape is complex, this is good enough for debug
                fprintf(stream, "\"%s\"", lit->value);
            } else {
                fprintf(stream, "%s", lit->value);
            }
            break;
        }
        case IR_EXPR_STATIC_STRING: {
            fprintf(stream, "\"%s\"", ((IRExprStaticString*)expr)->value);
            break;
        }
        case IR_EXPR_ENUM:
            fprintf(stream, "%s", ((IRExprEnum*)expr)->symbol);
            break;
        case IR_EXPR_REGISTRY_REF: {
            fprintf(stream, "%s", ((IRExprRegistryRef*)expr)->name);
            break;
        }
        case IR_EXPR_RAW_POINTER: {
             fprintf(stream, "(void*)%p", ((IRExprRawPointer*)expr)->ptr);
             break;
        }
        case IR_EXPR_CONTEXT_VAR:
             fprintf(stream, "/* CONTEXT_VAR: %s */", ((IRExprContextVar*)expr)->name);
            break;
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;
            fprintf(stream, "%s(", call->func_name);
            IRExprNode* arg_node = call->args;
            bool first = true;
            while(arg_node) {
                if (!first) fprintf(stream, ", ");
                bool is_struct_by_value = false;
                if (arg_node->expr->base.type == IR_EXPR_REGISTRY_REF) {
                    const char* ref_name = ((IRExprRegistryRef*)arg_node->expr)->name;
                    const char* c_type = registry_get_c_type_for_id(registry, ref_name);
                    if (c_type && strchr(c_type, '*') == NULL) {
                        is_struct_by_value = true;
                    }
                }
                if (is_struct_by_value) fprintf(stream, "&");
                debug_print_expr_as_c(arg_node->expr, registry, stream);
                first = false;
                arg_node = arg_node->next;
            }
            fprintf(stream, ")");
            break;
        }
        case IR_EXPR_ARRAY: {
            fprintf(stream, "(%p) => { ", ((IRExprArray*)expr)->static_array_ptr);
            IRExprNode* elem_node = ((IRExprArray*)expr)->elements;
            bool first = true;
            while(elem_node) {
                if (!first) fprintf(stream, ", ");
                debug_print_expr_as_c(elem_node->expr, registry, stream);
                first = false;
                elem_node = elem_node->next;
            }
            fprintf(stream, " }");
            break;
        }
        case IR_EXPR_RUNTIME_REG_ADD: {
            IRExprRuntimeRegAdd* reg = (IRExprRuntimeRegAdd*)expr;
            fprintf(stream, "obj_registry_add(\"%s\", ", reg->id);
            bool is_struct_by_value = false;
            if (reg->object_expr->base.type == IR_EXPR_REGISTRY_REF) {
                const char* ref_name = ((IRExprRegistryRef*)reg->object_expr)->name;
                const char* c_type = registry_get_c_type_for_id(registry, ref_name);
                if (c_type && strchr(c_type, '*') == NULL) {
                    is_struct_by_value = true;
                }
            }
            if (is_struct_by_value) fprintf(stream, "&");
            debug_print_expr_as_c(reg->object_expr, registry, stream);
            fprintf(stream, ")");
            break;
        }
        default:
            fprintf(stream, "/* UNKNOWN_EXPR */");
            break;
    }
}


// --- Core Rendering Logic ---

// This function processes a SINGLE object: creates it, registers it, and processes its operations.
static void render_single_object(ApiSpec* spec, IRObject* current_obj, Registry* registry) {
    DEBUG_LOG(LOG_MODULE_RENDERER, "Rendering object: c_name='%s', json_type='%s'", current_obj->c_name, current_obj->json_type);

    bool debug_c_code = debug_log_is_module_enabled(LOG_MODULE_RENDERER);
    if (debug_c_code) {
        fprintf(stderr, "[RENDERER C-CODE] // %s: %s\n", current_obj->registered_id ? current_obj->registered_id : "unnamed", current_obj->c_name);
        fprintf(stderr, "[RENDERER C-CODE] do {\n");
        bool is_pointer = (current_obj->c_type && strchr(current_obj->c_type, '*') != NULL);
        fprintf(stderr, "[RENDERER C-CODE]     %s %s%s;\n", current_obj->c_type, current_obj->c_name, is_pointer ? " = NULL" : "");
    }

    RenderValue constructor_result;
    constructor_result.type = RENDER_VAL_TYPE_NULL;
    constructor_result.as.p_val = NULL;

    void* c_obj = NULL;

    // 1. Create the object by executing its constructor expression
    if (current_obj->constructor_expr) {
        if (debug_c_code) {
            bool is_pointer = (current_obj->c_type && strchr(current_obj->c_type, '*') != NULL);
            fprintf(stderr, "[RENDERER C-CODE]     ");
            if (is_pointer) {
                fprintf(stderr, "%s = ", current_obj->c_name);
            }
            debug_print_expr_as_c(current_obj->constructor_expr, registry, stderr);
            fprintf(stderr, ";\n");
        }
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
    // This happens BEFORE operations are processed.
    registry_add_pointer(registry, c_obj, current_obj->c_name, current_obj->json_type, current_obj->c_type);
    // Add the C-name to the C-side registry as well, so it can be resolved by the dispatcher.
    obj_registry_add(current_obj->c_name, c_obj);

    if (current_obj->registered_id) {
         registry_add_pointer(registry, c_obj, current_obj->registered_id, current_obj->json_type, current_obj->c_type);
         obj_registry_add(current_obj->registered_id, c_obj);
         DEBUG_LOG(LOG_MODULE_REGISTRY, "Registered ID '%s' to pointer %p", current_obj->registered_id, c_obj);
    }

    // 2. Execute all operations for this object (setup calls and children, in order).
    if (current_obj->operations) {
        if (debug_c_code) fprintf(stderr, "[RENDERER C-CODE]\n");
        IROperationNode* op_node = current_obj->operations;
        while(op_node) {
            if (op_node->op_node->type == IR_NODE_OBJECT) {
                // It's a child object. Recursively render it.
                render_single_object(spec, (IRObject*)op_node->op_node, registry);
            } else if (op_node->op_node->type == IR_NODE_WARNING) {
                IRWarning* warn = (IRWarning*)op_node->op_node;
                print_hint("%s", warn->message);
            } else {
                // It's an expression (a setup call). Evaluate it.
                if (debug_c_code) {
                    fprintf(stderr, "[RENDERER C-CODE]     ");
                    debug_print_expr_as_c((IRExpr*)op_node->op_node, registry, stderr);
                    fprintf(stderr, ";\n");
                }
                RenderValue ignored_result;
                evaluate_expression(spec, registry, (IRExpr*)op_node->op_node, &ignored_result);
            }
            op_node = op_node->next;
        }
    }

    if (debug_c_code) {
        fprintf(stderr, "[RENDERER C-CODE] } while(0);\n\n");
    }
}

// This function iterates over a list of SIBLING objects.
static void render_object_list(ApiSpec* spec, IRObject* head, Registry* registry) {
    for (IRObject* current_obj = head; current_obj; current_obj = current_obj->next) {
        render_single_object(spec, current_obj, registry);
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

        case IR_EXPR_STATIC_STRING: {
            out_val->type = RENDER_VAL_TYPE_STRING;
            out_val->as.s_val = ((IRExprStaticString*)expr)->value;
            return;
        }

        case IR_EXPR_ENUM: {
            // An enum represents an integer value.
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
            else {
                char err_buf[256];
                snprintf(err_buf, sizeof(err_buf), "Unsupported array base type for renderer: %s", base_type);
                render_abort(err_buf);
            }
            free(base_type);

            void* c_array = malloc(element_count * element_size);
            if (!c_array) render_abort("Failed to allocate memory for static array.");

            int i = 0;
            for (IRExprNode* n = arr->elements; n; n=n->next) {
                RenderValue elem_val;
                evaluate_expression(spec, registry, n->expr, &elem_val);
                // Assuming all array elements resolve to integers for now.
                if (elem_val.type == RENDER_VAL_TYPE_INT) {
                    if (element_size == sizeof(lv_coord_t)) ((lv_coord_t*)c_array)[i] = (lv_coord_t)elem_val.as.i_val;
                    else if (element_size == sizeof(int32_t)) ((int32_t*)c_array)[i] = (int32_t)elem_val.as.i_val;
                    else if (element_size == sizeof(int)) ((int*)c_array)[i] = (int)elem_val.as.i_val;
                }
                i++;
            }
            
            arr->static_array_ptr = c_array;
            registry_add_static_array(registry, c_array);
            
            out_val->type = RENDER_VAL_TYPE_POINTER;
            out_val->as.p_val = c_array;

	    printf("OUT ARR >> %p\n", c_array);

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

            // 2. Repackage the concrete values into temporary IR nodes that the dispatcher can understand.
            // This is the key part of the fix: we now create IR_EXPR_RAW_POINTER for intermediate pointers.
            IRExprNode* temp_ir_args_head = NULL;
            for (i = 0; i < arg_count; i++) {
                IRExpr* temp_expr = NULL;
                switch (evaluated_args[i].type) {
                    case RENDER_VAL_TYPE_INT: {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%ld", (long)evaluated_args[i].as.i_val);
                        temp_expr = ir_new_expr_literal(buf, "int");
                        break;
                    }
                    case RENDER_VAL_TYPE_BOOL:
                        temp_expr = ir_new_expr_literal(evaluated_args[i].as.b_val ? "true" : "false", "bool");
                        break;
                    case RENDER_VAL_TYPE_COLOR: {
                        char buf[32];
                        snprintf(buf, sizeof(buf), "%u", lv_color_to_u32(evaluated_args[i].as.color_val));
                        temp_expr = ir_new_expr_literal(buf, "lv_color_t");
                        break;
                    }
                    case RENDER_VAL_TYPE_STRING:
                        temp_expr = ir_new_expr_literal(evaluated_args[i].as.s_val, "const char*");
                        ((IRExprLiteral*)temp_expr)->is_string = true;
                        break;
                    case RENDER_VAL_TYPE_POINTER:
                    case RENDER_VAL_TYPE_NULL: {
                        void* ptr = evaluated_args[i].as.p_val;
                        const char* id = registry_get_id_from_pointer(registry, ptr);
                        if (id) {
                             // If the pointer is registered, pass its ID so it can be looked up again.
                            temp_expr = ir_new_expr_literal(id, "void*");
                            ((IRExprLiteral*)temp_expr)->is_string = true;
                        } else {
                            // If the pointer is not registered, it's an intermediate result.
                            // Pass the raw pointer value directly.
                            temp_expr = ir_new_expr_raw_pointer(ptr, "void*");
                        }
                        break;
                    }
                }
                if (temp_expr) {
                    ir_expr_list_add(&temp_ir_args_head, temp_expr);
                }
            }

            // 3. Create a C-style array of IRNode* for the dispatcher from the linked list.
            IRNode** final_ir_args_array = arg_count > 0 ? calloc(arg_count, sizeof(IRNode*)) : NULL;
            if (arg_count > 0 && !final_ir_args_array) render_abort("Failed to alloc final_ir_args_array");

            i = 0;
            IRExprNode* current_arg_node = temp_ir_args_head;
            while(current_arg_node) {
                final_ir_args_array[i++] = (IRNode*)current_arg_node->expr;
                current_arg_node = current_arg_node->next;
            }

            // 4. Determine target and final arguments for the dispatcher
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

            // 5. Dispatch the call
            *out_val = dynamic_lvgl_call_ir(call->func_name, target_obj, dispatcher_args, dispatcher_arg_count, spec);

            // 6. Cleanup
            free(evaluated_args);
            // Free the temporary IR expression list we created
            ir_expr_list_add(&temp_ir_args_head, NULL); // Hack to call ir_free on the list
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

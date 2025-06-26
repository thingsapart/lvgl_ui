#include "codegen.h"
#include "api_spec.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h> // For malloc, free

// --- Forward Declarations ---
static void codegen_object(IRObject* obj, const ApiSpec* api_spec, IRRoot* ir_root, int indent, const char* parent_c_name);
static void codegen_expr(IRExpr* expr, const ApiSpec* api_spec, IRRoot* ir_root, IRObject* use_view_context_owner, const char* expected_c_type);
static void print_indent(int level);
static IRObject* clone_ir_object(IRObject* original);
static char* sanitize_c_identifier(const char* input_name);

// --- Main Entry Point ---
void codegen_generate_c(IRRoot* root, const ApiSpec* api_spec) {
    if (!root) {
        printf("// No IR root provided.\n");
        return;
    }
    IRObject* current_obj = root->root_objects;
    while (current_obj) {
        codegen_object(current_obj, api_spec, root, 1, "parent");
        current_obj = current_obj->next;
    }
}

// --- Core Codegen Logic ---
static void codegen_object(IRObject* obj, const ApiSpec* api_spec, IRRoot* ir_root, int indent, const char* parent_c_name) {
    if (!obj) return;

    if (obj->json_type && strcmp(obj->json_type, "use-view") == 0) {
        if (!obj->use_view_component_id) {
            fprintf(stderr, "Codegen Error: use-view object '%s' has no component ID.\n", obj->c_name);
            return;
        }
        IRComponent* comp_def = ir_root->components;
        while(comp_def) {
            if (strcmp(comp_def->id, obj->use_view_component_id) == 0) break;
            comp_def = comp_def->next;
        }
        if (!comp_def || !comp_def->root_widget) {
            fprintf(stderr, "Codegen Error: Component '%s' not found or is empty for use-view '%s'.\n", obj->use_view_component_id, obj->c_name);
            return;
        }
        IRObject* component_root_instance = clone_ir_object(comp_def->root_widget);
        if (!component_root_instance) {
            fprintf(stderr, "Codegen Error: Failed to clone component for use-view '%s'.\n", obj->c_name);
            return;
        }
        free(component_root_instance->c_name);
        component_root_instance->c_name = strdup(obj->c_name);
        if (obj->registered_id) {
            print_indent(indent);
            printf("obj_registry_add(\"%s\", (void*)%s);\n", obj->registered_id, component_root_instance->c_name);
        }
        IRProperty* override_prop = obj->properties;
        while(override_prop) {
            ir_property_list_add(&component_root_instance->properties, ir_new_property(override_prop->name, override_prop->value));
            override_prop = override_prop->next;
        }
        codegen_object(component_root_instance, api_spec, ir_root, indent, parent_c_name);
        ir_free((IRNode*)component_root_instance);
        return;
    }

    const WidgetDefinition* widget_def = api_spec_find_widget(api_spec, obj->json_type);
    bool is_widget_allocation = false;
    print_indent(indent);
    if (obj->json_type && strcmp(obj->json_type, "style") == 0 && widget_def && widget_def->c_type && widget_def->init_func) {
        printf("%s* %s = (%s*)malloc(sizeof(%s));\n", widget_def->c_type, obj->c_name, widget_def->c_type, widget_def->c_type);
        print_indent(indent);
        printf("if (%s != NULL) {\n", obj->c_name);
        print_indent(indent + 1);
        printf("memset(%s, 0, sizeof(%s));\n", obj->c_name, widget_def->c_type);
        print_indent(indent + 1);
        printf("%s(%s);\n", widget_def->init_func, obj->c_name);
        print_indent(indent);
        printf("} else {\n");
        print_indent(indent + 1);
        printf("fprintf(stderr, \"Error: Failed to malloc for object %s of type %s\\n\");\n", obj->c_name, widget_def->c_type);
        print_indent(indent);
        printf("}\n");
    } else if (widget_def && widget_def->create) {
        printf("lv_obj_t* %s = %s(%s);\n", obj->c_name, widget_def->create, parent_c_name ? parent_c_name : "lv_screen_active()");
        is_widget_allocation = true;
    } else if (widget_def && widget_def->c_type && widget_def->init_func) {
        printf("%s %s;\n", widget_def->c_type, obj->c_name);
        print_indent(indent);
        printf("%s(&%s);\n", widget_def->init_func, obj->c_name);
    } else {
        printf("lv_obj_t* %s = lv_obj_create(%s);\n", obj->c_name, parent_c_name ? parent_c_name : "lv_screen_active()");
        is_widget_allocation = true;
    }

    if (is_widget_allocation) {
        print_indent(indent);
        printf("if (%s == NULL) render_abort(\"Error: Failed to create widget '%s' (type: %s).\");\n", obj->c_name, obj->c_name, obj->json_type);
    }

    if (obj->registered_id) {
        print_indent(indent);
        printf("obj_registry_add(\"%s\", (void*)%s);\n", obj->registered_id, obj->c_name);
    }

    IRProperty* prop = obj->properties;
    int temp_arg_counter = 0;

    while (prop) {
        if (strcmp(prop->name, "size") == 0 && prop->value->type == IR_EXPR_ARRAY) {
            IRExprArray* arr = (IRExprArray*)prop->value;
            if (arr->elements && arr->elements->next && !arr->elements->next->next) {
                print_indent(indent);
                printf("lv_obj_set_size(%s, ", obj->c_name);
                codegen_expr(arr->elements->expr, api_spec, ir_root, obj, "lv_coord_t");
                printf(", ");
                codegen_expr(arr->elements->next->expr, api_spec, ir_root, obj, "lv_coord_t");
                printf(");\n");
                prop = prop->next;
                continue;
            }
        }

        char actual_c_function_name[128];
        strncpy(actual_c_function_name, prop->name, sizeof(actual_c_function_name) -1);
        actual_c_function_name[sizeof(actual_c_function_name)-1] = '\0';

        if (strcmp(prop->name, "add_style") == 0) {
            snprintf(actual_c_function_name, sizeof(actual_c_function_name), "lv_obj_add_style");
        } else if (strcmp(prop->name, "align") == 0 && obj->json_type && strcmp(obj->json_type, "obj")==0) {
             snprintf(actual_c_function_name, sizeof(actual_c_function_name), "lv_obj_align");
        }

        const FunctionDefinition* direct_func_def = api_spec_find_function(api_spec, actual_c_function_name);

        if (direct_func_def && prop->value->type == IR_EXPR_ARRAY) {
            IRExprArray* arr = (IRExprArray*)prop->value;
            print_indent(indent);
            printf("%s(%s", actual_c_function_name, obj->c_name);

            IRExprNode* arg_elem = arr->elements;
            const FunctionArg* func_arg_spec = NULL;
            if (direct_func_def->args_head) {
                const char* first_c_arg_type = direct_func_def->args_head->type;
                if (first_c_arg_type &&
                    (strcmp(first_c_arg_type, "lv_obj_t*") == 0 ||
                     strcmp(first_c_arg_type, "lv_style_t*") == 0 )) {
                    func_arg_spec = direct_func_def->args_head->next;
                } else {
                    func_arg_spec = direct_func_def->args_head;
                }
            }

            while(arg_elem) {
                printf(", ");
                const char* expected_arg_type = func_arg_spec ? func_arg_spec->type : NULL;
                codegen_expr(arg_elem->expr, api_spec, ir_root, obj, expected_arg_type);
                if (func_arg_spec) func_arg_spec = func_arg_spec->next;
                arg_elem = arg_elem->next;
            }
            printf(");\n");
            prop = prop->next;
            continue;
        }

        const PropertyDefinition* prop_def = api_spec_find_property(api_spec, obj->json_type, prop->name);
        char setter_func[128];
        char temp_setter_name[128];
        bool setter_found = false;

        if (prop_def && prop_def->setter) {
            strncpy(setter_func, prop_def->setter, sizeof(setter_func) - 1);
            setter_func[sizeof(setter_func) - 1] = '\0';
            setter_found = true;
        } else {
            snprintf(temp_setter_name, sizeof(temp_setter_name), "lv_%s_set_%s", obj->json_type, prop->name);
            if (api_spec_has_function(api_spec, temp_setter_name)) {
                strncpy(setter_func, temp_setter_name, sizeof(setter_func) -1);
                 setter_func[sizeof(setter_func)-1] = '\0';
                setter_found = true;
            } else {
                if (strcmp(obj->json_type, "obj") != 0) {
                    snprintf(temp_setter_name, sizeof(temp_setter_name), "lv_obj_set_%s", prop->name);
                    if (api_spec_has_function(api_spec, temp_setter_name)) {
                        strncpy(setter_func, temp_setter_name, sizeof(setter_func) - 1);
                        setter_func[sizeof(setter_func) - 1] = '\0';
                        setter_found = true;
                    }
                }
            }
            if (!setter_found) {
                snprintf(setter_func, sizeof(setter_func), "lv_%s_set_%s", obj->json_type, prop->name);
            }
        }

        char obj_ref_for_setter[128];
        if (widget_def && widget_def->init_func && strcmp(obj->json_type, "style") == 0) { // style is pointer
            snprintf(obj_ref_for_setter, sizeof(obj_ref_for_setter), "%s", obj->c_name);
        } else if (widget_def && widget_def->init_func) { // e.g. anim is stack, pass by address
            snprintf(obj_ref_for_setter, sizeof(obj_ref_for_setter), "&%s", obj->c_name);
        } else { // widget is pointer
            snprintf(obj_ref_for_setter, sizeof(obj_ref_for_setter), "%s", obj->c_name);
        }

        const FunctionArg* first_value_arg_details = NULL;
        if (prop_def && prop_def->func_args) {
             // If first C arg is obj* or style*, then JSON value corresponds to the *next* C arg
            if (prop_def->func_args->type &&
                (strcmp(prop_def->func_args->type, "lv_obj_t*") == 0 ||
                 strcmp(prop_def->func_args->type, "lv_style_t*") == 0)) {
                first_value_arg_details = prop_def->func_args->next;
            } else {
                first_value_arg_details = prop_def->func_args;
            }
        }

        bool is_true_no_arg_setter_scenario = false;
        if (prop_def && !first_value_arg_details && prop->value->type == IR_EXPR_LITERAL) {
            IRExprLiteral* lit = (IRExprLiteral*)prop->value;
            if (!lit->is_string && (strcmp(lit->value, "true") == 0 || strcmp(lit->value, "false") == 0)) {
                is_true_no_arg_setter_scenario = true;
            }
        }

        char args_c_code[1024] = "";
        bool has_args_to_print = false;

        if (first_value_arg_details) {
            has_args_to_print = true;

            if (prop->value->type == IR_EXPR_ARRAY && first_value_arg_details->next) {
                IRExprArray* arr = (IRExprArray*)prop->value;
                IRExprNode* elem_node = arr->elements;
                const FunctionArg* current_arg_spec = first_value_arg_details;

                while(elem_node && current_arg_spec) {
                    strcat(args_c_code, ", ");

                    const char* arg_c_type = current_arg_spec->type;
                    char temp_arg_var_name[64];
                    snprintf(temp_arg_var_name, sizeof(temp_arg_var_name), "_arg_val_%d", temp_arg_counter++);

                    print_indent(indent);
                    printf("%s %s = ", arg_c_type ? arg_c_type : "void*", temp_arg_var_name);
                    codegen_expr(elem_node->expr, api_spec, ir_root, obj, arg_c_type);
                    printf(";\n");

                    if (arg_c_type && (strcmp(arg_c_type, "lv_obj_t*") == 0 || strcmp(arg_c_type, "lv_style_t*") == 0)) {
                        bool is_literal_null = (elem_node->expr->type == IR_EXPR_LITERAL &&
                                                !((IRExprLiteral*)elem_node->expr)->is_string &&
                                                strcmp(((IRExprLiteral*)elem_node->expr)->value, "NULL") == 0);
                        if (!is_literal_null) {
                            print_indent(indent);
                            printf("if (%s == NULL) render_abort(\"Error: Argument for '%s' in '%s' is NULL (property: %s, object: %s).\");\n",
                                   temp_arg_var_name, current_arg_spec->name ? current_arg_spec->name : "unknown", setter_func, prop->name, obj->c_name);
                        }
                    }
                    strcat(args_c_code, temp_arg_var_name);

                    elem_node = elem_node->next;
                    current_arg_spec = current_arg_spec->next;
                }
            } else {
                const char* arg_c_type = first_value_arg_details->type;
                char temp_arg_var_name[64];
                snprintf(temp_arg_var_name, sizeof(temp_arg_var_name), "_arg_val_%d", temp_arg_counter++);

                print_indent(indent);
                printf("%s %s = ", arg_c_type ? arg_c_type : "void*", temp_arg_var_name);
                codegen_expr(prop->value, api_spec, ir_root, obj, arg_c_type);
                printf(";\n");

                if (arg_c_type && (strcmp(arg_c_type, "lv_obj_t*") == 0 || strcmp(arg_c_type, "lv_style_t*") == 0)) {
                     bool is_literal_null = (prop->value->type == IR_EXPR_LITERAL &&
                                             !((IRExprLiteral*)prop->value)->is_string &&
                                             strcmp(((IRExprLiteral*)prop->value)->value, "NULL") == 0);
                    if (!is_literal_null) {
                        print_indent(indent);
                        printf("if (%s == NULL) render_abort(\"Error: Argument '%s' for '%s' is NULL (property: %s, object: %s).\");\n",
                               temp_arg_var_name, first_value_arg_details->name ? first_value_arg_details->name : "unknown", setter_func, prop->name, obj->c_name);
                    }
                }
                strcat(args_c_code, ", ");
                strcat(args_c_code, temp_arg_var_name);
            }
        }

        if (strcmp(prop->name, "style") == 0 && prop->value->type == IR_EXPR_REGISTRY_REF) {
            print_indent(indent);
            char style_ptr_var[64];
            snprintf(style_ptr_var, sizeof(style_ptr_var), "_style_ptr_%d", temp_arg_counter++);
            printf("lv_style_t* %s = (lv_style_t*)", style_ptr_var);
            codegen_expr(prop->value, api_spec, ir_root, obj, "lv_style_t*");
            printf(";\n");
            print_indent(indent);
            printf("if (%s != NULL) {\n", style_ptr_var);
            print_indent(indent + 1);
            printf("lv_obj_add_style(%s, %s, 0);\n", obj->c_name, style_ptr_var);
            print_indent(indent);
            printf("} else {\n");
            print_indent(indent + 1);
            printf("fprintf(stderr, \"Warning: Style '%s' not found in registry for object '%s'.\\n\");\n",
                   ((IRExprRegistryRef*)prop->value)->name, obj->c_name);
            print_indent(indent);
            printf("}\n");
        } else if (is_true_no_arg_setter_scenario) {
            bool call_the_setter_for_toggle = (prop->value->type == IR_EXPR_LITERAL && strcmp(((IRExprLiteral*)prop->value)->value, "true") == 0);
            if (call_the_setter_for_toggle) {
                print_indent(indent);
                printf("%s(%s);\n", setter_func, obj_ref_for_setter);
            } else {
                print_indent(indent);
                printf("/* Property '%s' (value: ", prop->name);
                codegen_expr(prop->value, api_spec, ir_root, obj, NULL);
                printf(") not called for %s (setter expects no value args, and value was not 'true') */\n", obj->c_name);
            }
        } else {
            print_indent(indent);
            if (has_args_to_print) {
                printf("%s(%s%s);\n", setter_func, obj_ref_for_setter, args_c_code);
            } else {
                printf("%s(%s);\n", setter_func, obj_ref_for_setter);
            }
        }
        prop = prop->next;
    }

    IRWithBlock* wb = obj->with_blocks;
    while(wb) {
        char temp_var_name[64];
        snprintf(temp_var_name, sizeof(temp_var_name), "with_target_%s", obj->c_name);
        print_indent(indent);
        printf("{\n");
        print_indent(indent + 1);
        printf("void* %s = ", temp_var_name);
        codegen_expr(wb->target_expr, api_spec, ir_root, obj, NULL);
        printf(";\n");
        IRProperty* with_prop = wb->properties;
        while(with_prop) {
            const char* target_obj_type_for_with = "obj";
            const PropertyDefinition* with_prop_def = api_spec_find_property(api_spec, target_obj_type_for_with, with_prop->name);
            char setter_func[128];
            if (with_prop_def && with_prop_def->setter) {
                strncpy(setter_func, with_prop_def->setter, sizeof(setter_func) - 1);
                setter_func[sizeof(setter_func) - 1] = '\0';
            } else {
                snprintf(setter_func, sizeof(setter_func), "lv_%s_set_%s", target_obj_type_for_with, with_prop->name);
            }
            const FunctionArg* with_first_value_arg_details = NULL;
            if (with_prop_def && with_prop_def->func_args && with_prop_def->func_args->next) {
                with_first_value_arg_details = with_prop_def->func_args->next;
            }
            char with_args_c_code[512] = "";
            bool with_has_args_to_print = false;
            if (with_first_value_arg_details) {
                strcat(with_args_c_code, ", ");
                with_has_args_to_print = true;
                const char* with_arg_c_type = with_first_value_arg_details->type;
                bool with_needs_null_check = with_arg_c_type && (strcmp(with_arg_c_type, "lv_obj_t*") == 0 || strcmp(with_arg_c_type, "lv_style_t*") == 0);
                char temp_with_arg_var_name[64];
                snprintf(temp_with_arg_var_name, sizeof(temp_with_arg_var_name), "_with_arg_val_%d", temp_arg_counter++);
                print_indent(indent + 1);
                printf("%s %s = ", with_arg_c_type ? with_arg_c_type : "void*", temp_with_arg_var_name);
                codegen_expr(with_prop->value, api_spec, ir_root, obj, with_arg_c_type);
                printf(";\n");
                if (with_needs_null_check) {
                     bool is_literal_null = (with_prop->value->type == IR_EXPR_LITERAL &&
                                             !((IRExprLiteral*)with_prop->value)->is_string &&
                                             strcmp(((IRExprLiteral*)with_prop->value)->value, "NULL") == 0);
                    if (!is_literal_null) {
                        print_indent(indent + 1);
                        printf("if (%s == NULL) render_abort(\"Error: Argument for '%s' in 'with' block call '%s' is NULL (property: %s, object: %s).\");\n",
                               temp_with_arg_var_name, with_first_value_arg_details->name ? with_first_value_arg_details->name : "unknown", setter_func, with_prop->name, obj->c_name);
                    }
                }
                strcat(with_args_c_code, temp_with_arg_var_name);
            }
            print_indent(indent + 1);
            if (with_has_args_to_print) {
                printf("%s((lv_obj_t*)%s%s);\n", setter_func, temp_var_name, with_args_c_code);
            } else {
                printf("%s((lv_obj_t*)%s);\n", setter_func, temp_var_name);
            }
            with_prop = with_prop->next;
        }
        if (wb->children_root) {
            IRObject* child = wb->children_root->children;
            while(child) {
                codegen_object(child, api_spec, ir_root, indent + 1, temp_var_name);
                child = child->next;
            }
        }
        print_indent(indent);
        printf("}\n");
        wb = wb->next;
    }

    if (obj->children) {
        print_indent(indent);
        printf("\n");
        IRObject* child = obj->children;
        while (child) {
            codegen_object(child, api_spec, ir_root, indent, obj->c_name);
            child = child->next;
        }
    }
}

static void codegen_expr(IRExpr* expr, const ApiSpec* api_spec, IRRoot* ir_root, IRObject* use_view_context_owner, const char* expected_c_type) {
    if (!expr) {
        printf("NULL");
        return;
    }
    switch(expr->type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)expr;
            if (lit->is_string) printf("\"%s\"", lit->value);
            else printf("%s", lit->value);
            break;
        }
        case IR_EXPR_STATIC_STRING:
            printf("\"%s\"", ((IRExprStaticString*)expr)->value);
            break;
        case IR_EXPR_ENUM:
            printf("%s", ((IRExprEnum*)expr)->symbol);
            break;
        case IR_EXPR_REGISTRY_REF: {
            IRExprRegistryRef* reg_ref_expr = (IRExprRegistryRef*)expr;
            const char* ref_name = reg_ref_expr->name;
            if (ref_name && ref_name[0] == '@') {
                const char* id_to_lookup = ref_name + 1;
                if (expected_c_type && strchr(expected_c_type, '*')) {
                     printf("(%s)", expected_c_type);
                }
                printf("obj_registry_get(\"%s\")", id_to_lookup);
            } else {
                fprintf(stderr, "Warning: Registry reference '%s' does not start with '@'. Generating as raw name.\n", ref_name ? ref_name : "NULL");
                printf("%s", ref_name ? ref_name : "NULL");
            }
            break;
        }
        case IR_EXPR_CONTEXT_VAR: {
            const char* var_name = ((IRExprContextVar*)expr)->name;
            if (use_view_context_owner && use_view_context_owner->use_view_context) {
                IRProperty* ctx_prop = use_view_context_owner->use_view_context;
                while(ctx_prop) {
                    if (strcmp(ctx_prop->name, var_name) == 0) {
                        codegen_expr(ctx_prop->value, api_spec, ir_root, use_view_context_owner, expected_c_type);
                        return;
                    }
                    ctx_prop = ctx_prop->next;
                }
            }
            printf("/* Context var '%s' not found */ NULL", var_name);
            break;
        }
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;
            printf("%s(", call->func_name);
            IRExprNode* arg = call->args;
            const FunctionArg* first_arg_for_call = api_spec_get_function_args_by_name(api_spec, call->func_name);
            const FunctionArg* current_func_arg_type = first_arg_for_call;
            while(arg) {
                codegen_expr(arg->expr, api_spec, ir_root, use_view_context_owner, current_func_arg_type ? current_func_arg_type->type : NULL);
                if (arg->next) printf(", ");
                if (current_func_arg_type) current_func_arg_type = current_func_arg_type->next;
                arg = arg->next;
            }
            printf(")");
            break;
        }
        case IR_EXPR_ARRAY: {
            IRExprArray* arr = (IRExprArray*)expr;
            IRExprNode* elem = arr->elements;
            printf("{ ");
            bool first = true;
            while(elem) {
                if (!first) printf(", ");
                codegen_expr(elem->expr, api_spec, ir_root, use_view_context_owner, NULL);
                first = false;
                elem = elem->next;
            }
            printf(" }");
            break;
        }
        default:
            printf("/* unhandled expr type %d */", expr->type);
    }
}

static void print_indent(int level) {
    for (int i = 0; i < level; ++i) {
        printf("    ");
    }
}

static char* sanitize_c_identifier(const char* input_name) {
    if (!input_name || *input_name == '\0') return strdup("unnamed_var");
    size_t len = strlen(input_name);
    char* sanitized = malloc(len + 2);
    if (!sanitized) return strdup("oom_var");
    char* s_ptr = sanitized;
    const char* i_ptr = input_name;
    if (isdigit((unsigned char)*i_ptr)) {
        *s_ptr++ = '_';
    }
    while(*i_ptr) {
        if (isalnum((unsigned char)*i_ptr)) {
            *s_ptr++ = *i_ptr;
        } else if (s_ptr > sanitized && *(s_ptr-1) != '_') {
            *s_ptr++ = '_';
        }
        i_ptr++;
    }
    *s_ptr = '\0';
    return sanitized;
}

static IRExpr* clone_ir_expr(IRExpr* original_expr);
static IRProperty* clone_ir_property_list(IRProperty* original_props);
static IRObject* clone_ir_object_list(IRObject* original_objs);
static IRWithBlock* clone_ir_with_block_list(IRWithBlock* original_blocks);

static IRExprNode* clone_ir_expr_list(IRExprNode* original_expr_list) {
    if (!original_expr_list) return NULL;
    IRExprNode* head = NULL;
    IRExprNode* current_new = NULL;
    IRExprNode* current_original = original_expr_list;
    while (current_original) {
        IRExpr* cloned_expr = clone_ir_expr(current_original->expr);
        IRExprNode* new_node = calloc(1, sizeof(IRExprNode));
        new_node->expr = cloned_expr;
        new_node->next = NULL;
        if (!head) {
            head = new_node;
            current_new = head;
        } else {
            current_new->next = new_node;
            current_new = new_node;
        }
        current_original = current_original->next;
    }
    return head;
}

static IRExpr* clone_ir_expr(IRExpr* original_expr) {
    if (!original_expr) return NULL;
    switch (original_expr->type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* orig_lit = (IRExprLiteral*)original_expr;
            IRExprLiteral* new_lit = (IRExprLiteral*)ir_new_expr_literal(orig_lit->value);
            new_lit->is_string = orig_lit->is_string;
            if (orig_lit->is_string) {
                free(new_lit);
                new_lit = (IRExprLiteral*)ir_new_expr_literal_string(orig_lit->value);
            }
            return (IRExpr*)new_lit;
        }
        case IR_EXPR_STATIC_STRING:
            return ir_new_expr_static_string(((IRExprStaticString*)original_expr)->value);
        case IR_EXPR_ENUM:
            return ir_new_expr_enum(((IRExprEnum*)original_expr)->symbol, ((IRExprEnum*)original_expr)->value);
        case IR_EXPR_REGISTRY_REF:
            return ir_new_expr_registry_ref(((IRExprRegistryRef*)original_expr)->name);
        case IR_EXPR_CONTEXT_VAR:
            return ir_new_expr_context_var(((IRExprContextVar*)original_expr)->name);
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* orig_call = (IRExprFunctionCall*)original_expr;
            IRExprNode* cloned_args = clone_ir_expr_list(orig_call->args);
            return ir_new_expr_func_call(orig_call->func_name, cloned_args);
        }
        case IR_EXPR_ARRAY: {
            IRExprArray* orig_arr = (IRExprArray*)original_expr;
            IRExprNode* cloned_elements = clone_ir_expr_list(orig_arr->elements);
            return ir_new_expr_array(cloned_elements);
        }
        default:
            fprintf(stderr, "clone_ir_expr: Unknown expression type %d\n", original_expr->type);
            return NULL;
    }
}

static IRProperty* clone_ir_property_list(IRProperty* original_props) {
    if (!original_props) return NULL;
    IRProperty* head = NULL;
    IRProperty* current_new = NULL;
    IRProperty* current_original = original_props;
    while (current_original) {
        IRExpr* cloned_value = clone_ir_expr(current_original->value);
        IRProperty* new_prop = ir_new_property(current_original->name, cloned_value);
        if (!head) {
            head = new_prop;
            current_new = head;
        } else {
            current_new->next = new_prop;
            current_new = new_prop;
        }
        current_original = current_original->next;
    }
    return head;
}

static IRObject* clone_ir_object(IRObject* original);

static IRWithBlock* clone_ir_with_block_list(IRWithBlock* original_blocks) {
    if (!original_blocks) return NULL;
    IRWithBlock* head = NULL;
    IRWithBlock* current_new = NULL;
    IRWithBlock* current_original = original_blocks;
    while (current_original) {
        IRExpr* cloned_target_expr = clone_ir_expr(current_original->target_expr);
        IRProperty* cloned_properties = clone_ir_property_list(current_original->properties);
        IRObject* cloned_children_root = clone_ir_object(current_original->children_root);
        IRWithBlock* new_block = ir_new_with_block(cloned_target_expr, cloned_properties, cloned_children_root);

        if (!head) {
            head = new_block;
            current_new = head;
        } else {
            current_new->next = new_block;
            current_new = new_block;
        }
        current_original = current_original->next;
    }
    return head;
}

static IRObject* clone_ir_object_list(IRObject* original_objs) {
    if (!original_objs) return NULL;
    IRObject* head = NULL;
    IRObject* current_new = NULL;
    IRObject* current_original = original_objs;
    while (current_original) {
        IRObject* new_obj = clone_ir_object(current_original);
        if (!head) {
            head = new_obj;
            current_new = head;
        } else {
            current_new->next = new_obj;
            current_new = new_obj;
        }
        current_original = current_original->next;
    }
    return head;
}

// Deep clone for IRObject
static IRObject* clone_ir_object(IRObject* original) {
    if (!original) return NULL;
    IRObject* clone = ir_new_object(original->c_name, original->json_type, original->registered_id);

    if (original->use_view_component_id) {
        clone->use_view_component_id = strdup(original->use_view_component_id);
    }
    clone->use_view_context = clone_ir_property_list(original->use_view_context);
    clone->properties = clone_ir_property_list(original->properties);
    clone->children = clone_ir_object_list(original->children);
    clone->with_blocks = clone_ir_with_block_list(original->with_blocks);

    return clone;
}

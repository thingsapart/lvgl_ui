#include "codegen.h"
#include "api_spec.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h> // For malloc, free

// --- Forward Declarations ---
static void codegen_object(IRObject* obj, const ApiSpec* api_spec, IRRoot* ir_root, int indent, const char* parent_c_name);
// Added expected_c_type for casting registry lookups and other type-aware codegen
static void codegen_expr(IRExpr* expr, const ApiSpec* api_spec, IRRoot* ir_root, IRObject* use_view_context_owner, const char* expected_c_type);
static void print_indent(int level);
static IRObject* clone_ir_object(IRObject* original); // Helper for use-view
static char* sanitize_c_identifier(const char* input_name);

// --- Main Entry Point ---
void codegen_generate_c(IRRoot* root, const ApiSpec* api_spec) {
    if (!root) {
        printf("// No IR root provided.\n");
        return;
    }

    // Process all top-level objects, assuming they are parented to `parent`
    // which should be the main screen/container passed to the UI creation function.
    IRObject* current_obj = root->root_objects;
    while (current_obj) {
        codegen_object(current_obj, api_spec, root, 1, "parent");
        current_obj = current_obj->next;
    }
}

// --- Core Codegen Logic ---

static void codegen_object(IRObject* obj, const ApiSpec* api_spec, IRRoot* ir_root, int indent, const char* parent_c_name) {
    if (!obj) return;

    // Handle 'use-view' by finding and inlining the component
    if (obj->json_type && strcmp(obj->json_type, "use-view") == 0) {
        if (!obj->use_view_component_id) {
            fprintf(stderr, "Codegen Error: use-view object '%s' has no component ID.\n", obj->c_name);
            return;
        }

        // Find component definition in the IR
        IRComponent* comp_def = ir_root->components;
        while(comp_def) {
            if (strcmp(comp_def->id, obj->use_view_component_id) == 0) break;
            comp_def = comp_def->next;
        }

        if (!comp_def || !comp_def->root_widget) {
            fprintf(stderr, "Codegen Error: Component '%s' not found or is empty for use-view '%s'.\n", obj->use_view_component_id, obj->c_name);
            return;
        }

        // Clone the component's root widget to avoid mutating the original IR.
        IRObject* component_root_instance = clone_ir_object(comp_def->root_widget);
        if (!component_root_instance) {
            fprintf(stderr, "Codegen Error: Failed to clone component for use-view '%s'.\n", obj->c_name);
            return;
        }

        // The instance takes the C variable name of the use-view object.
        free(component_root_instance->c_name);
        component_root_instance->c_name = strdup(obj->c_name); // obj->c_name is from use-view's "named" or generated

        // If the use-view node itself was named, register ITS c_name with ITS registered_id.
        // The component_root_instance now effectively IS the use-view instance.
        if (obj->registered_id) {
            print_indent(indent);
            // Use component_root_instance->c_name because that's the actual C variable for the component's root obj.
            printf("obj_registry_add(\"%s\", (void*)%s);\n", obj->registered_id, component_root_instance->c_name);
        }

        // Apply properties from the use-view node as overrides.
        IRProperty* override_prop = obj->properties;
        while(override_prop) {
            // This is a simple append; a real system might replace existing properties.
            ir_property_list_add(&component_root_instance->properties, ir_new_property(override_prop->name, override_prop->value));
            override_prop = override_prop->next;
        }

        codegen_object(component_root_instance, api_spec, ir_root, indent, parent_c_name);

        ir_free((IRNode*)component_root_instance); // Clean up the cloned instance
        return;
    }


    // --- Object Creation ---
    const WidgetDefinition* widget_def = api_spec_find_widget(api_spec, obj->json_type);
    print_indent(indent);
    if (widget_def && widget_def->create) { // It's a standard widget
        printf("lv_obj_t* %s = %s(%s);\n", obj->c_name, widget_def->create, parent_c_name ? parent_c_name : "lv_screen_active()");
    } else if (widget_def && widget_def->c_type && widget_def->init_func) { // It's an object like a style
        printf("%s %s;\n", widget_def->c_type, obj->c_name);
        print_indent(indent);
        printf("%s(&%s);\n", widget_def->init_func, obj->c_name);
    } else { // Fallback to generic object
        printf("lv_obj_t* %s = lv_obj_create(%s);\n", obj->c_name, parent_c_name ? parent_c_name : "lv_screen_active()");
    }

    // If the object has a registered ID, add it to the C-side registry
    if (obj->registered_id) {
        print_indent(indent);
        // Cast to void* to be compatible with the generic registry function
        printf("obj_registry_add(\"%s\", (void*)%s);\n", obj->registered_id, obj->c_name);
    }

    // --- Properties ---
    IRProperty* prop = obj->properties;
    while (prop) {
        const PropertyDefinition* prop_def = api_spec_find_property(api_spec, obj->json_type, prop->name);

        char setter_func[128];
        char temp_setter_name[128];
        bool setter_found = false;

        if (prop_def && prop_def->setter) {
            strncpy(setter_func, prop_def->setter, sizeof(setter_func) - 1);
            setter_func[sizeof(setter_func) - 1] = '\0';
            setter_found = true;
        } else {
            // Try lv_<type>_set_<prop>
            snprintf(temp_setter_name, sizeof(temp_setter_name), "lv_%s_set_%s", obj->json_type, prop->name);
            if (api_spec_has_function(api_spec, temp_setter_name)) {
                strncpy(setter_func, temp_setter_name, sizeof(setter_func) -1);
                setter_func[sizeof(setter_func) -1] = '\0';
                setter_found = true;
            } else {
                // Fallback: if not an "obj" type already, try lv_obj_set_<prop>
                if (strcmp(obj->json_type, "obj") != 0) {
                    snprintf(temp_setter_name, sizeof(temp_setter_name), "lv_obj_set_%s", prop->name);
                    if (api_spec_has_function(api_spec, temp_setter_name)) {
                        strncpy(setter_func, temp_setter_name, sizeof(setter_func) - 1);
                        setter_func[sizeof(setter_func) - 1] = '\0';
                        setter_found = true;
                    }
                }
            }

            // If still not found, use the original constructed name as a last resort
            if (!setter_found) {
                snprintf(setter_func, sizeof(setter_func), "lv_%s_set_%s", obj->json_type, prop->name);
            }
        }

        print_indent(indent);

        char obj_ref_for_setter[128];
        if (widget_def && widget_def->init_func) {
            snprintf(obj_ref_for_setter, sizeof(obj_ref_for_setter), "&%s", obj->c_name);
        } else {
            snprintf(obj_ref_for_setter, sizeof(obj_ref_for_setter), "%s", obj->c_name);
        }

        printf("%s(%s", setter_func, obj_ref_for_setter);

        const FunctionArg* first_value_arg_details = NULL;
        if (prop_def && prop_def->func_args) {
            if (prop_def->func_args->next) {
                 first_value_arg_details = prop_def->func_args->next;
            }
        }

        bool called_with_args = false;
        if (first_value_arg_details) {
            printf(", ");
            if (prop->value->type == IR_EXPR_ARRAY) {
                IRExprArray* arr = (IRExprArray*)prop->value;
                IRExprNode* elem = arr->elements;
                const FunctionArg* current_arg_details_iter = first_value_arg_details;
                bool first_array_elem = true;
                while(elem) {
                    if (!first_array_elem) printf(", ");
                    codegen_expr(elem->expr, api_spec, ir_root, obj, current_arg_details_iter ? current_arg_details_iter->type : NULL);
                    first_array_elem = false;
                    if (current_arg_details_iter) current_arg_details_iter = current_arg_details_iter->next;
                    elem = elem->next;
                }
            } else {
                codegen_expr(prop->value, api_spec, ir_root, obj, first_value_arg_details->type);
            }
            called_with_args = true;
        } else if (prop_def && !first_value_arg_details) {
            // No value args as per prop_def (e.g. lv_obj_move_foreground)
            // This path is taken, call_setter will determine if a call is made.
        } else {
            // No prop_def, or prop_def indicates no value args, but value is not simple true/false.
            // Fallback to trying to print the value as an argument if it's not literally NULL.
            if (prop->value->type != IR_EXPR_LITERAL || strcmp(((IRExprLiteral*)prop->value)->value, "NULL") != 0 ||
                (prop->value->type == IR_EXPR_LITERAL && ((IRExprLiteral*)prop->value)->is_string)) {
                 printf(", ");
                 codegen_expr(prop->value, api_spec, ir_root, obj, NULL);
                 called_with_args = true;
            }
        }

        if (prop_def && !first_value_arg_details) { // This is the no-arg setter path
            bool call_the_setter_no_args = false;
            if (prop->value->type == IR_EXPR_LITERAL) {
                IRExprLiteral* lit = (IRExprLiteral*)prop->value;
                if (!lit->is_string && ((strcmp(lit->value, "true") == 0) || (strcmp(lit->value, "0") != 0 && atoi(lit->value) != 0))) {
                    call_the_setter_no_args = true;
                }
            } else if (prop->value->type == IR_EXPR_ENUM) {
                 if (strcmp(((IRExprEnum*)prop->value)->symbol, "false") != 0 && strcmp(((IRExprEnum*)prop->value)->symbol, "0") != 0 ) {
                    call_the_setter_no_args = true;
                 }
            }
            if(call_the_setter_no_args) {
                 printf(");\n"); // Close the parenthesis from printf("%s(%s", setter_func, obj_ref_for_setter);
            } else {
                 // Effectively comment out the call by not closing the parenthesis and adding a comment.
                 printf(" /* Property '%s' (value: ", prop->name);
                 codegen_expr(prop->value, api_spec, ir_root, obj, NULL);
                 printf(") not called for %s (setter expects no value args, and value was false-ish) */\n", obj->c_name);
            }
        } else { // Has args, or ambiguous case that printed args
             printf(");\n");
        }
        prop = prop->next;
    }

    // --- 'with' blocks ---
    IRWithBlock* wb = obj->with_blocks;
    while(wb) {
        char temp_var_name[64];
        snprintf(temp_var_name, sizeof(temp_var_name), "with_target_%s", obj->c_name);

        print_indent(indent);
        printf("{\n");
        print_indent(indent + 1);
        printf("void* %s = ", temp_var_name);
        codegen_expr(wb->target_expr, api_spec, ir_root, obj, NULL); // Pass NULL for expected_c_type for the target expr
        printf(";\n");

        IRProperty* with_prop = wb->properties;
        while(with_prop) {
            char setter_func[128];
            // This assumes all 'with' targets are lv_obj_t for property setting.
            // TODO: This could be more robust by checking actual type of with_target_dummy_with_1 if known
            snprintf(setter_func, sizeof(setter_func), "lv_obj_set_%s", with_prop->name);
            print_indent(indent + 1);
            printf("%s(%s, ", setter_func, temp_var_name);
            // For properties inside 'with', we'd ideally get expected_c_type from the setter_func.
            // For now, passing NULL as this is complex to look up for dynamically constructed setters.
            codegen_expr(with_prop->value, api_spec, ir_root, obj, NULL);
            printf(");\n");
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

    // --- Children ---
    if (obj->children) {
        print_indent(indent);
        printf("\n"); // Spacer
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
        case IR_EXPR_REGISTRY_REF:
        {
            const char* ref_name = ((IRExprRegistryRef*)expr)->name;
            if (ref_name && ref_name[0] == '@') {
                if (expected_c_type && strchr(expected_c_type, '*')) {
                    printf("(%s)", expected_c_type); // Print cast, e.g. (lv_style_t*)
                }
                printf("obj_registry_get(\"%s\")", ref_name + 1); // Skip the '@'
            } else {
                fprintf(stderr, "Warning: Registry reference '%s' does not start with '@'. Generating as raw name.\n", ref_name ? ref_name : "NULL");
                printf("%s", ref_name ? ref_name : "NULL"); // Might lead to compile error if var undefined
            }
            break;
        }
        case IR_EXPR_CONTEXT_VAR: {
            const char* var_name = ((IRExprContextVar*)expr)->name;
            if (use_view_context_owner && use_view_context_owner->use_view_context) {
                IRProperty* ctx_prop = use_view_context_owner->use_view_context;
                while(ctx_prop) {
                    if (strcmp(ctx_prop->name, var_name) == 0) {
                        // When resolving context var, we don't know its expected type in *this* specific usage,
                        // but the original 'expected_c_type' for the outer property still applies somewhat.
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
            // This case is now primarily for array initializers like { val1, val2 }
            // If an array is used for multiple function arguments, codegen_object's property loop handles it.
            IRExprArray* arr = (IRExprArray*)expr;
            IRExprNode* elem = arr->elements;
            printf("{ ");
            bool first = true;
            while(elem) {
                if (!first) printf(", ");
                // For elements of an array initializer, expected_c_type of the outer context doesn't apply to individual elements.
                // Pass NULL for expected_c_type for elements.
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

// --- Helpers ---
static void print_indent(int level) {
    for (int i = 0; i < level; ++i) {
        printf("    "); // 4 spaces
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
            new_lit->is_string = orig_lit->is_string; // ir_new_expr_literal sets is_string to false by default
            if (orig_lit->is_string) { // If original was string literal, use the right constructor
                free(new_lit); // free the one from ir_new_expr_literal
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


static IRObject* clone_ir_object(IRObject* original); // Forward declare for mutual recursion if needed by with_blocks or children

static IRWithBlock* clone_ir_with_block_list(IRWithBlock* original_blocks) {
    if (!original_blocks) return NULL;
    IRWithBlock* head = NULL;
    IRWithBlock* current_new = NULL;
    IRWithBlock* current_original = original_blocks;
    while (current_original) {
        IRExpr* cloned_target_expr = clone_ir_expr(current_original->target_expr);
        IRProperty* cloned_properties = clone_ir_property_list(current_original->properties);
        IRObject* cloned_children_root = clone_ir_object(current_original->children_root); // Recursive call
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
        IRObject* new_obj = clone_ir_object(current_original); // Recursive call
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

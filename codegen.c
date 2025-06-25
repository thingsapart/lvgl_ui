#include "codegen.h"
#include "api_spec.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h> // For malloc, free

// --- Forward Declarations ---
static void codegen_object(IRObject* obj, const ApiSpec* api_spec, IRRoot* ir_root, int indent, const char* parent_c_name);
static void codegen_expr(IRExpr* expr, const ApiSpec* api_spec, IRRoot* ir_root, IRObject* use_view_context_owner);
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

        // The instance takes the name of the use-view object.
        free(component_root_instance->c_name);
        component_root_instance->c_name = strdup(obj->c_name);

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
        if (prop_def && prop_def->setter) {
            strncpy(setter_func, prop_def->setter, sizeof(setter_func) - 1);
            setter_func[sizeof(setter_func) - 1] = '\0';
        } else {
            // Construct a plausible setter name
            snprintf(setter_func, sizeof(setter_func), "lv_%s_set_%s", obj->json_type, prop->name);
        }

        print_indent(indent);
        // Handle pointer vs value-type objects correctly
        if (widget_def && widget_def->init_func) {
            // It's a value type (e.g. lv_style_t), pass by address
            printf("%s(&%s, ", setter_func, obj->c_name);
        } else {
            // It's a pointer type (e.g. lv_obj_t*), pass by value
            printf("%s(%s, ", setter_func, obj->c_name);
        }

        codegen_expr(prop->value, api_spec, ir_root, obj); // Pass 'obj' as context for $vars
        printf(");\n");

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
        codegen_expr(wb->target_expr, api_spec, ir_root, obj);
        printf(";\n");

        IRProperty* with_prop = wb->properties;
        while(with_prop) {
            char setter_func[128];
            // This assumes all 'with' targets are lv_obj_t for property setting.
            snprintf(setter_func, sizeof(setter_func), "lv_obj_set_%s", with_prop->name);
            print_indent(indent + 1);
            printf("%s(%s, ", setter_func, temp_var_name);
            codegen_expr(with_prop->value, api_spec, ir_root, obj);
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

static void codegen_expr(IRExpr* expr, const ApiSpec* api_spec, IRRoot* ir_root, IRObject* use_view_context_owner) {
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
            char* c_var_name = sanitize_c_identifier(((IRExprRegistryRef*)expr)->name);
            printf("%s", c_var_name);
            free(c_var_name);
            break;
        }
        case IR_EXPR_CONTEXT_VAR: {
            const char* var_name = ((IRExprContextVar*)expr)->name;
            if (use_view_context_owner && use_view_context_owner->use_view_context) {
                IRProperty* ctx_prop = use_view_context_owner->use_view_context;
                while(ctx_prop) {
                    if (strcmp(ctx_prop->name, var_name) == 0) {
                        codegen_expr(ctx_prop->value, api_spec, ir_root, use_view_context_owner);
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
            while(arg) {
                codegen_expr(arg->expr, api_spec, ir_root, use_view_context_owner);
                if (arg->next) printf(", ");
                arg = arg->next;
            }
            printf(")");
            break;
        }
        case IR_EXPR_ARRAY: {
            IRExprArray* arr = (IRExprArray*)expr;
            printf("{ ");
            IRExprNode* elem = arr->elements;
            while(elem) {
                codegen_expr(elem->expr, api_spec, ir_root, use_view_context_owner);
                if (elem->next) printf(", ");
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

// NOTE: This is a shallow clone. A full deep clone is needed for robust component use.
static IRObject* clone_ir_object(IRObject* original) {
    if (!original) return NULL;
    IRObject* clone = ir_new_object(original->c_name, original->json_type, original->registered_id);

    // A real implementation would deeply clone properties, with_blocks, and children here.
    // For this example, we are sharing pointers, which is not safe in general but will work
    // for a simple, single-pass codegen.
    clone->properties = original->properties;
    clone->children = original->children;
    clone->with_blocks = original->with_blocks;

    return clone;
}

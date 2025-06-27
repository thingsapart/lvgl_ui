#include "ir_printer.h"
#include "ir.h"
#include <stdio.h>

// --- Forward Declarations ---
static void print_indent(int level);
static void print_expr(IRExpr* expr);
static void print_property_list(IRProperty* head, int indent_level);
static void print_object_list(IRObject* head, int indent_level);
static void print_with_block_list(IRWithBlock* head, int indent_level);

// --- Helper Functions ---

static void print_indent(int level) {
    for (int i = 0; i < level; ++i) {
        printf("  "); // 2 spaces per indent level
    }
}

static void print_expr(IRExpr* expr) {
    if (!expr) {
        printf("NULL_EXPR");
        return;
    }
    switch(expr->type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)expr;
            if (lit->is_string) printf("\"%s\"", lit->value);
            else printf("%s", lit->value);
            break;
        }
        case IR_EXPR_STATIC_STRING: {
            printf("!\"%s\"", ((IRExprStaticString*)expr)->value);
            break;
        }
        case IR_EXPR_ENUM: {
            printf("%s", ((IRExprEnum*)expr)->symbol);
            break;
        }
        case IR_EXPR_REGISTRY_REF: {
            printf("%s", ((IRExprRegistryRef*)expr)->name);
            break;
        }
        case IR_EXPR_CONTEXT_VAR: {
            printf("%s", ((IRExprContextVar*)expr)->name);
            break;
        }
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;
            printf("%s(", call->func_name);
            IRExprNode* arg_node = call->args;
            while(arg_node) {
                print_expr(arg_node->expr);
                if (arg_node->next) printf(", ");
                arg_node = arg_node->next;
            }
            printf(")");
            break;
        }
        case IR_EXPR_ARRAY: {
            IRExprArray* arr = (IRExprArray*)expr;
            printf("[");
            IRExprNode* elem_node = arr->elements;
            while(elem_node) {
                print_expr(elem_node->expr);
                if (elem_node->next) printf(", ");
                elem_node = elem_node->next;
            }
            printf("]");
            break;
        }
        default:
            printf("UNKNOWN_EXPR");
            break;
    }
}

static void print_property_list(IRProperty* head, int indent_level) {
    IRProperty* current = head;
    while(current) {
        print_indent(indent_level);
        printf("[PROPERTY name=\"%s\"] value=", current->name);
        print_expr(current->value);
        printf("\n");
        current = current->next;
    }
}

static void print_object_list(IRObject* head, int indent_level) {
    IRObject* current = head;
    while(current) {
        print_indent(indent_level);
        printf("[OBJECT c_name=\"%s\" json_type=\"%s\"", current->c_name, current->json_type);
        if (current->registered_id) {
            printf(" id=\"%s\"", current->registered_id);
        }
        printf("]\n");

        if (current->properties) {
            print_property_list(current->properties, indent_level + 1);
        }
        if (current->with_blocks) {
            print_with_block_list(current->with_blocks, indent_level + 1);
        }
        if (current->children) {
            print_indent(indent_level + 1);
            printf("[CHILDREN]\n");
            print_object_list(current->children, indent_level + 2);
        }
        current = current->next;
    }
}

static void print_with_block_list(IRWithBlock* head, int indent_level) {
    IRWithBlock* current = head;
    while (current) {
        print_indent(indent_level);
        printf("[WITH target=");
        print_expr(current->target_expr);
        printf("]\n");

        if (current->properties) {
            print_property_list(current->properties, indent_level + 1);
        }
        if (current->children_root) {
             print_indent(indent_level + 1);
            printf("[DO]\n");
            print_object_list(current->children_root, indent_level + 2);
        }
        current = current->next;
    }
}


// --- Main Backend Function ---

void ir_print_backend(IRRoot* root, const ApiSpec* api_spec) {
    (void)api_spec; // Not used in this backend

    if (!root) {
        printf("[IR_PRINTER] IR Root is NULL.\n");
        return;
    }

    printf("[ROOT]\n");

    if (root->components) {
        // TODO: Implement printing for component definitions if needed
    }

    if (root->root_objects) {
        print_object_list(root->root_objects, 1);
    } else {
        print_indent(1);
        printf("(No root objects)\n");
    }
}

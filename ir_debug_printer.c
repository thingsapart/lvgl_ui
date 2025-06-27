#include "ir_debug_printer.h"
#include "ir.h"
#include <stdio.h>

// --- Forward Declarations ---
static void debug_print_indent(int level);
static void debug_print_expr(IRExpr* expr, int indent_level);
static void debug_print_property_list(IRProperty* head, int indent_level);
static void debug_print_object_list(IRObject* head, int indent_level);
static void debug_print_with_block_list(IRWithBlock* head, int indent_level);

// --- Helper Functions ---

static const char* get_ir_node_type_str(int type) {
    switch(type) {
        // High-level nodes
        case IR_NODE_ROOT: return "IR_NODE_ROOT";
        case IR_NODE_OBJECT: return "IR_NODE_OBJECT";
        case IR_NODE_COMPONENT_DEF: return "IR_NODE_COMPONENT_DEF";
        case IR_NODE_PROPERTY: return "IR_NODE_PROPERTY";
        case IR_NODE_WITH_BLOCK: return "IR_NODE_WITH_BLOCK";
        // Expression nodes
        case IR_EXPR_LITERAL: return "IR_EXPR_LITERAL";
        case IR_EXPR_ENUM: return "IR_EXPR_ENUM";
        case IR_EXPR_FUNCTION_CALL: return "IR_EXPR_FUNCTION_CALL";
        case IR_EXPR_ARRAY: return "IR_EXPR_ARRAY";
        case IR_EXPR_REGISTRY_REF: return "IR_EXPR_REGISTRY_REF";
        case IR_EXPR_CONTEXT_VAR: return "IR_EXPR_CONTEXT_VAR";
        case IR_EXPR_STATIC_STRING: return "IR_EXPR_STATIC_STRING";
        default: return "UNKNOWN_NODE_TYPE";
    }
}

static void debug_print_indent(int level) {
    for (int i = 0; i < level; ++i) {
        printf("  "); // 2 spaces per indent level
    }
}

static void debug_print_expr(IRExpr* expr, int indent_level) {
    if (!expr) {
        debug_print_indent(indent_level);
        printf("[NULL_EXPR]\n");
        return;
    }

    debug_print_indent(indent_level);
    printf("[%s] ", get_ir_node_type_str(expr->type));

    switch(expr->type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)expr;
            printf("value=%s is_string=%s\n", lit->value, lit->is_string ? "true" : "false");
            break;
        }
        case IR_EXPR_STATIC_STRING: {
            printf("value=\"%s\"\n", ((IRExprStaticString*)expr)->value);
            break;
        }
        case IR_EXPR_ENUM: {
            IRExprEnum* en = (IRExprEnum*)expr;
            printf("symbol=%s value(unresolved)=%ld\n", en->symbol, (long)en->value);
            break;
        }
        case IR_EXPR_REGISTRY_REF: {
            printf("name=%s\n", ((IRExprRegistryRef*)expr)->name);
            break;
        }
        case IR_EXPR_CONTEXT_VAR: {
            printf("name=%s\n", ((IRExprContextVar*)expr)->name);
            break;
        }
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;
            printf("func_name=\"%s\"\n", call->func_name);
            debug_print_indent(indent_level + 1);
            printf("[ARGS]\n");
            IRExprNode* arg_node = call->args;
            if (!arg_node) {
                 debug_print_indent(indent_level + 2);
                 printf("(No arguments)\n");
            }
            while(arg_node) {
                debug_print_expr(arg_node->expr, indent_level + 2);
                arg_node = arg_node->next;
            }
            break;
        }
        case IR_EXPR_ARRAY: {
            IRExprArray* arr = (IRExprArray*)expr;
            printf("\n");
            debug_print_indent(indent_level + 1);
            printf("[ELEMENTS]\n");
            IRExprNode* elem_node = arr->elements;
             if (!elem_node) {
                 debug_print_indent(indent_level + 2);
                 printf("(Empty array)\n");
            }
            while(elem_node) {
                debug_print_expr(elem_node->expr, indent_level + 2);
                elem_node = elem_node->next;
            }
            break;
        }
        default:
            printf("Unhandled node type.\n");
            break;
    }
}

static void debug_print_property_list(IRProperty* head, int indent_level) {
    IRProperty* current = head;
    while(current) {
        debug_print_indent(indent_level);
        printf("[%s] name=\"%s\"\n", get_ir_node_type_str(current->base.type), current->name);
        debug_print_indent(indent_level + 1);
        printf("[VALUE]\n");
        debug_print_expr(current->value, indent_level + 2);
        current = current->next;
    }
}

static void debug_print_with_block_list(IRWithBlock* head, int indent_level) {
    IRWithBlock* current = head;
    while (current) {
        debug_print_indent(indent_level);
        printf("[%s]\n", get_ir_node_type_str(current->base.type));
        debug_print_indent(indent_level + 1);
        printf("[TARGET_EXPR]\n");
        debug_print_expr(current->target_expr, indent_level + 2);

        if (current->properties) {
            debug_print_indent(indent_level + 1);
            printf("[PROPERTIES]\n");
            debug_print_property_list(current->properties, indent_level + 2);
        }
        if (current->children_root) {
            debug_print_indent(indent_level + 1);
            printf("[DO_CHILDREN]\n");
            debug_print_object_list(current->children_root, indent_level + 2);
        }
        current = current->next;
    }
}

static void debug_print_object_list(IRObject* head, int indent_level) {
    IRObject* current = head;
    while(current) {
        debug_print_indent(indent_level);
        printf("[%s] c_name=\"%s\" json_type=\"%s\"", get_ir_node_type_str(current->base.type), current->c_name, current->json_type);
        if (current->registered_id) {
            printf(" id=\"%s\"", current->registered_id);
        }
        printf("\n");

        if (current->properties) {
            debug_print_indent(indent_level + 1);
            printf("[PROPERTIES]\n");
            debug_print_property_list(current->properties, indent_level + 2);
        }
        if (current->with_blocks) {
            debug_print_indent(indent_level + 1);
            printf("[WITH_BLOCKS]\n");
            debug_print_with_block_list(current->with_blocks, indent_level + 2);
        }
        if (current->children) {
            debug_print_indent(indent_level + 1);
            printf("[CHILDREN]\n");
            debug_print_object_list(current->children, indent_level + 2);
        }
        current = current->next;
    }
}


// --- Main Backend Function ---

void ir_debug_print_backend(IRRoot* root, const ApiSpec* api_spec) {
    (void)api_spec; // Not used in this backend

    if (!root) {
        printf("[IR_DEBUG_PRINTER] IR Root is NULL.\n");
        return;
    }

    printf("[%s]\n", get_ir_node_type_str(root->base.type));

    if (root->components) {
        // TODO: Implement printing for component definitions if needed
    }

    if (root->root_objects) {
        debug_print_object_list(root->root_objects, 1);
    } else {
        debug_print_indent(1);
        printf("(No root objects)\n");
    }
}

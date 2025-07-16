#include "ir_debug_printer.h"
#include "ir.h"
#include <stdio.h>

// --- Forward Declarations ---
static void debug_print_indent(int level);
static void debug_print_expr(IRExpr* expr, int indent_level);
static void debug_print_expr_list(IRExprNode* head, int indent_level);
static void debug_print_object_list(IRObject* head, int indent_level);
static void debug_print_with_block_list(IRWithBlock* head, int indent_level);
static void debug_print_node(IRNode* node, int indent_level);


// --- Helper Functions ---

static const char* get_ir_node_type_str(int type) {
    switch(type) {
        case IR_NODE_ROOT: return "IR_NODE_ROOT";
        case IR_NODE_OBJECT: return "IR_NODE_OBJECT";
        case IR_NODE_COMPONENT_DEF: return "IR_NODE_COMPONENT_DEF";
        case IR_NODE_PROPERTY: return "IR_NODE_PROPERTY";
        case IR_NODE_WITH_BLOCK: return "IR_NODE_WITH_BLOCK";
        case IR_NODE_WARNING: return "IR_NODE_WARNING";
        case IR_NODE_OBSERVER: return "IR_NODE_OBSERVER";
        case IR_NODE_ACTION: return "IR_NODE_ACTION";
        case IR_EXPR_LITERAL: return "IR_EXPR_LITERAL";
        case IR_EXPR_ENUM: return "IR_EXPR_ENUM";
        case IR_EXPR_FUNCTION_CALL: return "IR_EXPR_FUNCTION_CALL";
        case IR_EXPR_ARRAY: return "IR_EXPR_ARRAY";
        case IR_EXPR_REGISTRY_REF: return "IR_EXPR_REGISTRY_REF";
        case IR_EXPR_CONTEXT_VAR: return "IR_EXPR_CONTEXT_VAR";
        case IR_EXPR_STATIC_STRING: return "IR_EXPR_STATIC_STRING";
        case IR_EXPR_RUNTIME_REG_ADD: return "IR_EXPR_RUNTIME_REG_ADD";
        default: return "UNKNOWN_NODE_TYPE";
    }
}

static void debug_print_indent(int level) {
    for (int i = 0; i < level; ++i) printf("  ");
}

static void debug_print_expr(IRExpr* expr, int indent_level) {
    debug_print_node((IRNode*)expr, indent_level);
}


static void debug_print_node(IRNode* node, int indent_level) {
    if (!node) {
        debug_print_indent(indent_level);
        printf("[NULL_NODE]\n");
        return;
    }

    debug_print_indent(indent_level);
    printf("[%s] ", get_ir_node_type_str(node->type));
    if (node->type >= IR_EXPR_LITERAL) {
         printf("type: <%s> ", ((IRExpr*)node)->c_type ? ((IRExpr*)node)->c_type : "null");
    }

    switch(node->type) {
        case IR_EXPR_LITERAL:
            printf("value=%s is_string=%s\n", ((IRExprLiteral*)node)->value, ((IRExprLiteral*)node)->is_string ? "true" : "false");
            break;
        case IR_EXPR_STATIC_STRING:
            printf("value=\"%s\"\n", ((IRExprStaticString*)node)->value);
            break;
        case IR_EXPR_ENUM:
            printf("symbol=%s\n", ((IRExprEnum*)node)->symbol);
            break;
        case IR_EXPR_REGISTRY_REF:
            printf("name=%s\n", ((IRExprRegistryRef*)node)->name);
            break;
        case IR_EXPR_CONTEXT_VAR:
            printf("name=$%s\n", ((IRExprContextVar*)node)->name);
            break;
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)node;
            printf("func_name=\"%s\"\n", call->func_name);
            debug_print_indent(indent_level + 1);
            printf("[ARGS]\n");
            debug_print_expr_list(call->args, indent_level + 2);
            break;
        }
        case IR_EXPR_ARRAY: {
            IRExprArray* arr = (IRExprArray*)node;
            printf("ptr=%p\n", (void*)arr);
            debug_print_indent(indent_level + 1);
            printf("[ELEMENTS]\n");
            debug_print_expr_list(arr->elements, indent_level + 2);
            break;
        }
        case IR_EXPR_RUNTIME_REG_ADD: {
            IRExprRuntimeRegAdd* reg = (IRExprRuntimeRegAdd*)node;
            printf("id=\"%s\"\n", reg->id);
            debug_print_indent(indent_level + 1);
            printf("[OBJECT_EXPR]\n");
            debug_print_expr(reg->object_expr, indent_level + 2);
            break;
        }
        case IR_NODE_OBJECT: {
            debug_print_object_list((IRObject*)node, indent_level);
            return; // Avoid double printing
        }
        case IR_NODE_WARNING: {
             printf("message=\"%s\"\n", ((IRWarning*)node)->message);
             break;
        }
        case IR_NODE_OBSERVER: {
            IRObserver* obs = (IRObserver*)node;
            printf("state=\"%s\" type=%d\n", obs->state_name, obs->update_type);
            debug_print_indent(indent_level + 1);
            printf("[CONFIG_EXPR]\n");
            debug_print_expr(obs->config_expr, indent_level + 2);
            break;
        }
        case IR_NODE_ACTION: {
            IRAction* act = (IRAction*)node;
            printf("name=\"%s\" type=%d\n", act->action_name, act->action_type);
            if (act->data_expr) {
                debug_print_indent(indent_level + 1);
                printf("[DATA_EXPR]\n");
                debug_print_expr(act->data_expr, indent_level + 2);
            }
            break;
        }
        default:
            printf("Unhandled node type.\n");
            break;
    }
}


static void debug_print_expr_list(IRExprNode* head, int indent_level) {
    if (!head) {
        debug_print_indent(indent_level);
        printf("(empty)\n");
        return;
    }
    for (IRExprNode* current = head; current; current = current->next) {
        debug_print_expr(current->expr, indent_level);
    }
}

static void debug_print_object_list(IRObject* head, int indent_level); // Forward declare

static void debug_print_with_block_list(IRWithBlock* head, int indent_level) {
    for (IRWithBlock* current = head; current; current = current->next) {
        debug_print_indent(indent_level);
        printf("[%s]\n", get_ir_node_type_str(current->base.type));
        debug_print_indent(indent_level + 1);
        printf("[TARGET_EXPR]\n");
        debug_print_expr(current->target_expr, indent_level + 2);

        if (current->setup_calls) {
            debug_print_indent(indent_level + 1);
            printf("[SETUP_CALLS]\n");
            debug_print_expr_list(current->setup_calls, indent_level + 2);
        }
        if (current->children_root) {
            debug_print_indent(indent_level + 1);
            printf("[DO_CHILDREN]\n");
            debug_print_object_list(current->children_root, indent_level + 2);
        }
    }
}

static void debug_print_object_list(IRObject* head, int indent_level) {
    for (IRObject* current = head; current; current = current->next) {
        debug_print_indent(indent_level);
        printf("[%s] c_name=\"%s\" json_type=\"%s\" c_type=\"%s\"",
               get_ir_node_type_str(current->base.type), current->c_name, current->json_type, current->c_type);
        if (current->registered_id) printf(" id=\"%s\"", current->registered_id);
        printf("\n");

        debug_print_indent(indent_level + 1);
        printf("[CONSTRUCTOR_EXPR]\n");
        debug_print_expr(current->constructor_expr, indent_level + 2);

        if (current->operations) {
            debug_print_indent(indent_level + 1);
            printf("[OPERATIONS]\n");
            IROperationNode* op_node = current->operations;
            while(op_node) {
                debug_print_node(op_node->op_node, indent_level + 2);
                op_node = op_node->next;
            }
        }

        if (current->with_blocks) {
            debug_print_indent(indent_level + 1);
            printf("[WITH_BLOCKS]\n");
            debug_print_with_block_list(current->with_blocks, indent_level + 2);
        }
    }
}


void ir_debug_print_backend(IRRoot* root, const ApiSpec* api_spec) {
    (void)api_spec;
    if (!root) {
        printf("[IR_DEBUG_PRINTER] IR Root is NULL.\n");
        return;
    }
    printf("[%s]\n", get_ir_node_type_str(root->base.type));
    if (root->root_objects) {
        debug_print_object_list(root->root_objects, 1);
    } else {
        debug_print_indent(1);
        printf("(No root objects)\n");
    }
}

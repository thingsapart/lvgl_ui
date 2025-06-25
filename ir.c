#include "ir.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// --- Helper for safe string duplication ---
static char* safe_strdup(const char* s) {
    return s ? strdup(s) : NULL;
}

// --- Forward declarations for free helpers ---
static void free_expr_list(IRExprNode* head);
static void free_property_list(IRProperty* head);
static void free_with_block_list(IRWithBlock* head);
static void free_object_list(IRObject* head);
static void free_component_def_list(IRComponent* head);

// --- Factory functions for Expressions ---

IRExpr* ir_new_expr_literal(const char* value) {
    IRExprLiteral* lit = calloc(1, sizeof(IRExprLiteral));
    lit->base.type = IR_EXPR_LITERAL;
    lit->value = safe_strdup(value);
    lit->is_string = false;
    return (IRExpr*)lit;
}

IRExpr* ir_new_expr_literal_string(const char* value) {
    IRExprLiteral* lit = calloc(1, sizeof(IRExprLiteral));
    lit->base.type = IR_EXPR_LITERAL;
    lit->value = safe_strdup(value);
    lit->is_string = true;
    return (IRExpr*)lit;
}

IRExpr* ir_new_expr_static_string(const char* value) {
    IRExprStaticString* sstr = calloc(1, sizeof(IRExprStaticString));
    sstr->base.type = IR_EXPR_STATIC_STRING;
    sstr->value = safe_strdup(value);
    return (IRExpr*)sstr;
}

IRExpr* ir_new_expr_enum(const char* symbol, intptr_t val) {
    IRExprEnum* en = calloc(1, sizeof(IRExprEnum));
    en->base.type = IR_EXPR_ENUM;
    en->symbol = safe_strdup(symbol);
    en->value = val;
    return (IRExpr*)en;
}

IRExpr* ir_new_expr_func_call(const char* func_name, IRExprNode* args) {
    IRExprFunctionCall* call = calloc(1, sizeof(IRExprFunctionCall));
    call->base.type = IR_EXPR_FUNCTION_CALL;
    call->func_name = safe_strdup(func_name);
    call->args = args;
    return (IRExpr*)call;
}

IRExpr* ir_new_expr_array(IRExprNode* elements) {
    IRExprArray* arr = calloc(1, sizeof(IRExprArray));
    arr->base.type = IR_EXPR_ARRAY;
    arr->elements = elements;
    return (IRExpr*)arr;
}

IRExpr* ir_new_expr_registry_ref(const char* name) {
    IRExprRegistryRef* ref = calloc(1, sizeof(IRExprRegistryRef));
    ref->base.type = IR_EXPR_REGISTRY_REF;
    ref->name = safe_strdup(name);
    return (IRExpr*)ref;
}

IRExpr* ir_new_expr_context_var(const char* name) {
    IRExprContextVar* var = calloc(1, sizeof(IRExprContextVar));
    var->base.type = IR_EXPR_CONTEXT_VAR;
    var->name = safe_strdup(name);
    return (IRExpr*)var;
}

// --- Factory functions for High-Level Constructs ---

IRRoot* ir_new_root() {
    IRRoot* root = calloc(1, sizeof(IRRoot));
    root->base.type = IR_NODE_ROOT;
    return root;
}

IRObject* ir_new_object(const char* c_name, const char* json_type, const char* registered_id) {
    IRObject* obj = calloc(1, sizeof(IRObject));
    obj->base.type = IR_NODE_OBJECT;
    obj->c_name = safe_strdup(c_name);
    obj->json_type = safe_strdup(json_type);
    obj->registered_id = safe_strdup(registered_id);
    return obj;
}

IRComponent* ir_new_component_def(const char* id, IRObject* root_widget) {
    IRComponent* comp = calloc(1, sizeof(IRComponent));
    comp->base.type = IR_NODE_COMPONENT_DEF;
    comp->id = safe_strdup(id);
    comp->root_widget = root_widget;
    return comp;
}

IRProperty* ir_new_property(const char* name, IRExpr* value) {
    IRProperty* prop = calloc(1, sizeof(IRProperty));
    prop->base.type = IR_NODE_PROPERTY;
    prop->name = safe_strdup(name);
    prop->value = value;
    return prop;
}

IRWithBlock* ir_new_with_block(IRExpr* target, IRProperty* props, IRObject* children) {
    IRWithBlock* wb = calloc(1, sizeof(IRWithBlock));
    wb->base.type = IR_NODE_WITH_BLOCK;
    wb->target_expr = target;
    wb->properties = props;
    wb->children_root = children;
    return wb;
}


// --- List Management ---
#define IMPLEMENT_LIST_ADD(func_name, node_type, list_head_type) \
void func_name(list_head_type** head, node_type* item) { \
    if (!head || !item) return; \
    if (!*head) { \
        *head = item; \
    } else { \
        list_head_type* current = *head; \
        while (current->next) { current = current->next; } \
        current->next = item; \
    } \
}

IMPLEMENT_LIST_ADD(ir_object_list_add, IRObject, IRObject)
IMPLEMENT_LIST_ADD(ir_property_list_add, IRProperty, IRProperty)
IMPLEMENT_LIST_ADD(ir_with_block_list_add, IRWithBlock, IRWithBlock)
IMPLEMENT_LIST_ADD(ir_component_def_list_add, IRComponent, IRComponent)

void ir_expr_list_add(IRExprNode** head, IRExpr* expr) {
    if (!head || !expr) return;
    IRExprNode* new_node = calloc(1, sizeof(IRExprNode));
    new_node->expr = expr;
    if (!*head) {
        *head = new_node;
    } else {
        IRExprNode* current = *head;
        while (current->next) { current = current->next; }
        current->next = new_node;
    }
}

// --- Free Functions ---

static void free_expr_list(IRExprNode* head) {
    IRExprNode* current = head;
    while (current) {
        IRExprNode* next = current->next;
        ir_free((IRNode*)current->expr);
        free(current);
        current = next;
    }
}

static void free_property_list(IRProperty* head) {
    IRProperty* current = head;
    while (current) {
        IRProperty* next = current->next;
        ir_free((IRNode*)current);
        current = next;
    }
}

static void free_with_block_list(IRWithBlock* head) {
    IRWithBlock* current = head;
    while (current) {
        IRWithBlock* next = current->next;
        ir_free((IRNode*)current);
        current = next;
    }
}

static void free_object_list(IRObject* head) {
    IRObject* current = head;
    while (current) {
        IRObject* next = current->next;
        ir_free((IRNode*)current);
        current = next;
    }
}

static void free_component_def_list(IRComponent* head) {
    IRComponent* current = head;
    while (current) {
        IRComponent* next = current->next;
        ir_free((IRNode*)current);
        current = next;
    }
}

void ir_free(IRNode* node) {
    if (!node) return;
    switch (node->type) {
        // High-level nodes
        case IR_NODE_ROOT: {
            IRRoot* root = (IRRoot*)node;
            free_component_def_list(root->components);
            free_object_list(root->root_objects);
            break;
        }
        case IR_NODE_OBJECT: {
            IRObject* obj = (IRObject*)node;
            free(obj->c_name);
            free(obj->json_type);
            free(obj->registered_id);
            free(obj->use_view_component_id);
            free_property_list(obj->use_view_context);
            free_property_list(obj->properties);
            free_with_block_list(obj->with_blocks);
            free_object_list(obj->children);
            break;
        }
        case IR_NODE_COMPONENT_DEF: {
            IRComponent* comp = (IRComponent*)node;
            free(comp->id);
            ir_free((IRNode*)comp->root_widget);
            break;
        }
        case IR_NODE_PROPERTY: {
            IRProperty* prop = (IRProperty*)node;
            free(prop->name);
            ir_free((IRNode*)prop->value);
            break;
        }
        case IR_NODE_WITH_BLOCK: {
            IRWithBlock* wb = (IRWithBlock*)node;
            ir_free((IRNode*)wb->target_expr);
            free_property_list(wb->properties);
            ir_free((IRNode*)wb->children_root);
            break;
        }

        // Expression nodes
        case IR_EXPR_LITERAL: free(((IRExprLiteral*)node)->value); break;
        case IR_EXPR_STATIC_STRING: free(((IRExprStaticString*)node)->value); break;
        case IR_EXPR_ENUM: free(((IRExprEnum*)node)->symbol); break;
        case IR_EXPR_REGISTRY_REF: free(((IRExprRegistryRef*)node)->name); break;
        case IR_EXPR_CONTEXT_VAR: free(((IRExprContextVar*)node)->name); break;
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)node;
            free(call->func_name);
            free_expr_list(call->args);
            break;
        }
        case IR_EXPR_ARRAY: free_expr_list(((IRExprArray*)node)->elements); break;
        default: break;
    }
    free(node);
}

// --- Node Value Accessors ---

const char* ir_node_get_string(IRNode* node) {
    if (!node) return NULL;
    switch (node->type) {
        case IR_EXPR_LITERAL:
            return ((IRExprLiteral*)node)->is_string ? ((IRExprLiteral*)node)->value : NULL;
        case IR_EXPR_STATIC_STRING:
            return ((IRExprStaticString*)node)->value;
        case IR_EXPR_REGISTRY_REF:
            return ((IRExprRegistryRef*)node)->name;
        default:
            fprintf(stderr, "Warning: ir_node_get_string called on incompatible node type %d\n", node->type);
            return NULL;
    }
}

intptr_t ir_node_get_int(IRNode* node) {
    if (!node) return 0;
    switch (node->type) {
        case IR_EXPR_LITERAL:
            if (!((IRExprLiteral*)node)->is_string && ((IRExprLiteral*)node)->value) {
                return strtol(((IRExprLiteral*)node)->value, NULL, 0);
            }
            return 0;
        case IR_EXPR_ENUM:
            return ((IRExprEnum*)node)->value;
        default:
            fprintf(stderr, "Warning: ir_node_get_int called on incompatible node type %d\n", node->type);
            return 0;
    }
}

bool ir_node_get_bool(IRNode* node) {
    if (!node) return false;
    if (node->type == IR_EXPR_LITERAL) {
        IRExprLiteral* lit = (IRExprLiteral*)node;
        if (!lit->is_string && lit->value) {
            return strcmp(lit->value, "true") == 0 || (strtol(lit->value, NULL, 0) != 0);
        }
    }
    fprintf(stderr, "Warning: ir_node_get_bool called on incompatible node type %d\n", node->type);
    return false;
}

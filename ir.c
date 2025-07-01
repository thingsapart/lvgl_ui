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
static void free_expr(IRExpr* expr);
static void free_operation_list(IROperationNode* head);

// --- Factory functions for Expressions ---

IRExpr* ir_new_expr_literal(const char* value, const char* c_type) {
    IRExprLiteral* lit = calloc(1, sizeof(IRExprLiteral));
    lit->base.base.type = IR_EXPR_LITERAL;
    lit->base.c_type = safe_strdup(c_type);
    lit->value = safe_strdup(value);
    lit->is_string = false;
    lit->len = lit->value ? strlen(lit->value) : 0;
    return (IRExpr*)lit;
}

IRExpr* ir_new_expr_literal_string(const char* value, size_t len) {
    IRExprLiteral* lit = calloc(1, sizeof(IRExprLiteral));
    lit->base.base.type = IR_EXPR_LITERAL;
    lit->base.c_type = safe_strdup("const char*");
    lit->value = malloc(len + 1); // +1 for safety null terminator
    if (lit->value) {
        memcpy(lit->value, value, len);
        lit->value[len] = '\0';
    }
    lit->len = len;
    lit->is_string = true;
    return (IRExpr*)lit;
}

IRExpr* ir_new_expr_static_string(const char* value, size_t len) {
    IRExprStaticString* sstr = calloc(1, sizeof(IRExprStaticString));
    sstr->base.base.type = IR_EXPR_STATIC_STRING;
    sstr->base.c_type = safe_strdup("const char*");
    sstr->value = malloc(len + 1);
    if (sstr->value) {
        memcpy(sstr->value, value, len);
        sstr->value[len] = '\0';
    }
    sstr->len = len;
    return (IRExpr*)sstr;
}

IRExpr* ir_new_expr_enum(const char* symbol, intptr_t val, const char* enum_c_type) {
    IRExprEnum* en = calloc(1, sizeof(IRExprEnum));
    en->base.base.type = IR_EXPR_ENUM;
    en->base.c_type = safe_strdup(enum_c_type);
    en->symbol = safe_strdup(symbol);
    en->value = val;
    return (IRExpr*)en;
}

IRExpr* ir_new_expr_func_call(const char* func_name, IRExprNode* args, const char* return_c_type) {
    IRExprFunctionCall* call = calloc(1, sizeof(IRExprFunctionCall));
    call->base.base.type = IR_EXPR_FUNCTION_CALL;
    call->base.c_type = safe_strdup(return_c_type);
    call->func_name = safe_strdup(func_name);
    call->args = args;
    return (IRExpr*)call;
}

IRExpr* ir_new_expr_array(IRExprNode* elements, const char* array_c_type) {
    IRExprArray* arr = calloc(1, sizeof(IRExprArray));
    arr->base.base.type = IR_EXPR_ARRAY;
    arr->base.c_type = safe_strdup(array_c_type);
    arr->elements = elements;
    arr->static_array_ptr = NULL; // Initialize cached pointer
    return (IRExpr*)arr;
}

IRExpr* ir_new_expr_registry_ref(const char* name, const char* c_type) {
    IRExprRegistryRef* ref = calloc(1, sizeof(IRExprRegistryRef));
    ref->base.base.type = IR_EXPR_REGISTRY_REF;
    ref->base.c_type = safe_strdup(c_type);
    ref->name = safe_strdup(name);
    return (IRExpr*)ref;
}

IRExpr* ir_new_expr_context_var(const char* name, const char* c_type) {
    IRExprContextVar* var = calloc(1, sizeof(IRExprContextVar));
    var->base.base.type = IR_EXPR_CONTEXT_VAR;
    var->base.c_type = safe_strdup(c_type);
    var->name = safe_strdup(name);
    return (IRExpr*)var;
}

IRExpr* ir_new_expr_runtime_reg_add(const char* id, IRExpr* object_expr) {
    IRExprRuntimeRegAdd* reg = calloc(1, sizeof(IRExprRuntimeRegAdd));
    reg->base.base.type = IR_EXPR_RUNTIME_REG_ADD;
    reg->base.c_type = safe_strdup("void"); // Registration function assumed to return void
    reg->id = safe_strdup(id);
    reg->object_expr = object_expr;
    return (IRExpr*)reg;
}

IRExpr* ir_new_expr_raw_pointer(void* ptr, const char* c_type) {
    IRExprRawPointer* raw_ptr = calloc(1, sizeof(IRExprRawPointer));
    raw_ptr->base.base.type = IR_EXPR_RAW_POINTER;
    raw_ptr->base.c_type = safe_strdup(c_type);
    raw_ptr->ptr = ptr;
    return (IRExpr*)raw_ptr;
}

// --- Factory functions for High-Level Constructs ---

IRRoot* ir_new_root() {
    IRRoot* root = calloc(1, sizeof(IRRoot));
    root->base.type = IR_NODE_ROOT;
    return root;
}

IRObject* ir_new_object(const char* c_name, const char* json_type, const char* c_type, const char* registered_id) {
    IRObject* obj = calloc(1, sizeof(IRObject));
    obj->base.type = IR_NODE_OBJECT;
    obj->c_name = safe_strdup(c_name);
    obj->json_type = safe_strdup(json_type);
    obj->c_type = safe_strdup(c_type);
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

IRWithBlock* ir_new_with_block(IRExpr* target, IRExprNode* calls, IRObject* children) {
    IRWithBlock* wb = calloc(1, sizeof(IRWithBlock));
    wb->base.type = IR_NODE_WITH_BLOCK;
    wb->target_expr = target;
    wb->setup_calls = calls;
    wb->children_root = children;
    return wb;
}

IRWarning* ir_new_warning(const char* message) {
    IRWarning* warn = calloc(1, sizeof(IRWarning));
    warn->base.type = IR_NODE_WARNING;
    warn->message = safe_strdup(message);
    return warn;
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

void ir_operation_list_add(IROperationNode** head, IRNode* node) {
    if (!head || !node) return;
    IROperationNode* new_node = calloc(1, sizeof(IROperationNode));
    new_node->op_node = node;
    new_node->next = NULL;

    if (!*head) {
        *head = new_node;
    } else {
        IROperationNode* current = *head;
        while (current->next) {
            current = current->next;
        }
        current->next = new_node;
    }
}


// --- Free Functions ---

static void free_expr(IRExpr* expr) {
    if (!expr) return;
    free(expr->c_type); // Free the C type string

    switch (expr->base.type) {
        case IR_EXPR_LITERAL: free(((IRExprLiteral*)expr)->value); break;
        case IR_EXPR_STATIC_STRING: free(((IRExprStaticString*)expr)->value); break;
        case IR_EXPR_ENUM: free(((IRExprEnum*)expr)->symbol); break;
        case IR_EXPR_REGISTRY_REF: free(((IRExprRegistryRef*)expr)->name); break;
        case IR_EXPR_CONTEXT_VAR: free(((IRExprContextVar*)expr)->name); break;
        case IR_EXPR_RAW_POINTER: /* ptr is not owned, do nothing */ break;
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;
            free(call->func_name);
            free_expr_list(call->args);
            break;
        }
        case IR_EXPR_ARRAY: {
            IRExprArray* arr = (IRExprArray*)expr;
            // Note: static_array_ptr is owned by the registry, not the IR.
            // So we DO NOT free it here.
            free_expr_list(arr->elements);
            break;
        }
        case IR_EXPR_RUNTIME_REG_ADD: {
            IRExprRuntimeRegAdd* reg = (IRExprRuntimeRegAdd*)expr;
            free(reg->id);
            free_expr(reg->object_expr);
            break;
        }
        default: break;
    }
    free(expr);
}


static void free_expr_list(IRExprNode* head) {
    IRExprNode* current = head;
    while (current) {
        IRExprNode* next = current->next;
        free_expr(current->expr);
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

static void free_operation_list(IROperationNode* head) {
    IROperationNode* current = head;
    while (current) {
        IROperationNode* next = current->next;
        ir_free(current->op_node);
        free(current);
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
            free(obj->c_type);
            free(obj->registered_id);
            free(obj->use_view_component_id);
            free_expr(obj->constructor_expr);
            free_operation_list(obj->operations);
            free_property_list(obj->use_view_context);
            free_with_block_list(obj->with_blocks);
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
            free_expr(prop->value);
            break;
        }
        case IR_NODE_WITH_BLOCK: {
            IRWithBlock* wb = (IRWithBlock*)node;
            free_expr(wb->target_expr);
            free_expr_list(wb->setup_calls);
            ir_free((IRNode*)wb->children_root);
            break;
        }
        case IR_NODE_WARNING: { // NEW
            IRWarning* warn = (IRWarning*)node;
            free(warn->message);
            break;
        }
        case IR_EXPR_LITERAL:
        case IR_EXPR_STATIC_STRING:
        case IR_EXPR_ENUM:
        case IR_EXPR_REGISTRY_REF:
        case IR_EXPR_CONTEXT_VAR:
        case IR_EXPR_FUNCTION_CALL:
        case IR_EXPR_ARRAY:
        case IR_EXPR_RUNTIME_REG_ADD:
        case IR_EXPR_RAW_POINTER:
            free_expr((IRExpr*)node);
            return; // free_expr already frees the node itself.
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

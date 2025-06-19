#include "ir.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h> // For NULL, perror

// Forward declarations for actual codegen functions from codegen.c
void codegen_expr_literal(IRNode* node, int indent);
void codegen_expr_variable(IRNode* node, int indent);
void codegen_expr_func_call(IRNode* node, int indent);
void codegen_expr_array(IRNode* node, int indent);
void codegen_expr_address_of(IRNode* node, int indent);
void codegen_stmt_block(IRNode* node, int indent);
void codegen_stmt_var_decl(IRNode* node, int indent);
void codegen_stmt_func_call_stmt(IRNode* node, int indent);
void codegen_stmt_comment(IRNode* node, int indent);
void codegen_stmt_widget_allocate(IRNode* node, int indent);
void codegen_stmt_object_allocate(IRNode* node, int indent);


// --- Forward declarations for free helpers ---
static void free_expr(IRExpr* expr);
static void free_stmt(IRStmt* stmt);
static void free_expr_list(IRExprNode* head);
static void free_stmt_list(IRStmtNode* head);

// --- Helper to initialize IRNode base ---
static void init_ir_node(IRNode* node, void (*free_func)(IRNode*), void (*codegen_func)(IRNode*, int)) {
    node->free = free_func;
    node->codegen = codegen_func;
}

// --- Factory functions for Expressions ---

IRExpr* ir_new_literal(const char* value) {
    IRExprLiteral* lit = (IRExprLiteral*)calloc(1, sizeof(IRExprLiteral));
    if (!lit) { perror("Failed to allocate IRExprLiteral"); return NULL; }
    init_ir_node((IRNode*)lit, (void (*)(IRNode*))free_expr, codegen_expr_literal);
    lit->base.type = IR_EXPR_LITERAL;
    lit->value = value ? strdup(value) : NULL;
    if (value && !lit->value) { perror("Failed to strdup literal value"); free(lit); return NULL; }
    return (IRExpr*)lit;
}

IRExpr* ir_new_literal_string(const char* value) {
    IRExprLiteral* lit = (IRExprLiteral*)calloc(1, sizeof(IRExprLiteral));
    if (!lit) { perror("Failed to allocate IRExprLiteral for string"); return NULL; }
    init_ir_node((IRNode*)lit, (void (*)(IRNode*))free_expr, codegen_expr_literal);
    lit->base.type = IR_EXPR_LITERAL;
    if (value) {
        char* quoted_value = (char*)malloc(strlen(value) + 3);
        if (!quoted_value) { perror("Failed to allocate for quoted string"); free(lit); return NULL; }
        sprintf(quoted_value, "\"%s\"", value);
        lit->value = quoted_value;
    } else {
        lit->value = strdup("\"\"");
        if (!lit->value) { perror("Failed to strdup empty quoted string"); free(lit); return NULL;}
    }
    return (IRExpr*)lit;
}

IRExpr* ir_new_variable(const char* name) {
    IRExprVariable* var = (IRExprVariable*)calloc(1, sizeof(IRExprVariable));
    if (!var) { perror("Failed to allocate IRExprVariable"); return NULL; }
    init_ir_node((IRNode*)var, (void (*)(IRNode*))free_expr, codegen_expr_variable);
    var->base.type = IR_EXPR_VARIABLE;
    var->name = name ? strdup(name) : NULL;
    if (name && !var->name) { perror("Failed to strdup variable name"); free(var); return NULL; }
    return (IRExpr*)var;
}

IRExpr* ir_new_func_call_expr(const char* func_name, IRExprNode* args) {
    IRExprFuncCall* call = (IRExprFuncCall*)calloc(1, sizeof(IRExprFuncCall));
    if (!call) { perror("Failed to allocate IRExprFuncCall"); return NULL; }
    init_ir_node((IRNode*)call, (void (*)(IRNode*))free_expr, codegen_expr_func_call);
    call->base.type = IR_EXPR_FUNC_CALL;
    call->func_name = func_name ? strdup(func_name) : NULL;
    if (func_name && !call->func_name) { perror("Failed to strdup func_name for call"); free(call); return NULL; }
    call->args = args;
    return (IRExpr*)call;
}

IRExpr* ir_new_array(IRExprNode* elements) {
    IRExprArray* arr = (IRExprArray*)calloc(1, sizeof(IRExprArray));
    if (!arr) { perror("Failed to allocate IRExprArray"); return NULL; }
    init_ir_node((IRNode*)arr, (void (*)(IRNode*))free_expr, codegen_expr_array);
    arr->base.type = IR_EXPR_ARRAY;
    arr->elements = elements;
    return (IRExpr*)arr;
}

IRExpr* ir_new_address_of(IRExpr* expr) {
    if (!expr) return NULL;
    IRExprAddressOf* addr = (IRExprAddressOf*)calloc(1, sizeof(IRExprAddressOf));
    if (!addr) { perror("Failed to allocate IRExprAddressOf"); return NULL; }
    init_ir_node((IRNode*)addr, (void (*)(IRNode*))free_expr, codegen_expr_address_of);
    addr->base.type = IR_EXPR_ADDRESS_OF;
    addr->expr = expr;
    return (IRExpr*)addr;
}

// --- Factory functions for Statements ---

IRStmtBlock* ir_new_block() {
    IRStmtBlock* block = (IRStmtBlock*)calloc(1, sizeof(IRStmtBlock));
    if (!block) { perror("Failed to allocate IRStmtBlock"); return NULL; }
    init_ir_node((IRNode*)block, (void (*)(IRNode*))free_stmt, codegen_stmt_block);
    block->base.type = IR_STMT_BLOCK;
    block->stmts = NULL;
    return block;
}

void ir_block_add_stmt(IRStmtBlock* block, IRStmt* stmt) {
    if (!block || !stmt) return;
    IRStmtNode* new_node = (IRStmtNode*)calloc(1, sizeof(IRStmtNode));
    if (!new_node) { perror("Failed to allocate IRStmtNode for block"); return; }
    new_node->stmt = stmt;
    new_node->next = NULL;
    if (!block->stmts) {
        block->stmts = new_node;
    } else {
        IRStmtNode* current = block->stmts;
        while (current->next) { current = current->next; }
        current->next = new_node;
    }
}

IRStmt* ir_new_var_decl(const char* type_name, const char* var_name, IRExpr* initializer) {
    IRStmtVarDecl* decl = (IRStmtVarDecl*)calloc(1, sizeof(IRStmtVarDecl));
    if (!decl) { perror("Failed to allocate IRStmtVarDecl"); return NULL; }
    init_ir_node((IRNode*)decl, (void (*)(IRNode*))free_stmt, codegen_stmt_var_decl);
    decl->base.type = IR_STMT_VAR_DECL;
    decl->type_name = type_name ? strdup(type_name) : NULL;
    if (type_name && !decl->type_name) { perror("Failed to strdup type_name for var_decl"); free(decl); return NULL; }
    decl->var_name = var_name ? strdup(var_name) : NULL;
    if (var_name && !decl->var_name) { perror("Failed to strdup var_name for var_decl"); free(decl->type_name); free(decl); return NULL; }
    decl->initializer = initializer;
    return (IRStmt*)decl;
}

IRStmt* ir_new_func_call_stmt(const char* func_name, IRExprNode* args) {
    IRStmtFuncCall* stmt_call = (IRStmtFuncCall*)calloc(1, sizeof(IRStmtFuncCall));
    if (!stmt_call) { perror("Failed to allocate IRStmtFuncCall"); return NULL; }
    init_ir_node((IRNode*)stmt_call, (void (*)(IRNode*))free_stmt, codegen_stmt_func_call_stmt);
    stmt_call->base.type = IR_STMT_FUNC_CALL;
    stmt_call->call = (IRExprFuncCall*)ir_new_func_call_expr(func_name, args);
    if (!stmt_call->call) { perror("Failed to create IRExprFuncCall for IRStmtFuncCall"); free(stmt_call); return NULL; }
    return (IRStmt*)stmt_call;
}

IRStmt* ir_new_comment(const char* text) {
    IRStmtComment* comment = (IRStmtComment*)calloc(1, sizeof(IRStmtComment));
    if (!comment) { perror("Failed to allocate IRStmtComment"); return NULL; }
    init_ir_node((IRNode*)comment, (void (*)(IRNode*))free_stmt, codegen_stmt_comment);
    comment->base.type = IR_STMT_COMMENT;
    comment->text = text ? strdup(text) : NULL;
    if (text && !comment->text) { perror("Failed to strdup comment text"); free(comment); return NULL; }
    return (IRStmt*)comment;
}

// For IRStmtWidgetAllocate
IRStmt* ir_new_widget_allocate_stmt(const char* c_var_name, const char* widget_c_type_name, const char* create_func_name, IRExpr* parent_expr) {
    if (!c_var_name || !widget_c_type_name || !create_func_name) {
        return NULL;
    }

    IRStmtWidgetAllocate* stmt = (IRStmtWidgetAllocate*)calloc(1, sizeof(IRStmtWidgetAllocate));
    if (!stmt) {
        perror("Failed to allocate IRStmtWidgetAllocate");
        return NULL;
    }
    init_ir_node((IRNode*)stmt, (void (*)(IRNode*))free_stmt, codegen_stmt_widget_allocate);
    stmt->base.type = IR_STMT_WIDGET_ALLOCATE;

    stmt->c_var_name = strdup(c_var_name);
    stmt->widget_c_type_name = strdup(widget_c_type_name);
    stmt->create_func_name = strdup(create_func_name);
    stmt->parent_expr = parent_expr;

    if (!stmt->c_var_name || !stmt->widget_c_type_name || !stmt->create_func_name) {
        perror("Failed to strdup members for IRStmtWidgetAllocate");
        free(stmt->c_var_name);
        free(stmt->widget_c_type_name);
        free(stmt->create_func_name);
        free(stmt);
        return NULL;
    }
    return (IRStmt*)stmt;
}

// For IRStmtObjectAllocate
IRStmt* ir_new_object_allocate_stmt(const char* c_var_name, const char* object_c_type_name, const char* init_func_name) {
    if (!c_var_name || !object_c_type_name || !init_func_name) {
        return NULL;
    }

    IRStmtObjectAllocate* stmt = (IRStmtObjectAllocate*)calloc(1, sizeof(IRStmtObjectAllocate));
    if (!stmt) {
        perror("Failed to allocate IRStmtObjectAllocate");
        return NULL;
    }
    init_ir_node((IRNode*)stmt, (void (*)(IRNode*))free_stmt, codegen_stmt_object_allocate);
    stmt->base.type = IR_STMT_OBJECT_ALLOCATE;

    stmt->c_var_name = strdup(c_var_name);
    stmt->object_c_type_name = strdup(object_c_type_name);
    stmt->init_func_name = strdup(init_func_name);

    if (!stmt->c_var_name || !stmt->object_c_type_name || !stmt->init_func_name) {
        perror("Failed to strdup members for IRStmtObjectAllocate");
        free(stmt->c_var_name);
        free(stmt->object_c_type_name);
        free(stmt->init_func_name);
        free(stmt);
        return NULL;
    }
    return (IRStmt*)stmt;
}


// --- Factory functions for Linked List Nodes ---

IRExprNode* ir_new_expr_node(IRExpr* expr) {
    if (!expr) return NULL;
    IRExprNode* node = (IRExprNode*)calloc(1, sizeof(IRExprNode));
    if (!node) { perror("Failed to allocate IRExprNode"); return NULL; }
    node->expr = expr;
    node->next = NULL;
    return node;
}

void ir_expr_list_add(IRExprNode** head, IRExpr* expr) {
    if (!head || !expr) return;
    IRExprNode* new_node = ir_new_expr_node(expr);
    if (!new_node) return;
    if (!*head) {
        *head = new_node;
    } else {
        IRExprNode* current = *head;
        while (current->next) { current = current->next; }
        current->next = new_node;
    }
}

// --- Free functions ---

static void free_expr_list(IRExprNode* head) {
    IRExprNode* current = head;
    while (current) {
        IRExprNode* next = current->next;
        if (current->expr) ir_free((IRNode*)current->expr);
        free(current);
        current = next;
    }
}

static void free_stmt_list(IRStmtNode* head) {
    IRStmtNode* current = head;
    while (current) {
        IRStmtNode* next = current->next;
        if (current->stmt) ir_free((IRNode*)current->stmt);
        free(current);
        current = next;
    }
}

static void free_expr(IRExpr* expr) {
    if (!expr) return;
    switch (expr->type) {
        case IR_EXPR_LITERAL: free(((IRExprLiteral*)expr)->value); break;
        case IR_EXPR_VARIABLE: free(((IRExprVariable*)expr)->name); break;
        case IR_EXPR_FUNC_CALL: {
            IRExprFuncCall* call = (IRExprFuncCall*)expr;
            free(call->func_name);
            free_expr_list(call->args);
        } break;
        case IR_EXPR_ARRAY: free_expr_list(((IRExprArray*)expr)->elements); break;
        case IR_EXPR_ADDRESS_OF: if (((IRExprAddressOf*)expr)->expr) ir_free((IRNode*)((IRExprAddressOf*)expr)->expr); break;
        default: fprintf(stderr, "Warning: Unknown IRExpr type in free_expr: %d\n", expr->type); break;
    }
    free(expr);
}

static void free_stmt(IRStmt* stmt) {
    if (!stmt) return;
    switch (stmt->type) {
        case IR_STMT_BLOCK: free_stmt_list(((IRStmtBlock*)stmt)->stmts); break;
        case IR_STMT_VAR_DECL: {
            IRStmtVarDecl* decl = (IRStmtVarDecl*)stmt;
            free(decl->type_name);
            free(decl->var_name);
            if (decl->initializer) ir_free((IRNode*)decl->initializer);
        } break;
        case IR_STMT_FUNC_CALL: if (((IRStmtFuncCall*)stmt)->call) ir_free((IRNode*)((IRStmtFuncCall*)stmt)->call); break;
        case IR_STMT_COMMENT: free(((IRStmtComment*)stmt)->text); break;
        default: fprintf(stderr, "Warning: Unknown IRStmt type in free_stmt: %d\n", stmt->type); break;
    }
    free(stmt);
}

void ir_free(IRNode* node) {
    if (!node || !node->free) return;
    node->free(node);
}

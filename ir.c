#include "ir.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h> // For NULL (if not included by stdlib.h)

// Forward declarations for actual codegen functions from codegen.c
// These are needed by ir.c to assign to function pointers in IRNode.
void codegen_expr_literal(IRNode* node, int indent);
void codegen_expr_variable(IRNode* node, int indent);
void codegen_expr_func_call(IRNode* node, int indent);
void codegen_expr_array(IRNode* node, int indent);
void codegen_expr_address_of(IRNode* node, int indent);
void codegen_stmt_block(IRNode* node, int indent);
void codegen_stmt_var_decl(IRNode* node, int indent);
void codegen_stmt_func_call_stmt(IRNode* node, int indent);
void codegen_stmt_comment(IRNode* node, int indent);

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
    if (!lit) return NULL;
    init_ir_node((IRNode*)lit, (void (*)(IRNode*))free_expr, codegen_expr_literal);
    lit->base.type = IR_EXPR_LITERAL;
    lit->value = value ? strdup(value) : NULL;
    return (IRExpr*)lit;
}

// Specific helper for string literals that need quotes in C
IRExpr* ir_new_literal_string(const char* value) {
    IRExprLiteral* lit = (IRExprLiteral*)calloc(1, sizeof(IRExprLiteral));
    if (!lit) return NULL;
    init_ir_node((IRNode*)lit, (void (*)(IRNode*))free_expr, codegen_expr_literal);
    lit->base.type = IR_EXPR_LITERAL;
    if (value) {
        // Allocate enough space for quotes and null terminator
        char* quoted_value = (char*)malloc(strlen(value) + 3);
        if (!quoted_value) {
            free(lit);
            return NULL;
        }
        sprintf(quoted_value, "\"%s\"", value);
        lit->value = quoted_value;
    } else {
        lit->value = strdup("\"\""); // Represent NULL string as empty quoted string
    }
    return (IRExpr*)lit;
}


IRExpr* ir_new_variable(const char* name) {
    IRExprVariable* var = (IRExprVariable*)calloc(1, sizeof(IRExprVariable));
    if (!var) return NULL;
    init_ir_node((IRNode*)var, (void (*)(IRNode*))free_expr, codegen_expr_variable);
    var->base.type = IR_EXPR_VARIABLE;
    var->name = name ? strdup(name) : NULL;
    return (IRExpr*)var;
}

IRExpr* ir_new_func_call_expr(const char* func_name, IRExprNode* args) {
    IRExprFuncCall* call = (IRExprFuncCall*)calloc(1, sizeof(IRExprFuncCall));
    if (!call) return NULL;
    init_ir_node((IRNode*)call, (void (*)(IRNode*))free_expr, codegen_expr_func_call);
    call->base.type = IR_EXPR_FUNC_CALL;
    call->func_name = func_name ? strdup(func_name) : NULL;
    call->args = args;
    return (IRExpr*)call;
}

IRExpr* ir_new_array(IRExprNode* elements) {
    IRExprArray* arr = (IRExprArray*)calloc(1, sizeof(IRExprArray));
    if (!arr) return NULL;
    init_ir_node((IRNode*)arr, (void (*)(IRNode*))free_expr, codegen_expr_array);
    arr->base.type = IR_EXPR_ARRAY;
    arr->elements = elements;
    return (IRExpr*)arr;
}

IRExpr* ir_new_address_of(IRExpr* expr) {
    IRExprAddressOf* addr = (IRExprAddressOf*)calloc(1, sizeof(IRExprAddressOf));
    if (!addr) return NULL;
    init_ir_node((IRNode*)addr, (void (*)(IRNode*))free_expr, codegen_expr_address_of);
    addr->base.type = IR_EXPR_ADDRESS_OF;
    addr->expr = expr;
    return (IRExpr*)addr;
}

// --- Factory functions for Statements ---

IRStmtBlock* ir_new_block() {
    IRStmtBlock* block = (IRStmtBlock*)calloc(1, sizeof(IRStmtBlock));
    if (!block) return NULL;
    init_ir_node((IRNode*)block, (void (*)(IRNode*))free_stmt, codegen_stmt_block);
    block->base.type = IR_STMT_BLOCK;
    block->stmts = NULL;
    return block;
}

void ir_block_add_stmt(IRStmtBlock* block, IRStmt* stmt) {
    if (!block || !stmt) return;
    IRStmtNode* new_node = (IRStmtNode*)calloc(1, sizeof(IRStmtNode));
    if (!new_node) return;
    new_node->stmt = stmt;
    new_node->next = NULL;

    if (!block->stmts) {
        block->stmts = new_node;
    } else {
        IRStmtNode* current = block->stmts;
        while (current->next) {
            current = current->next;
        }
        current->next = new_node;
    }
}

IRStmt* ir_new_var_decl(const char* type_name, const char* var_name, IRExpr* initializer) {
    IRStmtVarDecl* decl = (IRStmtVarDecl*)calloc(1, sizeof(IRStmtVarDecl));
    if (!decl) return NULL;
    init_ir_node((IRNode*)decl, (void (*)(IRNode*))free_stmt, codegen_stmt_var_decl);
    decl->base.type = IR_STMT_VAR_DECL;
    decl->type_name = type_name ? strdup(type_name) : NULL;
    decl->var_name = var_name ? strdup(var_name) : NULL;
    decl->initializer = initializer;
    return (IRStmt*)decl;
}

IRStmt* ir_new_func_call_stmt(const char* func_name, IRExprNode* args) {
    IRStmtFuncCall* stmt_call = (IRStmtFuncCall*)calloc(1, sizeof(IRStmtFuncCall));
    if (!stmt_call) return NULL;
    init_ir_node((IRNode*)stmt_call, (void (*)(IRNode*))free_stmt, codegen_stmt_func_call_stmt);
    stmt_call->base.type = IR_STMT_FUNC_CALL;
    // Note: IRExprFuncCall is directly embedded, not just pointed to.
    // So we allocate it as part of this object, but its 'args' and 'func_name' will be managed by its own free logic.
    // However, the current free_expr logic for IRExprFuncCall assumes it's heap allocated.
    // This design is a bit inconsistent. For now, let's make it a pointer.
    stmt_call->call = (IRExprFuncCall*)ir_new_func_call_expr(func_name, args);
    if (!stmt_call->call) {
        free(stmt_call);
        return NULL;
    }
    return (IRStmt*)stmt_call;
}

IRStmt* ir_new_comment(const char* text) {
    IRStmtComment* comment = (IRStmtComment*)calloc(1, sizeof(IRStmtComment));
    if (!comment) return NULL;
    init_ir_node((IRNode*)comment, (void (*)(IRNode*))free_stmt, codegen_stmt_comment);
    comment->base.type = IR_STMT_COMMENT;
    comment->text = text ? strdup(text) : NULL;
    return (IRStmt*)comment;
}

// --- Factory functions for Linked List Nodes ---

IRExprNode* ir_new_expr_node(IRExpr* expr) {
    IRExprNode* node = (IRExprNode*)calloc(1, sizeof(IRExprNode));
    if (!node) return NULL;
    node->expr = expr;
    node->next = NULL;
    return node;
}

void ir_expr_list_add(IRExprNode** head, IRExpr* expr) {
    if (!head) return;
    IRExprNode* new_node = ir_new_expr_node(expr);
    if (!new_node) return; // Allocation failed

    if (!*head) {
        *head = new_node;
    } else {
        IRExprNode* current = *head;
        while (current->next) {
            current = current->next;
        }
        current->next = new_node;
    }
}

// --- Free functions ---

static void free_expr_list(IRExprNode* head) {
    IRExprNode* current = head;
    while (current) {
        IRExprNode* next = current->next;
        if (current->expr) {
            ir_free((IRNode*)current->expr);
        }
        free(current);
        current = next;
    }
}

static void free_stmt_list(IRStmtNode* head) {
    IRStmtNode* current = head;
    while (current) {
        IRStmtNode* next = current->next;
        if (current->stmt) {
            ir_free((IRNode*)current->stmt);
        }
        free(current);
        current = next;
    }
}

static void free_expr(IRExpr* expr) {
    if (!expr) return;
    switch (expr->type) {
        case IR_EXPR_LITERAL:
            {
                IRExprLiteral* lit = (IRExprLiteral*)expr;
                free(lit->value);
            }
            break;
        case IR_EXPR_VARIABLE:
            {
                IRExprVariable* var = (IRExprVariable*)expr;
                free(var->name);
            }
            break;
        case IR_EXPR_FUNC_CALL:
            {
                IRExprFuncCall* call = (IRExprFuncCall*)expr;
                free(call->func_name);
                free_expr_list(call->args);
            }
            break;
        case IR_EXPR_ARRAY:
            {
                IRExprArray* arr = (IRExprArray*)expr;
                free_expr_list(arr->elements);
            }
            break;
        case IR_EXPR_ADDRESS_OF:
            {
                IRExprAddressOf* addr = (IRExprAddressOf*)expr;
                if (addr->expr) {
                    ir_free((IRNode*)addr->expr);
                }
            }
            break;
        default:
            // Should not happen
            fprintf(stderr, "Warning: Unknown IRExpr type in free_expr: %d\n", expr->type);
            break;
    }
    free(expr);
}

static void free_stmt(IRStmt* stmt) {
    if (!stmt) return;
    switch (stmt->type) {
        case IR_STMT_BLOCK:
            {
                IRStmtBlock* block = (IRStmtBlock*)stmt;
                free_stmt_list(block->stmts);
            }
            break;
        case IR_STMT_VAR_DECL:
            {
                IRStmtVarDecl* decl = (IRStmtVarDecl*)stmt;
                free(decl->type_name);
                free(decl->var_name);
                if (decl->initializer) {
                    ir_free((IRNode*)decl->initializer);
                }
            }
            break;
        case IR_STMT_FUNC_CALL:
            {
                IRStmtFuncCall* stmt_call = (IRStmtFuncCall*)stmt;
                // The IRExprFuncCall within IRStmtFuncCall is heap-allocated by ir_new_func_call_stmt
                if (stmt_call->call) {
                    // Its free function will handle its internal members (func_name, args)
                    ir_free((IRNode*)stmt_call->call);
                }
            }
            break;
        case IR_STMT_COMMENT:
            {
                IRStmtComment* comment = (IRStmtComment*)stmt;
                free(comment->text);
            }
            break;
        default:
            // Should not happen
            fprintf(stderr, "Warning: Unknown IRStmt type in free_stmt: %d\n", stmt->type);
            break;
    }
    free(stmt);
}

void ir_free(IRNode* node) {
    if (!node || !node->free) return;
    node->free(node);
}

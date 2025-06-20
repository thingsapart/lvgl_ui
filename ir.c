#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h> // For bool type
#include <ctype.h>   // For isspace, isdigit
#include <unistd.h>  // For STDERR_FILENO (for dprintf)

#include "ir.h"
#include "api_spec.h" // For ApiSpec, FunctionDefinition, api_spec_get_function, api_spec_is_enum_value etc.
#include "registry.h" // For Registry, registry_get_type_by_id, registry_get_id_by_gen_var
#include "utils.h"    // For dprintf, if that's where it's defined
#include "codegen.h"  // For codegen function declarations

// Forward declaration
void ir_free_expr_list(IRExprNode* head);

// Helper function to check if a string represents an integer, allowing for suffixes
static bool is_integer_literal(const char* s) {
    if (!s || *s == '\0') return false;
    char* p;
    // Skip leading whitespace
    while (isspace((unsigned char)*s)) s++;
    // Check for optional sign
    if (*s == '+' || *s == '-') s++;
    if (*s == '\0') return false; // Only a sign is not an integer

    long val = strtol(s, &p, 10); // Use strtol to parse the number part
    (void)val; // Mark val as used

    if (p == s) return false; // No digits were read

    // Check for valid suffixes (L, LL, U, UL, ULL and their lowercase counterparts)
    while (*p != '\0') {
        if (*p == 'L' || *p == 'l') {
            p++;
            if (*p == 'L' || *p == 'l') p++; // LL or ll
        } else if (*p == 'U' || *p == 'u') {
            p++;
        } else {
            return false; // Invalid character after number part
        }
    }
    return true; // Entire string consumed by number and valid suffixes
}


const char* ir_expr_get_type(IRExpr* expr, const struct ApiSpec* api_spec, struct Registry* registry) {
    if (!expr) {
        return "unknown_null_expr";
    }

    static char addr_type_buf[128];

    switch (expr->type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)expr;
            if (lit->value == NULL || strcmp(lit->value, "NULL") == 0) {
                return "NULL_t";
            }
            if (strcmp(lit->value, "true") == 0 || strcmp(lit->value, "false") == 0) {
                return "bool";
            }
            size_t len = strlen(lit->value);
            if (len >= 2 && lit->value[0] == '"' && lit->value[len - 1] == '"') {
                 return "const char*";
            }
            if (is_integer_literal(lit->value)) {
                return "int";
            }
            if (api_spec && api_spec_is_enum_value(api_spec, lit->value)) {
                const cJSON* enums_json = api_spec_get_enums(api_spec);
                const cJSON* enum_type_json = NULL;
                cJSON_ArrayForEach(enum_type_json, enums_json) {
                    const char* enum_name = cJSON_GetObjectItem(enum_type_json, "name")->valuestring;
                    const cJSON* members = cJSON_GetObjectItem(enum_type_json, "members");
                    const cJSON* member_json = NULL;
                    cJSON_ArrayForEach(member_json, members) {
                        if (strcmp(lit->value, member_json->valuestring) == 0) {
                            return enum_name;
                        }
                    }
                }
            }
            bool looks_like_identifier = true;
            for(const char* c = lit->value; *c; ++c) {
                if (!isalnum((unsigned char)*c) && *c != '_') {
                    looks_like_identifier = false;
                    break;
                }
            }
            if (looks_like_identifier && !isdigit((unsigned char)lit->value[0])) {
                 dprintf(STDERR_FILENO, "DEBUG: Literal '%s' not matching known types, assuming 'const char*' due to identifier look.\n", lit->value);
                 return "const char*";
            }
            return "unknown_literal_type";
        }
        case IR_EXPR_VARIABLE: {
            IRExprVariable* var = (IRExprVariable*)expr;
            if (registry && var->name) {
                const char* type_from_id = registry_get_type_by_id(registry, var->name);
                if (type_from_id) {
                    return type_from_id;
                }
                const char* original_id = registry_get_id_by_gen_var(registry, var->name);
                if (original_id) {
                    const char* type_from_original_id = registry_get_type_by_id(registry, original_id);
                    if (type_from_original_id) {
                        return type_from_original_id;
                    }
                }
            }
            return "unknown_var_type";
        }
        case IR_EXPR_FUNC_CALL: {
            IRExprFuncCall* call = (IRExprFuncCall*)expr;
            if (api_spec && call->func_name) {
                const FunctionDefinition* func_def = api_spec_get_function(api_spec, call->func_name);
                if (func_def && func_def->return_type && func_def->return_type[0] != '\0') {
                    return func_def->return_type;
                }
            }
            return "unknown_func_ret_type";
        }
        case IR_EXPR_ARRAY: {
            return "array_t";
        }
        case IR_EXPR_ADDRESS_OF: {
            IRExprAddressOf* addr = (IRExprAddressOf*)expr;
            const char* underlying_type = ir_expr_get_type(addr->expr, api_spec, registry);
            if (underlying_type && strcmp(underlying_type, "unknown_null_expr") != 0 &&
                !strstr(underlying_type, "unknown_")) {
                snprintf(addr_type_buf, sizeof(addr_type_buf), "%s*", underlying_type);
                return addr_type_buf;
            }
            return "unknown_ptr_type";
        }
        default:
            fprintf(stderr, "Warning: Unknown or statement type (%d) in ir_expr_get_type.\n", expr->type);
            return "invalid_expr_type_in_get_type";
    }
    return "unknown_expr_type_fallback";
}

// --- Factory functions for Expressions ---
IRExpr* ir_new_literal(const char* value) {
    IRExprLiteral* expr = (IRExprLiteral*)malloc(sizeof(IRExprLiteral));
    if (!expr) { perror("malloc IRExprLiteral"); return NULL; }
    expr->base.type = IR_EXPR_LITERAL;
    expr->base.free = (IRFreeFunc)ir_free;
    expr->base.codegen = (IRCodegenFunc)codegen_expr_literal; // Corrected
    expr->value = value ? strdup(value) : NULL;
    if (value && !expr->value) { perror("strdup literal value"); free(expr); return NULL; }
    return (IRExpr*)expr;
}

IRExpr* ir_new_literal_string(const char* raw_string_content) {
    IRExprLiteral* expr = (IRExprLiteral*)malloc(sizeof(IRExprLiteral));
    if (!expr) { perror("malloc IRExprLiteral for string"); return NULL; }
    expr->base.type = IR_EXPR_LITERAL;
    expr->base.free = (IRFreeFunc)ir_free;
    expr->base.codegen = (IRCodegenFunc)codegen_expr_literal; // Corrected (uses same as literal)

    if (raw_string_content) {
        size_t len = strlen(raw_string_content);
        expr->value = (char*)malloc(len + 3);
        if (!expr->value) { perror("malloc for literal string value"); free(expr); return NULL; }
        snprintf(expr->value, len + 3, "\"%s\"", raw_string_content);
    } else {
        expr->value = strdup("\"\"");
        if (!expr->value) { perror("strdup for empty literal string"); free(expr); return NULL; }
    }
    return (IRExpr*)expr;
}


IRExpr* ir_new_variable(const char* name) {
    IRExprVariable* expr = (IRExprVariable*)malloc(sizeof(IRExprVariable));
    if (!expr) { perror("malloc IRExprVariable"); return NULL; }
    expr->base.type = IR_EXPR_VARIABLE;
    expr->base.free = (IRFreeFunc)ir_free;
    expr->base.codegen = (IRCodegenFunc)codegen_expr_variable; // Corrected
    expr->name = name ? strdup(name) : NULL;
    if (name && !expr->name) { perror("strdup var name"); free(expr); return NULL; }
    return (IRExpr*)expr;
}

IRExpr* ir_new_func_call_expr(const char* func_name, IRExprNode* args) {
    IRExprFuncCall* expr = (IRExprFuncCall*)malloc(sizeof(IRExprFuncCall));
    if (!expr) {
        perror("malloc IRExprFuncCall");
        ir_free_expr_list(args);
        return NULL;
    }
    expr->base.type = IR_EXPR_FUNC_CALL;
    expr->base.free = (IRFreeFunc)ir_free;
    expr->base.codegen = (IRCodegenFunc)codegen_expr_func_call; // Corrected
    expr->args = NULL;

    expr->func_name = func_name ? strdup(func_name) : NULL;
    if (func_name && !expr->func_name) {
        perror("strdup func_name");
        ir_free_expr_list(args);
        free(expr);
        return NULL;
    }
    expr->args = args;
    return (IRExpr*)expr;
}

IRExpr* ir_new_array(IRExprNode* elements) {
    IRExprArray* expr = (IRExprArray*)malloc(sizeof(IRExprArray));
    if (!expr) { perror("malloc IRExprArray"); ir_free_expr_list(elements); return NULL; }
    expr->base.type = IR_EXPR_ARRAY;
    expr->base.free = (IRFreeFunc)ir_free;
    expr->base.codegen = (IRCodegenFunc)codegen_expr_array; // Corrected
    expr->elements = elements;
    return (IRExpr*)expr;
}

IRExpr* ir_new_address_of(IRExpr* target_expr) {
    if (!target_expr) return NULL;
    IRExprAddressOf* expr = (IRExprAddressOf*)malloc(sizeof(IRExprAddressOf));
    if (!expr) { perror("malloc IRExprAddressOf"); ir_free(target_expr); return NULL; }
    expr->base.type = IR_EXPR_ADDRESS_OF;
    expr->base.free = (IRFreeFunc)ir_free;
    expr->base.codegen = (IRCodegenFunc)codegen_expr_address_of; // Corrected
    expr->expr = target_expr;
    return (IRExpr*)expr;
}


// --- Factory functions for Statements ---
IRStmtBlock* ir_new_block() {
    IRStmtBlock* block = (IRStmtBlock*)calloc(1, sizeof(IRStmtBlock));
    if (!block) { perror("calloc IRStmtBlock"); return NULL; }
    block->base.type = IR_STMT_BLOCK;
    block->base.free = (IRFreeFunc)ir_free;
    block->base.codegen = (IRCodegenFunc)codegen_stmt_block;
    block->stmts = NULL;
    return block;
}

void ir_block_add_stmt(IRStmtBlock* block, IRStmt* stmt) {
    if (!block || !stmt) return;
    IRStmtNode* newNode = (IRStmtNode*)malloc(sizeof(IRStmtNode));
    if (!newNode) { perror("malloc IRStmtNode"); ir_free(stmt); return; }
    newNode->stmt = stmt;
    newNode->next = NULL;

    if (block->stmts == NULL) {
        block->stmts = newNode;
    } else {
        IRStmtNode* current = block->stmts;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = newNode;
    }
}

IRStmt* ir_new_var_decl(const char* type_name, const char* var_name, IRExpr* initializer) {
    IRStmtVarDecl* stmt = (IRStmtVarDecl*)malloc(sizeof(IRStmtVarDecl));
    if (!stmt) { perror("malloc IRStmtVarDecl"); ir_free(initializer); return NULL; }
    stmt->base.type = IR_STMT_VAR_DECL;
    stmt->base.free = (IRFreeFunc)ir_free;
    stmt->base.codegen = (IRCodegenFunc)codegen_stmt_var_decl;
    stmt->type_name = type_name ? strdup(type_name) : NULL;
    stmt->var_name = var_name ? strdup(var_name) : NULL;
    stmt->initializer = initializer;

    if ((type_name && !stmt->type_name) || (var_name && !stmt->var_name)) {
        perror("strdup for var_decl");
        free(stmt->type_name);
        free(stmt->var_name);
        ir_free(stmt->initializer);
        free(stmt);
        return NULL;
    }
    return (IRStmt*)stmt;
}

IRStmt* ir_new_func_call_stmt(const char* func_name, IRExprNode* args) {
    IRStmtFuncCall* stmt = (IRStmtFuncCall*)malloc(sizeof(IRStmtFuncCall));
    if (!stmt) { perror("malloc IRStmtFuncCall"); ir_free_expr_list(args); return NULL; }
    stmt->base.type = IR_STMT_FUNC_CALL;
    stmt->base.free = (IRFreeFunc)ir_free;
    stmt->base.codegen = (IRCodegenFunc)codegen_stmt_func_call_stmt;
    stmt->call = (IRExprFuncCall*)ir_new_func_call_expr(func_name, args);
    if (!stmt->call) {
        free(stmt);
        return NULL;
    }
    return (IRStmt*)stmt;
}

IRStmt* ir_new_comment(const char* text) {
    IRStmtComment* stmt = (IRStmtComment*)malloc(sizeof(IRStmtComment));
    if (!stmt) { perror("malloc IRStmtComment"); return NULL; }
    stmt->base.type = IR_STMT_COMMENT;
    stmt->base.free = (IRFreeFunc)ir_free;
    stmt->base.codegen = (IRCodegenFunc)codegen_stmt_comment;
    stmt->text = text ? strdup(text) : NULL;
    if (text && !stmt->text) { perror("strdup comment text"); free(stmt); return NULL; }
    return (IRStmt*)stmt;
}

IRStmt* ir_new_widget_allocate_stmt(const char* c_var_name, const char* widget_c_type_name, const char* create_func_name, IRExpr* parent_expr) {
    IRStmtWidgetAllocate* stmt = (IRStmtWidgetAllocate*)malloc(sizeof(IRStmtWidgetAllocate));
    if (!stmt) { perror("malloc IRStmtWidgetAllocate"); ir_free(parent_expr); return NULL; }
    stmt->base.type = IR_STMT_WIDGET_ALLOCATE;
    stmt->base.free = (IRFreeFunc)ir_free;
    stmt->base.codegen = (IRCodegenFunc)codegen_stmt_widget_allocate;
    stmt->c_var_name = c_var_name ? strdup(c_var_name) : NULL;
    stmt->widget_c_type_name = widget_c_type_name ? strdup(widget_c_type_name) : NULL;
    stmt->create_func_name = create_func_name ? strdup(create_func_name) : NULL;
    stmt->parent_expr = parent_expr;

    if ((c_var_name && !stmt->c_var_name) || (widget_c_type_name && !stmt->widget_c_type_name) || (create_func_name && !stmt->create_func_name)) {
        perror("strdup for widget_allocate_stmt");
        free(stmt->c_var_name);
        free(stmt->widget_c_type_name);
        free(stmt->create_func_name);
        ir_free(stmt->parent_expr);
        free(stmt);
        return NULL;
    }
    return (IRStmt*)stmt;
}

IRStmt* ir_new_object_allocate_stmt(const char* c_var_name, const char* object_c_type_name, const char* init_func_name) {
    IRStmtObjectAllocate* stmt = (IRStmtObjectAllocate*)malloc(sizeof(IRStmtObjectAllocate));
    if (!stmt) { perror("malloc IRStmtObjectAllocate"); return NULL; }
    stmt->base.type = IR_STMT_OBJECT_ALLOCATE;
    stmt->base.free = (IRFreeFunc)ir_free;
    stmt->base.codegen = (IRCodegenFunc)codegen_stmt_object_allocate;
    stmt->c_var_name = c_var_name ? strdup(c_var_name) : NULL;
    stmt->object_c_type_name = object_c_type_name ? strdup(object_c_type_name) : NULL;
    stmt->init_func_name = init_func_name ? strdup(init_func_name) : NULL;

    if ((c_var_name && !stmt->c_var_name) || (object_c_type_name && !stmt->object_c_type_name) || (init_func_name && !stmt->init_func_name)) {
        perror("strdup for object_allocate_stmt");
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
    IRExprNode* node = (IRExprNode*)malloc(sizeof(IRExprNode));
    if (!node) { perror("malloc IRExprNode"); ir_free(expr); return NULL; }
    node->expr = expr;
    node->next = NULL;
    return node;
}

void ir_expr_list_add(IRExprNode** head, IRExpr* expr) {
    if (!expr) return;
    IRExprNode* newNode = ir_new_expr_node(expr);
    if (!newNode) return;

    if (*head == NULL) {
        *head = newNode;
    } else {
        IRExprNode* current = *head;
        while (current->next != NULL) {
            current = current->next;
        }
        current->next = newNode;
    }
}

void ir_free_expr_list(IRExprNode* head) {
    IRExprNode* current = head;
    while (current) {
        IRExprNode* next = current->next;
        ir_free(current->expr);
        free(current);
        current = next;
    }
}


// --- Memory Management ---
void ir_free(IRNode* node) {
    if (!node) return;

    switch (node->type) {
        case IR_EXPR_LITERAL:
            free(((IRExprLiteral*)node)->value);
            break;
        case IR_EXPR_VARIABLE:
            free(((IRExprVariable*)node)->name);
            break;
        case IR_EXPR_FUNC_CALL: {
            IRExprFuncCall* call = (IRExprFuncCall*)node;
            free(call->func_name);
            ir_free_expr_list(call->args);
            break;
        }
        case IR_EXPR_ARRAY:
            ir_free_expr_list(((IRExprArray*)node)->elements);
            break;
        case IR_EXPR_ADDRESS_OF:
            ir_free(((IRExprAddressOf*)node)->expr);
            break;
        case IR_STMT_BLOCK: {
            IRStmtBlock* block = (IRStmtBlock*)node;
            IRStmtNode* current_stmt = block->stmts;
            while (current_stmt) {
                IRStmtNode* next_stmt = current_stmt->next;
                ir_free(current_stmt->stmt);
                free(current_stmt);
                current_stmt = next_stmt;
            }
            break;
        }
        case IR_STMT_VAR_DECL: {
            IRStmtVarDecl* decl = (IRStmtVarDecl*)node;
            free(decl->type_name);
            free(decl->var_name);
            ir_free(decl->initializer);
            break;
        }
        case IR_STMT_FUNC_CALL:
            ir_free((IRNode*)((IRStmtFuncCall*)node)->call);
            break;
        case IR_STMT_COMMENT:
            free(((IRStmtComment*)node)->text);
            break;
        case IR_STMT_WIDGET_ALLOCATE: {
            IRStmtWidgetAllocate* wa = (IRStmtWidgetAllocate*)node;
            free(wa->c_var_name);
            free(wa->widget_c_type_name);
            free(wa->create_func_name);
            ir_free(wa->parent_expr);
            break;
        }
        case IR_STMT_OBJECT_ALLOCATE: {
            IRStmtObjectAllocate* oa = (IRStmtObjectAllocate*)node;
            free(oa->c_var_name);
            free(oa->object_c_type_name);
            free(oa->init_func_name);
            break;
        }
        default:
            fprintf(stderr, "Error: Unknown IRNode type %d in ir_free.\n", node->type);
            break;
    }
    free(node);
}

#include "codegen.h"

#include <stdbool.h>
#include <stdio.h> // For printf
#include <string.h> // For strcmp, strstr

#include "ir.h"
#include "utils.h"

// --- Forward declarations for recursive codegen functions ---
static void codegen_stmt_internal(IRStmt* stmt, int indent_level);
static void codegen_expr_internal(IRExpr* expr); // Expressions generally don't need indent level directly

// --- Helper for indentation ---
static void print_indent(int level) {
    for (int i = 0; i < level; ++i) {
        printf("    "); // 4 spaces per indent level
    }
}

// --- Codegen function implementations ---

// These are the functions assigned to IRNode->codegen
void codegen_expr_literal(IRNode* node, int indent_level) {
    (void)indent_level; // Not used for expressions directly
    IRExprLiteral* lit = (IRExprLiteral*)node;
    if (lit->value) {
        // ir_new_literal_string already adds quotes for strings.
        // Other literals (numbers, constants like LV_ALIGN_CENTER) are printed as is.
        printf("%s", lit->value);
    } else {
        printf("NULL");
    }
}

void codegen_expr_variable(IRNode* node, int indent_level) {
    (void)indent_level;
    IRExprVariable* var = (IRExprVariable*)node;
    if (var->name) {
        printf("%s", var->name);
    } else {
        printf("/* unnamed variable */");
    }
}

void codegen_expr_func_call(IRNode* node, int indent_level) {
    (void)indent_level;
    IRExprFuncCall* call = (IRExprFuncCall*)node;
    if (call->func_name) {
        _dprintf(stderr, "CODEGEN_EXPR_FUNC_CALL: Processing func_name '%s'\n", call->func_name); fflush(stderr);
        printf("%s(", call->func_name);
        IRExprNode* current_arg = call->args;
        int arg_count = 0;
        while (current_arg) {
            if (arg_count > 0) {
                printf(", ");
            }
            if (current_arg->expr) {
                codegen_expr_internal(current_arg->expr);
            } else {
                printf("NULL /* missing arg expr */");
            }
            current_arg = current_arg->next;
            arg_count++;
        }
        printf(")");
    } else {
        printf("/* unnamed function call */");
    }
}

void codegen_expr_array(IRNode* node, int indent_level) {
    (void)indent_level;
    IRExprArray* arr = (IRExprArray*)node;
    // C array initializers are tricky without knowing the type.
    // LVGL often uses them for lv_style_prop_t value arrays or point arrays.
    // For generic C, it would be like '(type_t[]){elem1, elem2}'
    // For now, let's assume a simple comma-separated list suitable for function args
    // or simple array initializers where type is inferred or fixed.
    // This might need refinement based on how arrays are used by LVGL setters.
    // If it's for a style property array, the setter handles it.
    // If it's for, e.g., lv_chart_set_points, it's also fine.
    printf("{"); // Using braces for compound literal or array init
    IRExprNode* current_elem = arr->elements;
    int elem_count = 0;
    while (current_elem) {
        if (elem_count > 0) {
            printf(", ");
        }
        if (current_elem->expr) {
            codegen_expr_internal(current_elem->expr);
        } else {
            printf("NULL /* missing array element expr */");
        }
        current_elem = current_elem->next;
        elem_count++;
    }
    printf("}");
}

void codegen_expr_address_of(IRNode* node, int indent_level) {
    (void)indent_level;
    IRExprAddressOf* addr = (IRExprAddressOf*)node;
    printf("&");
    if (addr->expr) {
        // Parentheses might be needed depending on precedence: &(var.member) vs &var.member
        // codegen_expr_internal will handle the inner expression.
        // For simple variables or function calls, it's fine.
        codegen_expr_internal(addr->expr);
    } else {
        printf("NULL /* missing address_of expr */");
    }
}

void codegen_stmt_block(IRNode* node, int indent_level) {
    IRStmtBlock* block = (IRStmtBlock*)node;
    _dprintf(stderr, "CODEGEN_BLOCK: Entering block %p, indent: %d\n", (void*)block, indent_level); fflush(stderr);
    print_indent(indent_level);
    printf("{\n");

    IRStmtNode* current_stmt_node = block->stmts;
    while (current_stmt_node) {
        if (current_stmt_node->stmt) {
            _dprintf(stderr, "CODEGEN_BLOCK: Processing IRStmt type: %d in block %p\n", current_stmt_node->stmt->type, (void*)block); fflush(stderr);
            codegen_stmt_internal(current_stmt_node->stmt, indent_level + 1);
        }
        current_stmt_node = current_stmt_node->next;
    }

    print_indent(indent_level);
    printf("}\n");
    _dprintf(stderr, "CODEGEN_BLOCK: Exiting block %p\n", (void*)block); fflush(stderr);
}

void codegen_stmt_var_decl(IRNode* node, int indent_level) {
    IRStmtVarDecl* decl = (IRStmtVarDecl*)node;
    print_indent(indent_level);
    if (decl->type_name) {
        printf("%s ", decl->type_name);
    } else {
        printf("/* untyped */ ");
    }
    if (decl->var_name) {
        printf("%s", decl->var_name);
    } else {
        printf("/* unnamed_var */");
    }
    if (decl->initializer) {
        printf(" = ");
        codegen_expr_internal(decl->initializer);
    }
    printf(";\n");
}

// Name changed from ir.c's dummy to avoid potential conflict if they were in same file
void codegen_stmt_func_call_stmt(IRNode* node, int indent_level) {
    IRStmtFuncCall* stmt_call = (IRStmtFuncCall*)node;
    if (stmt_call->call && stmt_call->call->func_name) {
        _dprintf(stderr, "CODEGEN_FUNC_CALL_STMT: Generating call for function '%s'\n", stmt_call->call->func_name); fflush(stderr);
    } else {
        _dprintf(stderr, "CODEGEN_FUNC_CALL_STMT: Generating call for UNKNOWN function\n"); fflush(stderr);
    }
    print_indent(indent_level);
    if (stmt_call->call) {
        IRNode* call_as_node = (IRNode*)stmt_call->call; // Cast IRExprFuncCall* to IRNode*
        if (call_as_node && call_as_node->codegen) {    // Check for NULL before calling
            call_as_node->codegen(call_as_node, indent_level);
        } else if (stmt_call->call) { // If call_as_node is NULL but stmt_call->call wasn't, it implies codegen ptr was NULL
             printf("/* codegen function pointer missing for func_call_expr */");
        } else {
             printf("/* NULL func_call_expr in stmt */");
        }
        // A function call used as a statement needs a semicolon.
        printf(";\n");
    } else {
        printf("/* empty function call stmt */;\n");
    }
}

void codegen_stmt_comment(IRNode* node, int indent_level) {
    IRStmtComment* comment = (IRStmtComment*)node;
    print_indent(indent_level);
    if (comment->text) {
        printf("// %s\n", comment->text);
    } else {
        printf("// empty comment\n");
    }
}

void codegen_stmt_widget_allocate(IRNode* node, int indent_level) {
    IRStmtWidgetAllocate* stmt = (IRStmtWidgetAllocate*)node;
    print_indent(indent_level);

    const char* c_type = stmt->widget_c_type_name ? stmt->widget_c_type_name : "lv_obj_t";

    // LVGL create functions return TYPE*, so no '*' needed in c_type here for the variable declaration.
    // e.g. lv_obj_t * my_btn = lv_btn_create(...); widget_c_type_name should be "lv_obj_t"
    printf("%s* %s = %s(", c_type, stmt->c_var_name, stmt->create_func_name);
    if (stmt->parent_expr) {
        codegen_expr_internal(stmt->parent_expr);
    } else {
        // Default to lv_scr_act() if parent_expr is NULL, as most LVGL objects need a parent.
        // This is a common convention for top-level objects on the current screen.
        printf("lv_scr_act()");
    }
    printf(");\n");
}

void codegen_stmt_object_allocate(IRNode* node, int indent_level) {
    IRStmtObjectAllocate* stmt = (IRStmtObjectAllocate*)node;

    // Line 1: TYPE* var_name = (TYPE*)malloc(sizeof(TYPE));
    print_indent(indent_level);
    printf("%s* %s = (%s*)malloc(sizeof(%s));\n",
           stmt->object_c_type_name, // e.g. "lv_style_t"
           stmt->c_var_name,
           stmt->object_c_type_name,
           stmt->object_c_type_name);

    // Line 2: if (var_name != NULL) { ... }
    print_indent(indent_level);
    printf("if (%s != NULL) {\n", stmt->c_var_name);

    // Line 3: memset(var_name, 0, sizeof(TYPE));
    print_indent(indent_level + 1);
    printf("memset(%s, 0, sizeof(%s));\n",
           stmt->c_var_name,
           stmt->object_c_type_name);

    // Line 4: init_func(var_name); // Assumes init_func takes Type*
    print_indent(indent_level + 1);
    printf("%s(%s);\n",
           stmt->init_func_name,
           stmt->c_var_name); // Pass the pointer directly

    print_indent(indent_level);
    printf("} else {\n");
    print_indent(indent_level + 1);
    // Using __FILE__ and __LINE__ directly in a printf like this might be tricky
    // if this generated code is then compiled elsewhere.
    // A simpler error message might be better, or a dedicated error macro.
    // For now, keeping it simple:
    printf("fprintf(stderr, \"Error: Failed to malloc for object %%s of type %%s\\n\", \"%s\", \"%s\");\n", // Escaped %s and quotes
            stmt->c_var_name, stmt->object_c_type_name);
    print_indent(indent_level);
    printf("}\n");
}


// --- Internal dispatchers ---
static void codegen_expr_internal(IRExpr* expr) {
    if (!expr) {
        printf("/* invalid expr (NULL) */");
        return;
    }
    IRNode* expr_as_node = (IRNode*)expr; // Cast IRExpr* to IRNode*
    if (expr_as_node->codegen) {
        expr_as_node->codegen(expr_as_node, 0); // Indent level not really used by exprs
    } else {
        printf("/* codegen function pointer missing for expr */");
    }
}

static void codegen_stmt_internal(IRStmt* stmt, int indent_level) {
    if (!stmt) {
        print_indent(indent_level);
        printf("/* invalid stmt (NULL) */;\n");
        return;
    }
    IRNode* stmt_as_node = (IRNode*)stmt; // Cast IRStmt* to IRNode*
    if (stmt_as_node->codegen) {
        stmt_as_node->codegen(stmt_as_node, indent_level);
    } else {
        print_indent(indent_level);
        printf("/* codegen function pointer missing for stmt */;\n");
    }
}


// --- Main entry point for codegen ---
void codegen_generate_c(IRStmtBlock* root_block, const char* parent_var_name) {
    // parent_var_name is the name of the C variable for the root LVGL object (e.g., "screen" or "parent")
    // This codegen currently assumes the IR starts "inside" a function like `void create_ui(lv_obj_t* parent_var_name) { ... IR ... }`
    // So, the 'parent_var_name' is mostly for context if the IR refers to its top-level parent.
    // The IR generated by process_node usually creates its own top-level parent widgets or uses 'parent' passed to create_func.

    if (!root_block) {
        printf("// No IR root block provided to codegen_generate_c.\n");
        return;
    }

    // The root_block itself is a block of statements.
    // It doesn't get its own braces unless it's nested.
    // The create_ui function provides the outer scope.
    IRStmtNode* current_stmt_node = root_block->stmts;
    while (current_stmt_node) {
        if (current_stmt_node->stmt) {
            // Start top-level statements in the root block at indent level 1 (inside the function).
            codegen_stmt_internal(current_stmt_node->stmt, 1);
        }
        current_stmt_node = current_stmt_node->next;
    }
}

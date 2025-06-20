#ifndef CODEGEN_H
#define CODEGEN_H

#include "ir.h" // For IRNode and specific IR struct types

// --- Codegen Function Declarations for Statements ---
// All these functions now take IRNode* as their first parameter to match
// the IRCodegenFunc type and the definitions in codegen.c.
// The specific type casting will occur within the function definitions.

void codegen_stmt_block(IRNode* node, int indent_level);
void codegen_stmt_var_decl(IRNode* node, int indent_level);
void codegen_stmt_func_call_stmt(IRNode* node, int indent_level);
void codegen_stmt_comment(IRNode* node, int indent_level);
void codegen_stmt_widget_allocate(IRNode* node, int indent_level);
void codegen_stmt_object_allocate(IRNode* node, int indent_level);

// --- Codegen Function Declarations for Expressions ---
// These also now take IRNode* as their first parameter.
void codegen_expr_literal(IRNode* node, int indent_level);
void codegen_expr_variable(IRNode* node, int indent_level);
void codegen_expr_func_call(IRNode* node, int indent_level); // Note: Renamed from codegen_expr_func_call_expr for consistency
void codegen_expr_array(IRNode* node, int indent_level);
void codegen_expr_address_of(IRNode* node, int indent_level);


// --- Main C Code Generation Function ---
// Generates C code from the IR tree.
// root_block: The root of the IR statement tree.
// parent_var_name: Name of the parent lv_obj_t* for the UI (e.g. "parent" or "screen_main").
//                  This is primarily for the top-level screen objects.
void codegen_generate_c(IRStmtBlock* root_block, const char* initial_parent_var_name);

#endif // CODEGEN_H

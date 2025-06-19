#ifndef CODEGEN_H
#define CODEGEN_H

#include "ir.h" // For IRStmtBlock type

// Generates C code from the IR tree.
// root_block: The root of the IR statement tree.
// parent_var_name: Name of the parent lv_obj_t* for the UI (e.g. "parent" or "screen").
void codegen_generate_c(IRStmtBlock* root_block, const char* parent_var_name);

#endif // CODEGEN_H

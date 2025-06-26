#ifndef CODEGEN_H
#define CODEGEN_H

#include "ir.h"
#include "api_spec.h"

// Generates C code from the IR tree.
// root: The root of the IR tree.
// api_spec: The API specification for resolving properties to functions.
void codegen_generate_c(IRRoot* root, const ApiSpec* api_spec);

#endif // CODEGEN_H

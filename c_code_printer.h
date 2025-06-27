#ifndef C_CODE_PRINTER_H
#define C_CODE_PRINTER_H

#include "ir.h"
#include "api_spec.h"

/**
 * @brief The main function for the C code generation backend.
 *
 * This function traverses the IR tree and prints a valid, human-readable
 * C source file to stdout that will construct the UI described in the IR.
 *
 * @param root The root of the IR tree to print.
 * @param api_spec The API specification, used for context if needed.
 */
void c_code_print_backend(IRRoot* root, const ApiSpec* api_spec);

#endif // C_CODE_PRINTER_H

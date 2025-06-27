#ifndef IR_DEBUG_PRINTER_H
#define IR_DEBUG_PRINTER_H

#include "ir.h"
#include "api_spec.h"

/**
 * @brief The main function for the detailed IR debugging backend.
 *
 * This function traverses the IR tree and prints a highly detailed,
 * structured representation to stdout, including node types and hierarchy.
 * It is intended for in-depth debugging of the generator/parser phase.
 *
 * @param root The root of the IR tree to print.
 * @param api_spec The API specification (not used by this backend but
 *                 kept for a consistent backend function signature).
 */
void ir_debug_print_backend(IRRoot* root, const ApiSpec* api_spec);

#endif // IR_DEBUG_PRINTER_H

#ifndef IR_PRINTER_H
#define IR_PRINTER_H

#include "ir.h"
#include "api_spec.h"

/**
 * @brief The main function for the IR printing backend.
 *
 * This function traverses the entire IR tree and prints a structured,
 * indented representation of it to stdout. It's primarily used for
 * debugging the generator/parser phase.
 *
 * @param root The root of the IR tree to print.
 * @param api_spec The API specification (not used by this backend but
 *                 kept for a consistent backend function signature).
 */
void ir_print_backend(IRRoot* root, const ApiSpec* api_spec);

#endif // IR_PRINTER_H

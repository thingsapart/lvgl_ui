#ifndef WARNING_PRINTER_H
#define WARNING_PRINTER_H

#include "ir.h"

/**
 * @brief A backend that traverses the IR tree and prints a summary of all
 *        embedded warnings and hints.
 *
 * This function is intended to be run after all other backends to provide a
 * consolidated list of potential issues or best-practice suggestions found
 * during the generation phase.
 *
 * @param root The root of the IR tree to inspect.
 */
void warning_print_backend(IRRoot* root);

#endif // WARNING_PRINTER_H

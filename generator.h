#ifndef GENERATOR_H
#define GENERATOR_H

#include "ir.h"
#include "api_spec.h"
#include <cJSON.h>

/**
 * @brief Generates an Intermediate Representation (IR) tree from a UI specification JSON.
 *
 * This function is the main entry point for the parser. It takes the parsed UI JSON
 * and the API specification, and translates the declarative UI into a tree of IR nodes

 * that can be used for code generation or direct rendering. The top-level objects
 * in the UI spec are assumed to be parented to a pre-existing object, typically
 * identified by the name "parent".
 *
 * @param ui_spec_root The root of the parsed UI specification JSON (must be an array).
 * @param api_spec The parsed LVGL API specification.
 * @return A pointer to the root of the generated IR tree (IRRoot). The caller is
 *         responsible for freeing this tree using ir_free(). Returns NULL on error.
 */
IRRoot* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec);

#endif // GENERATOR_H

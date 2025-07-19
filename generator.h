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

/**
 * @brief Reads a UI specification file (JSON or YAML), parses it, and generates an IR tree.
 *
 * This is a convenience wrapper that handles file I/O and parsing before calling
 * `generate_ir_from_ui_spec`. It cleans up its own intermediate resources (file content buffer,
 * cJSON object).
 *
 * @param ui_spec_path The path to the UI specification file.
 * @param api_spec The parsed LVGL API specification.
 * @return A pointer to the root of the generated IR tree (IRRoot), or NULL on any error
 *         (file not found, parse error, generation error). The caller is responsible
 *         for freeing the returned tree.
 */
IRRoot* generate_ir_from_file(const char* ui_spec_path, const ApiSpec* api_spec);

/**
 * @brief Parses a UI specification string (JSON or YAML) and generates an IR tree.
 *
 * This is a convenience wrapper that handles parsing before calling
 * `generate_ir_from_ui_spec`. It cleans up its own intermediate resources (cJSON object).
 * This is the preferred method for in-memory operations like the VSCode server.
 *
 * @param ui_spec_string A string containing the UI specification.
 * @param api_spec The parsed LVGL API specification.
 * @return A pointer to the root of the generated IR tree (IRRoot), or NULL on any error
 *         (parse error, generation error). The caller is responsible for freeing the
 *         returned tree.
 */
IRRoot* generate_ir_from_string(const char* ui_spec_string, const ApiSpec* api_spec);


#endif // GENERATOR_H

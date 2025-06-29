#ifndef YAML_PARSER_H
#define YAML_PARSER_H

#include <cJSON.h>

/**
 * @brief Parses a string containing simple YAML content into a cJSON object.
 *
 * This is a limited YAML parser designed specifically for the lvgl_ui_generator's
 * file format. It supports:
 * - Top-level lists.
 * - Nested key-value maps.
 * - Indentation-based structure.
 * - Flow-style lists (`[a, b, c]`) and maps (`{k: v}`).
 * - Basic scalars (strings, numbers, bools, null).
 * - Comments starting with '#'.
 * - **Crucially, it preserves duplicate keys within a map**, which is
 *   necessary for properties like `add_style`.
 *
 * @param yaml_content A null-terminated string containing the YAML data.
 * @param error_message A pointer to a char* that will be allocated and set
 *                      to an error message if parsing fails. The caller is
 *                      responsible for freeing this string. If parsing is
 *                      successful, this will be set to NULL.
 * @return A pointer to the root cJSON object, or NULL on failure.
 *         The caller is responsible for freeing the returned cJSON tree
 *         using cJSON_Delete().
 */
cJSON* yaml_to_cjson(const char* yaml_content, char** error_message);

#endif // YAML_PARSER_H

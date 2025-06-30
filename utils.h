#ifndef UTILS_H
#define UTILS_H

#include <stddef.h> // For size_t

char* read_file(const char* filename);

#ifdef __DEBUG
  #define _dprintf fprintf
  #define _eprintf fprintf
#else
  #define _dprintf(...)
  #define _eprintf fprintf
#endif

#define ANSI_BOLD_RED "\x1b[1;31m"
#define ANSI_BOLD_LIGHT_BLUE "\033[1;94m"
#define ANSI_BOLD_LIGHT_RED "\033[1;91m"

#define ANSI_RESET    "\x1b[0m"

// Converts a C type string (e.g., "lv_label_t*", "lv_btn_t*")
// into the simplified object type string used by api_spec_find_property
// (e.g., "label", "button"). Defaults to "obj".
const char* get_obj_type_from_c_type(const char* c_type_str);

// Unescapes a C-style string literal. Handles common escape sequences like \n, \t, \xHH.
// The returned string is heap-allocated and must be freed by the caller.
// It can contain null bytes, so the length is returned in out_len.
char* unescape_c_string(const char* input, size_t* out_len);

// Forward declare IRNode and ApiSpec to avoid pulling in full headers here if not needed,
// or include them if their definitions are small / commonly used with utils.
// For now, let's assume forward declaration is okay for the .h
struct IRNode;
struct ApiSpec;

// Helper to get enum value from IRNode, potentially looking up string symbols
long ir_node_get_enum_value(struct IRNode* node, const char* expected_enum_c_type, struct ApiSpec* spec);

// Function to call when a rendering or code generation error occurs that should stop execution.
extern void render_abort(const char *msg);

// Shows a warning.
void print_warning(const char *msg, ...);

// Shows a hint.
void print_hint(const char *msg, ...);

#endif // UTILS_H

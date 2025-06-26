#ifndef UTILS_H
#define UTILS_H

char* read_file(const char* filename);

#ifdef __DEBUG
  #define _dprintf fprintf
  #define _eprintf fprintf
#else
  #define _dprintf(...)
  #define _eprintf fprintf
#endif

// Converts a C type string (e.g., "lv_label_t*", "lv_btn_t*")
// into the simplified object type string used by api_spec_find_property
// (e.g., "label", "button"). Defaults to "obj".
const char* get_obj_type_from_c_type(const char* c_type_str);

// Forward declare IRNode and ApiSpec to avoid pulling in full headers here if not needed,
// or include them if their definitions are small / commonly used with utils.
// For now, let's assume forward declaration is okay for the .h
struct IRNode;
struct ApiSpec;

// Helper to get enum value from IRNode, potentially looking up string symbols
long ir_node_get_enum_value(struct IRNode* node, const char* expected_enum_c_type, struct ApiSpec* spec);

// Function to call when a rendering or code generation error occurs that should stop execution.
extern void render_abort(const char *msg);

#endif // UTILS_H

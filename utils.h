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

#endif // UTILS_H

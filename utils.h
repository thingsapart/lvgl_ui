#ifndef UTILS_H
#define UTILS_H

char* read_file(const char* filename);

#define __DEBUG 1 // Ensure __DEBUG is defined

#ifdef __DEBUG
  #include <stdio.h> // Required for fprintf, stderr
  // Macro to print debug messages, including file, line, and function
  #define _dprintf(fp, fmt, ...) fprintf(fp, "%s:%d:%s(): " fmt, \
                                    __FILE__, __LINE__, __func__, ##__VA_ARGS__)
#else
  #define _dprintf(...) (void)0 // Suppress warnings for unused parameters if not debugging
#endif

// Converts a C type string (e.g., "lv_label_t*", "lv_btn_t*")
// into the simplified object type string used by api_spec_find_property
// (e.g., "label", "button"). Defaults to "obj".
const char* get_obj_type_from_c_type(const char* c_type_str);

#endif // UTILS_H

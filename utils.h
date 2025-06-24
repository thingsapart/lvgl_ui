#ifndef UTILS_H
#define UTILS_H

char* read_file(const char* filename);

// _dprintf macro has been removed and replaced by the DEBUG_LOG system in debug_log.h

// Converts a C type string (e.g., "lv_label_t*", "lv_btn_t*")
// into the simplified object type string used by api_spec_find_property
// (e.g., "label", "button"). Defaults to "obj".
const char* get_obj_type_from_c_type(const char* c_type_str);

#endif // UTILS_H

#include <stdio.h>
#include <stdlib.h>
#include "utils.h"

char* read_file(const char* filename) {
    FILE* f = fopen(filename, "rb");
    if (!f) {
        perror("fopen");
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long length = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* buffer = (char*)malloc(length + 1);
    if (buffer) {
        fread(buffer, 1, length, f);
        buffer[length] = '\0';
    }
    fclose(f);
    return buffer;
}

#include <string.h>

// Helper function to convert C type string to simplified object type string
const char* get_obj_type_from_c_type(const char* c_type_str) {
    if (c_type_str == NULL) {
        return "obj";
    }

    if (strcmp(c_type_str, "lv_label_t*") == 0) {
        return "label";
    } else if (strcmp(c_type_str, "lv_btn_t*") == 0) {
        return "button";
    } else if (strcmp(c_type_str, "lv_style_t*") == 0) {
        return "style";
    } else if (strcmp(c_type_str, "lv_obj_t*") == 0) {
        return "obj";
    }
    // Add more mappings as needed
    // ...

    // Default if no specific match
    return "obj";
}

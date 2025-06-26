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

#include "ir.h"       // For IRNode, IRExpr, etc.
#include "api_spec.h" // For ApiSpec, api_spec_find_enum_value
#include "debug_log.h"
#include <stdlib.h> // For strtol
#include <string.h> // For strcmp

// Implementation for ir_node_get_enum_value
long ir_node_get_enum_value(struct IRNode* node, const char* expected_enum_c_type, struct ApiSpec* spec) {
    if (!node) {
        DEBUG_LOG(LOG_MODULE_UTILS, "ir_node_get_enum_value: IRNode is NULL, defaulting to 0 for type %s", expected_enum_c_type);
        return 0;
    }

    // The node->type directly gives the IRExpr type if it's an expression.
    // IRNode itself is a union-like structure where 'type' indicates how to cast it.
    switch (node->type) {
        case IR_EXPR_ENUM:
            return ((IRExprEnum*)node)->value;
        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)node;
            if (lit->is_string && lit->value) { // If it's a string literal
                long enum_val;
                // Use api_spec to look up the string symbol
                if (api_spec_find_enum_value(spec, expected_enum_c_type, lit->value, &enum_val)) {
                    return enum_val;
                } else {
                    // Fallback: if the string is actually a number like "0", "1", try to parse it
                    char* endptr;
                    long val_from_str = strtol(lit->value, &endptr, 10);
                    if (endptr != lit->value && *endptr == '\0') { // Check if the entire string was a valid number
                        DEBUG_LOG(LOG_MODULE_UTILS, "Enum symbol '%s' for type '%s' not found, but parsed as integer %ld from string.", lit->value, expected_enum_c_type, val_from_str);
                        return val_from_str;
                    }
                    DEBUG_LOG(LOG_MODULE_UTILS, "Failed to find enum symbol '%s' for type '%s', and not a plain integer string. Defaulting to 0.", lit->value, expected_enum_c_type);
                    return 0; // Default on lookup failure
                }
            } else if (lit->value) { // Numeric literal (stored as string, e.g. "123")
                return (long)strtol(lit->value, NULL, 10);
            } else { // Null literal value or other non-string, non-numeric literal
                DEBUG_LOG(LOG_MODULE_UTILS, "ir_node_get_enum_value: Literal value is NULL or not a parsable number for type %s. Defaulting to 0.", expected_enum_c_type);
                return 0;
            }
        }
        default:
            // This case might be hit if an unexpected IRNode type is passed (e.g. IR_NODE_OBJECT instead of an expression type)
            // Or if it's an expression type not handled above (e.g. IR_EXPR_FUNCTION_CALL, IR_EXPR_ARRAY etc.)
            DEBUG_LOG(LOG_MODULE_UTILS, "Cannot get enum value from IRNode type %d for C type %s. Defaulting to 0.",
                      node->type, expected_enum_c_type);
            return 0; // Default for unhandled IRNode types or non-expression nodes
    }
}

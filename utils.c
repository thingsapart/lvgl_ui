#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "utils.h"
#include <limits.h>

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
#include <ctype.h>

// Helper function to convert a single hex digit character to its integer value
static int hex_digit_to_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

char* unescape_c_string(const char* input, size_t* out_len) {
    if (!input) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    size_t input_len = strlen(input);
    char* output = (char*)malloc(input_len + 1); // Worst case is no escapes, so same length.
    if (!output) {
        if (out_len) *out_len = 0;
        return NULL;
    }

    size_t i = 0, j = 0;
    while (i < input_len) {
        if (input[i] == '\\') {
            i++; // Skip the backslash
            if (i >= input_len) {
                // Dangling backslash at the end
                output[j++] = '\\';
                break;
            }
            switch (input[i]) {
                case 'n': output[j++] = '\n'; break;
                case 't': output[j++] = '\t'; break;
                case 'r': output[j++] = '\r'; break;
                case 'b': output[j++] = '\b'; break;
                case 'f': output[j++] = '\f'; break;
                case 'v': output[j++] = '\v'; break;
                case '\\': output[j++] = '\\'; break;
                case '\'': output[j++] = '\''; break;
                case '"': output[j++] = '"'; break;
                case 'x':
                case 'X': {
                    i++; // Skip 'x'
                    int val1 = -1, val2 = -1;
                    if (i < input_len) val1 = hex_digit_to_val(input[i]);

                    if (val1 != -1 && (i + 1) < input_len) {
                        val2 = hex_digit_to_val(input[i+1]);
                        if (val2 != -1) {
                            output[j++] = (char)((val1 << 4) | val2);
                            i++; // consumed second hex digit
                        } else {
                            output[j++] = (char)val1;
                        }
                    } else if (val1 != -1) {
                         output[j++] = (char)val1;
                    } else {
                        // Incomplete hex escape, treat as literal 'x'
                        i--; // backtrack to 'x'
                        output[j++] = input[i];
                    }
                    break;
                }
                case 'u': {
                    // Expect 4 hex digits, e.g., \uF00C
                    if (i + 4 < input_len) {
                        int d1 = hex_digit_to_val(input[i + 1]);
                        int d2 = hex_digit_to_val(input[i + 2]);
                        int d3 = hex_digit_to_val(input[i + 3]);
                        int d4 = hex_digit_to_val(input[i + 4]);
                        if (d1 != -1 && d2 != -1 && d3 != -1 && d4 != -1) {
                            unsigned int codepoint = (d1 << 12) | (d2 << 8) | (d3 << 4) | d4;

                            // Encode codepoint as UTF-8
                            if (codepoint < 0x80) {
                                output[j++] = (char)codepoint;
                            } else if (codepoint < 0x800) {
                                output[j++] = 0xC0 | (codepoint >> 6);
                                output[j++] = 0x80 | (codepoint & 0x3F);
                            } else if (codepoint < 0x10000) {
                                output[j++] = 0xE0 | (codepoint >> 12);
                                output[j++] = 0x80 | ((codepoint >> 6) & 0x3F);
                                output[j++] = 0x80 | (codepoint & 0x3F);
                            } else if (codepoint < 0x110000) {
                                output[j++] = 0xF0 | (codepoint >> 18);
                                output[j++] = 0x80 | ((codepoint >> 12) & 0x3F);
                                output[j++] = 0x80 | ((codepoint >> 6) & 0x3F);
                                output[j++] = 0x80 | (codepoint & 0x3F);
                            }
                            i += 4; // Move index past the 4 hex digits.
                            break;  // We're done with this escape sequence.
                        }
                    }
                    // If not a valid \uXXXX sequence, fall through to default.
                }
                default:
                    // Unrecognized escape sequence, just copy the character
                    output[j++] = input[i];
                    break;
            }
            i++;
        } else {
            output[j++] = input[i++];
        }
    }
    output[j] = '\0'; // Null terminate for safety, though it may contain other nulls.

    if (out_len) {
        *out_len = j;
    }
    return output;
}

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

void print_warning(const char *format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, ANSI_BOLD_RED "[WARNING] " ANSI_RESET);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

void print_hint(const char* format, ...) {
    va_list args;
    va_start(args, format);
    fprintf(stderr, ANSI_YELLOW "[HINT] " ANSI_RESET);
    vfprintf(stderr, format, args);
    va_end(args);
    fprintf(stderr, "\n");
}

static int min3(int a, int b, int c) {
    int m = a;
    if (b < m) m = b;
    if (c < m) m = c;
    return m;
}

int levenshtein_distance(const char *s1, const char *s2) {
    if (!s1) return s2 ? strlen(s2) : 0;
    if (!s2) return s1 ? strlen(s1) : 0;

    int len1 = strlen(s1);
    int len2 = strlen(s2);

    // Use two rows to optimize space from O(n*m) to O(m)
    int *v0 = malloc((len2 + 1) * sizeof(int));
    int *v1 = malloc((len2 + 1) * sizeof(int));
    if (!v0 || !v1) {
        if (v0) free(v0);
        if (v1) free(v1);
        return INT_MAX; // Indicate error
    }

    for (int i = 0; i <= len2; i++) {
        v0[i] = i;
    }

    for (int i = 0; i < len1; i++) {
        v1[0] = i + 1;
        for (int j = 0; j < len2; j++) {
            int cost = (s1[i] == s2[j]) ? 0 : 1;
            v1[j + 1] = min3(v1[j] + 1, v0[j + 1] + 1, v0[j] + cost);
        }
        // Copy v1 to v0 for the next iteration
        int *temp = v0;
        v0 = v1;
        v1 = temp;
    }

    int distance = v0[len2];
    free(v0);
    free(v1);
    return distance;
}

// extracts the base type from a c array/pointer type string.
// e.g., "const lv_coord_t*" -> "lv_coord_t"
// e.g., "char **" -> "char*"
// the caller must free the returned string.
char* get_array_base_type(const char* array_c_type) {
    if (!array_c_type) return strdup("unknown");

    char* type_copy = strdup(array_c_type);
    
    // Find the last '*' or '['
    char* last_ptr = strrchr(type_copy, '*');
    char* last_bracket = strrchr(type_copy, '[');
    char* split_at = (last_ptr > last_bracket) ? last_ptr : last_bracket;
    
    if (split_at) {
        *split_at = '\0'; // Terminate the string at the pointer/array symbol
    }
    
    // Trim trailing whitespace
    char* end = type_copy + strlen(type_copy) - 1;
    while (end >= type_copy && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }

    // Trim leading "const"
    char* start = type_copy;
    if (strncmp(start, "const ", 6) == 0) {
        start += 6;
    }
    
    char* result = strdup(start);
    free(type_copy);
    return result;
}


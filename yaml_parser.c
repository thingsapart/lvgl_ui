#include "yaml_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <string.h>
#include "utils.h"

#define YAML_PARSER_MAX_DEPTH 64
#define YAML_PARSER_MAX_LINE_LEN 1024
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_RESET   "\x1b[0m"
#define ANSI_BOLD          "\x1b[1m"

// --- Data Structures ---

typedef struct {
    cJSON *node;
    int indent;
} ParserStackFrame;

typedef struct {
    const char *ptr;
    int line_num;
    char **error;
    ParserStackFrame stack[YAML_PARSER_MAX_DEPTH];
    int stack_top;
    char *lines[8192]; // Max lines
    int num_lines;
    char *original_content_buffer; // To hold the duplicated string for lines
    int current_line_idx; // For multiline flow parsing
} ParserState;

// --- Forward Declarations ---
static cJSON *parse_flow_collection(ParserState *state);
static cJSON *parse_value(ParserState *state, bool is_flow_context);
static cJSON *parse_scalar(ParserState *state, bool is_flow_context);
static void parse_line(ParserState *state, int line_idx);
static void set_error(ParserState *state, const char* token_start, size_t token_len, const char *format, ...);
static int get_next_content_line_info(ParserState *state, int current_line_idx, int* out_indent, char* out_first_char);
static char* find_unquoted_toplevel_char(const char* s, char c);


// --- cJSON Helper ---
static void cjson_add_item_to_object_with_duplicates(cJSON *object, const char *string, cJSON *item) {
    if (!object || !string || !item || !cJSON_IsObject(object)) return;
    // cJSON_AddItemToObject itself just appends to the linked list of children.
    // It does not check for or replace existing items with the same key.
    // This allows for duplicate keys, which can be accessed by iterating
    // through the `child` linked list. cJSON_Print will also render all of them.
    cJSON_AddItemToObject(object, string, item);
}


// --- String & Line Helpers ---

// Finds the first occurrence of `c` in `s` that is not inside a matching pair of single or double quotes.
static char* find_unquoted_toplevel_char(const char* s, char c) {
    char quote = 0;
    while (*s) {
        if (quote == *s) {
            quote = 0; // End quote
        } else if (quote == 0) {
            if (*s == c) {
                return (char*)s; // Found target char
            }
            if (*s == '\'' || *s == '"') {
                quote = *s; // Start quote
            }
        }
        s++;
    }
    return NULL;
}

// Strips an inline YAML comment (` # ...`) from the end of a string.
// It is quote-aware and will not strip a '#' from inside a quoted string.
static void strip_trailing_comment(char* str) {
    if (!str) return;
    char* p = str;
    char quote_char = 0;
    while (*p) {
        if (quote_char) { // We are inside a quote
            if (*p == '\\') { // Handle escape sequence
                p++;
            } else if (*p == quote_char) {
                quote_char = 0; // End of quote
            }
        } else { // We are not inside a quote
            if (*p == '\'' || *p == '"') {
                quote_char = *p; // Start of quote
            } else if (*p == '#') {
                // A comment is a '#' preceded by a space.
                if (p > str && isspace((unsigned char)*(p-1))) {
                    *p = '\0'; // Terminate string here
                    return;
                }
            }
        }
        if (*p) p++; else break;
    }
}


static int get_indent(const char *line) {
    if (!line) return 0;
    int indent = 0;
    while (line[indent] == ' ') indent++;
    return indent;
}

static void split_lines(ParserState *state, const char *yaml_content) {
    state->original_content_buffer = strdup(yaml_content);
    if (!state->original_content_buffer) return;

    char *p = state->original_content_buffer;

    while (*p) {
        if (state->num_lines >= 8192) {
            set_error(state, NULL, 0, "Exceeded maximum number of lines (8192)");
            return;
        }
        char* start = p;
        while (*p && *p != '\n' && *p != '\r') p++;
        size_t len = p - start;
        state->lines[state->num_lines] = malloc(len + 1);
        memcpy(state->lines[state->num_lines], start, len);
        state->lines[state->num_lines][len] = '\0';
        state->num_lines++;

        if (*p == '\r' && *(p+1) == '\n') p += 2;
        else if (*p == '\n' || *p == '\r') p++;
    }
}

// Helper to find the indent and first character of the next meaningful line
static int get_next_content_line_info(ParserState *state, int current_line_idx, int* out_indent, char* out_first_char) {
    for (int j = current_line_idx + 1; j < state->num_lines; j++) {
        char* next_line = state->lines[j];
        int indent = get_indent(next_line);
        char* content = trim_whitespace(next_line + indent);
        if (*content != '\0' && *content != '#') {
            if(out_indent) *out_indent = indent;
            if(out_first_char) *out_first_char = *content;
            return j;
        }
    }
    if(out_indent) *out_indent = -1;
    if(out_first_char) *out_first_char = '\0';
    return -1;
}

// --- Recursive-Descent Value Parsers ---

static void skip_whitespace(ParserState *state) {
    while (true) {
        while (*state->ptr && isspace((unsigned char)*state->ptr)) {
            state->ptr++;
        }
        if (*state->ptr == '\0' && state->current_line_idx + 1 < state->num_lines) {
             state->current_line_idx++;
             state->line_num = state->current_line_idx + 1;
             state->ptr = state->lines[state->current_line_idx];
             continue;
        }
        break;
    }
}


static cJSON *parse_scalar(ParserState *state, bool is_flow_context) {
    char buffer[YAML_PARSER_MAX_LINE_LEN];
    char* write_ptr = buffer;
    const char* p = state->ptr;
    char quote = 0;
    bool had_quote = false;

    if (*p == '"' || *p == '\'') {
        quote = *p;
        p++;
        had_quote = true;
    }

    while (write_ptr < buffer + sizeof(buffer) - 1) {
        if (had_quote) {
            if (!*p) { break; } // Unterminated string
            if (*p == quote) {
                if (quote == '\'' && *(p + 1) == '\'') {
                    *write_ptr++ = '\'';
                    p += 2;
                    continue;
                }
                p++; // Consume closing quote
                break;
            }
            if (*p == '\\' && quote == '"') {
                p++;
                switch (*p) {
                    case 'n': *write_ptr++ = '\n'; break;
                    case 'r': *write_ptr++ = '\r'; break;
                    case 't': *write_ptr++ = '\t'; break;
                    case 'b': *write_ptr++ = '\b'; break;
                    case 'f': *write_ptr++ = '\f'; break;
                    case '"': *write_ptr++ = '"'; break;
                    case '\\': *write_ptr++ = '\\'; break;
                    default: *write_ptr++ = *p; break;
                }
                p++;
            } else {
                *write_ptr++ = *p++;
            }
        } else { // Unquoted
            if (is_flow_context) {
                // In flow context, we must stop at syntax characters
                if (!*p || strchr(",[]{}", *p) || (*p == ':' && (isspace((unsigned char)*(p + 1)) || *(p + 1) == '\0')) || (*p == '#' && p > state->ptr && isspace((unsigned char)*(p-1)))) {
                    break;
                }
            } else {
                // In block context, the value is the rest of the line.
                // Comments should have been stripped by the caller (parse_line).
                // We only need to stop at the end of the string.
                if (!*p) {
                    break;
                }
            }
            *write_ptr++ = *p++;
        }
    }
    *write_ptr = '\0';
    state->ptr = p;

    char* final_val_ptr = buffer;
    if (!had_quote) {
        final_val_ptr = trim_whitespace(buffer);
    }

    cJSON* node = NULL;
    if (!had_quote && *final_val_ptr != '\0') {
        if (strcmp(final_val_ptr, "null") == 0) node = cJSON_CreateNull();
        else if (strcmp(final_val_ptr, "true") == 0) node = cJSON_CreateTrue();
        else if (strcmp(final_val_ptr, "false") == 0) node = cJSON_CreateFalse();
        else {
            char* endptr;
            double num = strtod(final_val_ptr, &endptr);
            if (*endptr == '\0' && endptr != final_val_ptr) {
                node = cJSON_CreateNumber(num);
            }
        }
    }

    if (!node) {
        node = cJSON_CreateString(final_val_ptr);
    }
    return node;
}

static cJSON* parse_value(ParserState *state, bool is_flow_context) {
    skip_whitespace(state);
    if (*state->ptr == '[' || *state->ptr == '{') {
        return parse_flow_collection(state);
    }
    return parse_scalar(state, is_flow_context);
}

static cJSON *parse_flow_collection(ParserState *state) {
    char start_char = *state->ptr;
    if (start_char != '{' && start_char != '[') return NULL;

    char end_char = (start_char == '[') ? ']' : '}';
    cJSON *root = (start_char == '[') ? cJSON_CreateArray() : cJSON_CreateObject();

    state->ptr++; // Consume '[' or '{'

    while (true) {
        // Robustly skip all whitespace, newlines, and comments across multiple lines
        while (true) {
            if (*state->ptr == '\0') { // End of a line buffer
                if (state->current_line_idx + 1 >= state->num_lines) {
                    goto end_of_input;
                }
                state->current_line_idx++;
                state->line_num = state->current_line_idx + 1;
                state->ptr = state->lines[state->current_line_idx];
                continue;
            }
            if (isspace((unsigned char)*state->ptr)) {
                state->ptr++;
            } else if (*state->ptr == '#') {
                while (*state->ptr) state->ptr++; // Skip to end of line buffer
            } else {
                break; // Found a meaningful token
            }
        }

    end_of_input:
        if (*state->ptr == '\0') {
            break; // Reached end of input before finding closing character
        }

        if (*state->ptr == end_char) {
            state->ptr++; // Consume ']' or '}'
            return root;
        }

        // Handle trailing comma before end char: [ a, b, ]
        if (cJSON_GetArraySize(root) > 0 || (cJSON_IsObject(root) && root->child != NULL)) {
            skip_whitespace(state);
            if (*state->ptr == ',') {
                state->ptr++;
                skip_whitespace(state);
                if (*state->ptr == end_char) { // Trailing comma
                    state->ptr++;
                    return root;
                }
            }
        }

        if (cJSON_IsArray(root)) {
            cJSON_AddItemToArray(root, parse_value(state, true));
        } else { // It's an object
            const char* key_start_ptr = state->ptr;
            cJSON* key_node = parse_value(state, true);

            char key_string[256];
            if (cJSON_IsString(key_node)) {
                strncpy(key_string, key_node->valuestring, sizeof(key_string) - 1);
                key_string[sizeof(key_string) - 1] = '\0';
            } else if (cJSON_IsNumber(key_node)) {
                snprintf(key_string, sizeof(key_string), "%g", key_node->valuedouble);
            } else if (cJSON_IsTrue(key_node)) {
                strncpy(key_string, "true", sizeof(key_string));
            } else if (cJSON_IsFalse(key_node)) {
                strncpy(key_string, "false", sizeof(key_string));
            } else if (cJSON_IsNull(key_node)) {
                strncpy(key_string, "null", sizeof(key_string));
            } else {
                set_error(state, key_start_ptr, (state->ptr - key_start_ptr), "Invalid key in flow-style map");
                cJSON_Delete(key_node); cJSON_Delete(root); return cJSON_CreateNull();
            }
            cJSON_Delete(key_node);

            skip_whitespace(state);
            if (*state->ptr != ':') {
                set_error(state, state->ptr, 1, "Expected ':' in flow-style map");
                cJSON_Delete(root); return cJSON_CreateNull();
            }
            state->ptr++;

            cJSON* value_node = parse_value(state, true);
            cjson_add_item_to_object_with_duplicates(root, key_string, value_node);
        }

        // After an item, expect a comma or the end character
        skip_whitespace(state);
        if (*state->ptr == ',') {
            state->ptr++;
        } else if (*state->ptr != end_char && *state->ptr != '#' && *state->ptr != '\0') {
            set_error(state, state->ptr, 1, "Expected ',' or '%c' in flow collection", end_char);
            cJSON_Delete(root); return cJSON_CreateNull();
        }
    }

    set_error(state, state->ptr, 1, "Unterminated flow collection, missing '%c'", end_char);
    cJSON_Delete(root);
    return cJSON_CreateNull();
}


// --- Parser State Management ---

static void push_stack(ParserState *state, cJSON *node, int indent) {
    if (state->stack_top >= YAML_PARSER_MAX_DEPTH - 1) {
        set_error(state, NULL, 0, "Exceeded maximum YAML depth of %d", YAML_PARSER_MAX_DEPTH);
        return;
    }
    state->stack_top++;
    state->stack[state->stack_top].node = node;
    state->stack[state->stack_top].indent = indent;
}

static void pop_stack(ParserState *state) {
    if (state->stack_top > 0) {
        state->stack_top--;
    }
}

static void set_error(ParserState *state, const char* token_start, size_t token_len, const char *format, ...) {
    if (!state || !state->error || *state->error) return;

    char message_buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message_buffer, sizeof(message_buffer), format, args);
    va_end(args);

    char context_buffer[YAML_PARSER_MAX_LINE_LEN * 2 + 256] = {0};
    if (state->line_num > 0 && state->line_num <= state->num_lines) {
        const char* line_start = state->lines[state->line_num - 1];
        int col = (token_start) ? (token_start - line_start) : 0;
        size_t len = (token_len > 0) ? token_len : strlen(line_start) - col;
        if (len > YAML_PARSER_MAX_LINE_LEN) len = YAML_PARSER_MAX_LINE_LEN -1;


        if (col < 0 || col >= YAML_PARSER_MAX_LINE_LEN) col = 0;

        char underline[YAML_PARSER_MAX_LINE_LEN] = {0};
        memset(underline, ' ', col);
        memset(underline + col, '^', len);

        snprintf(context_buffer, sizeof(context_buffer),
                 "\n\n    ---> Error context (Line %d, Col %d):\n"
                 "%4d | %s\n"
                 "     | %s%s%s%s\n",
                 state->line_num, col + 1,
                 state->line_num, line_start,
                 ANSI_BOLD, ANSI_COLOR_RED, underline, ANSI_COLOR_RESET
                );
    }

    size_t total_len = strlen(message_buffer) + strlen(context_buffer) + 64;
    char *err_str = malloc(total_len);
    if (err_str) {
        snprintf(err_str, total_len, "YAML Parse Error: %s%s", message_buffer, context_buffer);
        *state->error = err_str;
    }
}

// --- Main Parsing Logic ---

void parse_line(ParserState *state, int line_idx) {
    state->line_num = line_idx + 1;
    state->current_line_idx = line_idx;
    char* line = state->lines[line_idx];

    int indent = get_indent(line);
    char* content_start = line + indent;
    char* p = trim_whitespace(content_start);

    if (*p == '\0' || *p == '#') return;

    // Adjust stack based on indentation.
    while (state->stack_top > 0 && indent <= state->stack[state->stack_top].indent) {
        pop_stack(state);
    }

    cJSON *current_parent = state->stack[state->stack_top].node;

    // --- Handle List Item ---
    if (*p == '-') {
        if (!cJSON_IsArray(current_parent)) {
            set_error(state, p, 1, "List item '-' found in a non-array context.");
            return;
        }

        cJSON *list_context = current_parent;
        // Iteratively handle nested lists like `- - - item`
        while (*p == '-') {
            char* next_char = trim_whitespace(p + 1);
            if (*next_char == '-') {
                cJSON* new_list = cJSON_CreateArray();
                cJSON_AddItemToArray(list_context, new_list);
                list_context = new_list;
                // THIS IS THE FIX: The new list is a new scope for subsequent lines.
                push_stack(state, list_context, indent);
                p = next_char;
                continue;
            }
            p = next_char;
            break;
        }

        state->ptr = p;

        if (*p == '\0') { // Line ends after `-` or `- -` etc.
            int next_indent = -1;
            char next_first_char = '\0';
            get_next_content_line_info(state, line_idx, &next_indent, &next_first_char);
            if (next_indent > indent) {
                 cJSON* container = (next_first_char == '-') ? cJSON_CreateArray() : cJSON_CreateObject();
                 cJSON_AddItemToArray(list_context, container);
                 push_stack(state, container, indent);
            } else {
                // This case handles a list item that is just " - " and is a sibling
                // to the previous item, implying an empty map should be added.
                // We must check that we are not creating an empty map for a `-`
                // that is *less* indented than the current context, which would be
                // a syntax error handled elsewhere. This is for `- \n  - key:val`
                if (state->stack_top > 0 && indent == state->stack[state->stack_top].indent) {
                     cJSON_AddItemToArray(list_context, cJSON_CreateObject());
                }
            }
            return;
        }

        char* colon = find_unquoted_toplevel_char(p, ':');
        char* bracket = strpbrk(p, "[{"); // No need to make this quote-aware
        if (colon && (!bracket || colon < bracket)) {
            cJSON* new_map_item = cJSON_CreateObject();
            cJSON_AddItemToArray(list_context, new_map_item);
            push_stack(state, new_map_item, indent);
        } else {
            char* val_str = p;
            strip_trailing_comment(val_str);
            state->ptr = val_str;
            cJSON* val_node = parse_value(state, false);
            cJSON_AddItemToArray(list_context, val_node);
            return;
        }
    }

    // --- Handle Mapping ---
    current_parent = state->stack[state->stack_top].node;
    if (!cJSON_IsObject(current_parent)) {
        if (*p != '\0') set_error(state, p, strlen(p), "Invalid mapping (not in an object context)");
        return;
    }

    char* colon = find_unquoted_toplevel_char(p, ':');
    if (!colon) {
        set_error(state, p, strlen(p), "Invalid mapping (missing ':')");
        return;
    }

    *colon = '\0';
    char* key = trim_whitespace(p);
    // Unquote the key if it's quoted
    if ((*key == '"' || *key == '\'') && key[strlen(key)-1] == *key) {
        key[strlen(key)-1] = '\0';
        key++;
    }


    char* val_str = colon + 1;
    strip_trailing_comment(val_str);
    val_str = trim_whitespace(val_str);

    if (*val_str == '\0') {
        int next_indent = -1;
        char next_first_char = '\0';
        int next_line_idx = get_next_content_line_info(state, line_idx, &next_indent, &next_first_char);

        if (next_line_idx != -1 && next_indent > indent) {
            if (next_first_char == '[' || next_first_char == '{') {
                 state->current_line_idx = next_line_idx;
                 state->ptr = trim_whitespace(state->lines[state->current_line_idx]);
                 cJSON* val_node = parse_value(state, false);
                 cjson_add_item_to_object_with_duplicates(current_parent, key, val_node);
            } else {
                cJSON* container = (next_first_char == '-') ? cJSON_CreateArray() : cJSON_CreateObject();
                cjson_add_item_to_object_with_duplicates(current_parent, key, container);
                push_stack(state, container, indent);
            }
        } else {
            cjson_add_item_to_object_with_duplicates(current_parent, key, cJSON_CreateNull());
        }
    } else {
        state->ptr = val_str;
        cJSON* val_node = parse_value(state, false);
        cjson_add_item_to_object_with_duplicates(current_parent, key, val_node);
    }
}

// --- Public API ---

cJSON* yaml_to_cjson(const char* yaml_content, char** error_message) {
    if (!yaml_content) {
        if (error_message) *error_message = strdup("Input YAML content is NULL.");
        return NULL;
    }

    ParserState state = {0};
    state.error = error_message;
    if (error_message) *error_message = NULL;
    state.stack_top = -1;

    split_lines(&state, yaml_content);
    if (state.error && *state.error) {
        if(state.original_content_buffer) free(state.original_content_buffer);
        for(int i=0; i<state.num_lines; i++) if(state.lines[i]) free(state.lines[i]);
        return NULL;
    }

    cJSON *root = NULL;
    int first_content_line_idx = -1;
    char first_char = '\0';
    char* first_line_content = NULL;

    for (int i = 0; i < state.num_lines; i++) {
        char* content = trim_whitespace(state.lines[i]);
        if (*content != '\0' && *content != '#') {
            first_content_line_idx = i;
            first_char = *content;
            first_line_content = content;
            break;
        }
    }

    if (first_content_line_idx == -1) {
        root = cJSON_CreateArray();
    } else {
        state.ptr = first_line_content;
        state.current_line_idx = first_content_line_idx;
        state.line_num = first_content_line_idx + 1;


        // If the document starts with a flow collection, parse it directly.
        if (first_char == '{' || first_char == '[') {
            root = parse_value(&state, true);
        } else if (find_unquoted_toplevel_char(state.ptr, ':') == NULL && first_char != '-') {
            // No colon on the line and not a list item: must be a root scalar.
             root = parse_value(&state, false);
        } else {
            // Otherwise, start the line-by-line block parser.
            root = (first_char == '-') ? cJSON_CreateArray() : cJSON_CreateObject();
            push_stack(&state, root, -1);
            for (int i = first_content_line_idx; i < state.num_lines; i++) {
                parse_line(&state, i);
                if (state.error && *state.error) break;
                i = state.current_line_idx;
            }
        }
    }

    if (state.error && *state.error) {
        cJSON_Delete(root);
        root = NULL;
    } else if (root && !cJSON_IsArray(root) && !cJSON_IsObject(root)) {
        // Wrap root scalar in an array
        cJSON* array_wrapper = cJSON_CreateArray();
        cJSON_AddItemToArray(array_wrapper, root);
        root = array_wrapper;
    } else if (root && cJSON_IsObject(root)) {
        // Wrap root object in an array
        cJSON* array_wrapper = cJSON_CreateArray();
        cJSON_AddItemToArray(array_wrapper, root);
        root = array_wrapper;
    }
    else if (!root) {
        root = cJSON_CreateArray();
    }


    if(state.original_content_buffer) free(state.original_content_buffer);
    for(int i=0; i<state.num_lines; i++) if(state.lines[i]) free(state.lines[i]);

    return root;
}

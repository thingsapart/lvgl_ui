#include "yaml_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>

#define YAML_PARSER_MAX_DEPTH 64
#define YAML_PARSER_MAX_LINE_LEN 1024

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
} ParserState;

// --- Forward Declarations ---
static cJSON *parse_value_string(const char *str);
static void parse_line(ParserState *state, int line_idx);
static void set_error(ParserState *state, const char *format, ...);

// --- cJSON Helper ---

// Adds an item to a cJSON object, but unlike the standard cJSON_AddItemToObject,
// it does not check for or replace existing keys. It simply appends the new
// item to the object's child linked list, allowing for duplicate keys.
static void cjson_add_item_to_object_with_duplicates(cJSON *object, const char *string, cJSON *item) {
    if (!object || !string || !item || !cJSON_IsObject(object)) return;

    if (item->string) free(item->string);
    item->string = strdup(string);

    cJSON *child = object->child;
    if (!child) {
        object->child = item;
    } else {
        while (child->next) {
            child = child->next;
        }
        child->next = item;
        item->prev = child;
    }
}


// --- String & Line Helpers ---

static char* trim_whitespace(char *str) {
    char *end;
    while (isspace((unsigned char)*str)) str++;
    if (*str == 0) return str;
    end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) end--;
    *(end + 1) = 0;
    return str;
}

static int get_indent(const char *line) {
    int indent = 0;
    while (line[indent] == ' ') {
        indent++;
    }
    return indent;
}

static void split_lines(ParserState *state, const char *yaml_content) {
    char *buffer = strdup(yaml_content);
    char *p = buffer;
    while ((state->lines[state->num_lines] = strtok(p, "\n\r")) != NULL) {
        p = NULL;
        state->num_lines++;
        if (state->num_lines >= 8192) {
            set_error(state, "Exceeded maximum number of lines (8192)");
            break;
        }
    }
    // We don't free buffer here because strtok holds pointers into it.
    // It will be freed with the state at the end.
}

// --- Value Parser ---

// Very simple flow collection parser. Not fully YAML compliant, but handles the examples.
static cJSON *parse_flow_collection(const char **p_str) {
    const char *ptr = *p_str;
    char start_char = *ptr;
    char end_char = (start_char == '[') ? ']' : '}';
    cJSON *root = (start_char == '[') ? cJSON_CreateArray() : cJSON_CreateObject();

    ptr++; // Skip '[' or '{'
    *p_str = ptr; // Update position

    while (*ptr && *ptr != end_char) {
        while (isspace((unsigned char)*ptr)) ptr++;
        if (*ptr == end_char || *ptr == '\0') break;

        const char *item_start = ptr;
        int collection_level = 0;
        int quote_level = 0;

        // Find end of current item (handling nested collections)
        while (*ptr) {
            if (quote_level == 0) {
                 if (*ptr == '[' || *ptr == '{') collection_level++;
                 if (*ptr == ']' || *ptr == '}') collection_level--;
            }
            if (*ptr == '"') quote_level = !quote_level;

            if (collection_level < 0) break; // End of our collection
            if (collection_level == 0 && (*ptr == ',' || *ptr == end_char)) break;
            ptr++;
        }
        
        char *item_str = strndup(item_start, ptr - item_start);
        char *trimmed_item = trim_whitespace(item_str);

        if (cJSON_IsObject(root)) {
            char *colon = strchr(trimmed_item, ':');
            if (colon) {
                *colon = '\0';
                char *key = trim_whitespace(trimmed_item);
                char *val_str = trim_whitespace(colon + 1);
                cJSON *val_node = parse_value_string(val_str);
                cjson_add_item_to_object_with_duplicates(root, key, val_node);
            }
        } else { // Array
             cJSON_AddItemToArray(root, parse_value_string(trimmed_item));
        }

        free(item_str);

        if (*ptr == ',') ptr++;
    }

    if (*ptr == end_char) ptr++;
    *p_str = ptr;
    return root;
}


static cJSON* parse_value_string(const char *str) {
    char* value_str = trim_whitespace(strdup(str));
    if (!value_str) return cJSON_CreateNull();
    if (!*value_str) {
        free(value_str);
        return cJSON_CreateNull();
    }
    
    char* end = value_str + strlen(value_str) - 1;

    // 1. Handle flow collections recursively
    if (*value_str == '[' && *end == ']') {
        cJSON* node = parse_flow_collection(&str); 
        free(value_str);
        return node;
    }
    if (*value_str == '{' && *end == '}') {
        cJSON* node = parse_flow_collection(&str);
        free(value_str);
        return node;
    }

    // 2. Handle specific keywords
    if (strcmp(value_str, "null") == 0) { free(value_str); return cJSON_CreateNull(); }
    if (strcmp(value_str, "true") == 0) { free(value_str); return cJSON_CreateTrue(); }
    if (strcmp(value_str, "false") == 0) { free(value_str); return cJSON_CreateFalse(); }

    // 3. Handle quoted strings
    if ((*value_str == '\'' && *end == '\'') || (*value_str == '"' && *end == '"')) {
        *end = '\0';
        cJSON* item = cJSON_CreateString(value_str + 1);
        free(value_str);
        return item;
    }

    // 4. Try to parse as a number. Only if the ENTIRE string is a number.
    char* endptr;
    double num = strtod(value_str, &endptr);
    if (*endptr == '\0' && endptr != value_str) {
        free(value_str);
        return cJSON_CreateNumber(num);
    }
    
    // 5. If all else fails, it's an unquoted string.
    // This will correctly handle "LV_ALIGN_CENTER", "#FF0000", "50%", etc.
    cJSON* item = cJSON_CreateString(value_str);
    free(value_str);
    return item;
}

// --- Parser State Management ---

static void push_stack(ParserState *state, cJSON *node, int indent) {
    if (state->stack_top >= YAML_PARSER_MAX_DEPTH - 1) {
        set_error(state, "Exceeded maximum YAML depth of %d", YAML_PARSER_MAX_DEPTH);
        return;
    }
    state->stack_top++;
    state->stack[state->stack_top].node = node;
    state->stack[state->stack_top].indent = indent;
}

static void pop_stack(ParserState *state) {
    if (state->stack_top > -1) {
        state->stack_top--;
    }
}

static void set_error(ParserState *state, const char *format, ...) {
    if (state->error && *state->error) return; // Don't overwrite first error
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    char *err_str = malloc(strlen(buffer) + 32);
    sprintf(err_str, "YAML Error on line %d: %s", state->line_num, buffer);
    *state->error = err_str;
}

// --- Main Parsing Logic ---

void parse_line(ParserState *state, int line_idx) {
    state->line_num = line_idx + 1;
    char original_line[YAML_PARSER_MAX_LINE_LEN];
    strncpy(original_line, state->lines[line_idx], YAML_PARSER_MAX_LINE_LEN - 1);
    original_line[YAML_PARSER_MAX_LINE_LEN - 1] = '\0';
    
    int indent = get_indent(original_line);
    char* content = trim_whitespace(original_line + indent);
    if (*content == '\0' || *content == '#') {
        return; // Skip empty or comment-only lines
    }
    
    while (state->stack_top > 0 && indent <= state->stack[state->stack_top].indent) {
        pop_stack(state);
    }

    cJSON *parent = state->stack[state->stack_top].node;

    // This block handles both list items (`- key: val`) and regular items (`  key: val`).
    // It's responsible for splitting the key from the value+comment part.
    char *item_content = content;
    if (*content == '-') {
        if (!cJSON_IsArray(parent)) {
            set_error(state, "Found list item '-' in a non-array context.");
            return;
        }

        cJSON *new_obj = cJSON_CreateObject();
        cJSON_AddItemToArray(parent, new_obj);
        push_stack(state, new_obj, indent);
        parent = new_obj; // The new parent for this line's content is the new object.
        item_content = trim_whitespace(content + 1);
        
        // If the line was only "-", there's nothing more to process.
        if (*item_content == '\0') {
            return;
        }

    } else {
        if (!cJSON_IsObject(parent)) {
            set_error(state, "Found key-value pair in a non-object context.");
            return;
        }
    }

    // Now, `item_content` holds the `key: value # comment` part, and `parent` is correct.
    char *colon = strchr(item_content, ':');
    if (!colon) {
        set_error(state, "Invalid item (missing ':'). Content: '%s'", item_content);
        return;
    }
    *colon = '\0';
    char *key = trim_whitespace(item_content);
    char *val_str = colon + 1;

    // ** CORRECTED COMMENT HANDLING FOR INLINE VALUES **
    bool in_single_quotes = false;
    bool in_double_quotes = false;
    char *p = val_str;
    while(*p) {
        if (*p == '\'' && !in_double_quotes) in_single_quotes = !in_single_quotes;
        else if (*p == '"' && !in_single_quotes) in_double_quotes = !in_double_quotes;
        else if (*p == '#' && !in_single_quotes && !in_double_quotes) {
            *p = '\0'; // Truncate the string at the unquoted comment
            break;
        }
        p++;
    }
    char *trimmed_val_str = trim_whitespace(val_str);

    if (*trimmed_val_str == '\0') {
        // --- Smart Lookahead for Block Content ---
        int next_content_indent = -1;
        char* next_content_start = NULL;
        for (int j = line_idx + 1; j < state->num_lines; j++) {
            char temp_line[YAML_PARSER_MAX_LINE_LEN];
            strncpy(temp_line, state->lines[j], YAML_PARSER_MAX_LINE_LEN-1);
            temp_line[YAML_PARSER_MAX_LINE_LEN-1] = '\0';

            int current_indent = get_indent(temp_line);
            char* current_content = trim_whitespace(temp_line + current_indent);
            
            if (*current_content == '\0' || *current_content == '#') {
                continue; // Skip empty/comment line
            }
            next_content_indent = current_indent;
            next_content_start = current_content;
            break;
        }

        if (next_content_indent > indent) {
            bool is_list = (next_content_start && *next_content_start == '-');
            if (is_list) {
                cJSON *arr = cJSON_CreateArray();
                cjson_add_item_to_object_with_duplicates(parent, key, arr);
                push_stack(state, arr, indent);
            } else {
                cJSON *obj = cJSON_CreateObject();
                cjson_add_item_to_object_with_duplicates(parent, key, obj);
                push_stack(state, obj, indent);
            }
        } else {
            cjson_add_item_to_object_with_duplicates(parent, key, cJSON_CreateNull());
        }
    } else {
        cJSON *val_node = parse_value_string(trimmed_val_str);
        cjson_add_item_to_object_with_duplicates(parent, key, val_node);
    }
}


// --- Public API ---

cJSON* yaml_to_cjson(const char* yaml_content, char** error_message) {
    if (!yaml_content) {
        *error_message = strdup("Input YAML content is NULL.");
        return NULL;
    }
    
    ParserState state = {0};
    state.error = error_message;
    *error_message = NULL;
    state.stack_top = -1;

    char* content_copy = strdup(yaml_content);
    split_lines(&state, content_copy);

    cJSON *root = cJSON_CreateArray();
    push_stack(&state, root, -1);
    
    for (int i = 0; i < state.num_lines; i++) {
        parse_line(&state, i);
        if (*state.error) break;
    }

    free(content_copy); // Frees the buffer used by strtok

    if (*state.error) {
        cJSON_Delete(root);
        return NULL;
    }

    return root;
}

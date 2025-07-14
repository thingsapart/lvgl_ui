#include "yaml_parser.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include "utils.h"

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
static cJSON *parse_value(const char **p_str, ParserState *state);
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

// This is now the main recursive parsing function. It takes a pointer-to-a-pointer
// to the string and advances it as it consumes tokens.
static cJSON* parse_value(const char **p_str, ParserState *state) {
    const char *ptr = *p_str;
    while(isspace((unsigned char)*ptr)) ptr++;

    // 1. Handle flow collections recursively
    if (*ptr == '[' || *ptr == '{') {
        char start_char = *ptr;
        char end_char = (start_char == '[') ? ']' : '}';
        cJSON *root = (start_char == '[') ? cJSON_CreateArray() : cJSON_CreateObject();
        
        ptr++; // Skip '[' or '{'

        while (*ptr) {
            while (isspace((unsigned char)*ptr)) ptr++;
            if (*ptr == end_char || *ptr == '\0') break;

            if (cJSON_IsArray(root)) {
                 cJSON_AddItemToArray(root, parse_value(&ptr, state));
            } else { // Object
                cJSON* key_node = parse_value(&ptr, state);
                if (!cJSON_IsString(key_node)) {
                    set_error(state, "Invalid key in flow-style map");
                    cJSON_Delete(key_node);
                    cJSON_Delete(root);
                    return cJSON_CreateNull();
                }
                while(isspace((unsigned char)*ptr)) ptr++;
                if (*ptr != ':') {
                    set_error(state, "Expected ':' in flow-style map");
                    cJSON_Delete(key_node);
                    cJSON_Delete(root);
                    return cJSON_CreateNull();
                }
                ptr++; // Skip ':'
                cjson_add_item_to_object_with_duplicates(root, key_node->valuestring, parse_value(&ptr, state));
                cJSON_Delete(key_node);
            }

            while(isspace((unsigned char)*ptr)) ptr++;
            if (*ptr == ',') ptr++;
            else if (*ptr != end_char) {
                set_error(state, "Expected ',' or '%c' in flow collection", end_char);
                cJSON_Delete(root);
                return cJSON_CreateNull();
            }
        }
        if (*ptr == end_char) ptr++;
        *p_str = ptr;
        return root;
    }

    // 2. Handle quoted strings
    if (*ptr == '"' || *ptr == '\'') {
        char quote = *ptr;
        ptr++;
        const char* start = ptr;
        while (*ptr && *ptr != quote) {
            if (*ptr == '\\') ptr++; // Skip escaped char
            ptr++;
        }
        char* val = strndup(start, ptr - start);
        if (*ptr == quote) ptr++;
        *p_str = ptr;
        cJSON* node = cJSON_CreateString(val);
        free(val);
        return node;
    }

    // 3. Handle unquoted scalars
    const char* start = ptr;
    while (*ptr && *ptr != ',' && *ptr != ']' && *ptr != '}' && *ptr != ':') {
        ptr++;
    }
    char* val = strndup(start, ptr - start);
    char* trimmed_val = trim_whitespace(val);
    *p_str = ptr;

    if (strcmp(trimmed_val, "null") == 0) { free(val); return cJSON_CreateNull(); }
    if (strcmp(trimmed_val, "true") == 0) { free(val); return cJSON_CreateTrue(); }
    if (strcmp(trimmed_val, "false") == 0) { free(val); return cJSON_CreateFalse(); }
    
    char* endptr;
    double num = strtod(trimmed_val, &endptr);
    if (*endptr == '\0' && endptr != trimmed_val) {
        free(val);
        return cJSON_CreateNumber(num);
    }

    cJSON* node = cJSON_CreateString(trimmed_val);
    free(val);
    return node;
}


// A simple wrapper to start the recursive parsing process.
static cJSON* parse_value_string(const char *str, ParserState* state) {
    const char *p = str;
    return parse_value(&p, state);
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
    if (state && state->error && *state->error) return; // Don't overwrite first error
    char buffer[256];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);

    if (state && state->error) {
        char *err_str = malloc(strlen(buffer) + 32);
        sprintf(err_str, "YAML Error on line %d: %s", state->line_num, buffer);
        *state->error = err_str;
    }
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

    // --- CASE 1: Line is a list item ---
    if (*content == '-') {
        if (!cJSON_IsArray(parent)) {
            set_error(state, "Found list item '-' in a non-array context.");
            return;
        }

        char *item_content = trim_whitespace(content + 1);

        // Check if the item is a flow-style collection on the same line
        if (*item_content == '{' || *item_content == '[') {
            cJSON* flow_node = parse_value_string(item_content, state);
            cJSON_AddItemToArray(parent, flow_node);
            return; // This line is fully processed
        }

        // It's a block-style item. Create a new object for it.
        cJSON *new_obj = cJSON_CreateObject();
        cJSON_AddItemToArray(parent, new_obj);
        push_stack(state, new_obj, indent);
        
        // FIX: Update the local parent variable to the new object context
        parent = new_obj;

        // If there's content on the same line, process it as a k:v pair for the new object.
        if (*item_content != '\0') {
            content = item_content; // The rest of the logic will process this content.
        } else {
            return; // No content on this line, wait for the next indented line.
        }
    }
    
    // --- CASE 2: Line is a key-value pair ---
    if (!cJSON_IsObject(parent)) {
        set_error(state, "Found key-value pair in a non-object context.");
        return;
    }
    
    char *colon = strchr(content, ':');
    if (!colon) {
        set_error(state, "Invalid item (missing ':'). Content: '%s'", content);
        return;
    }
    *colon = '\0';
    char *key = trim_whitespace(content);
    char *val_str = colon + 1;

    // Remove trailing comments from the value part
    bool in_single_quotes = false;
    bool in_double_quotes = false;
    char *p = val_str;
    while(*p) {
        if (*p == '\'' && !in_double_quotes) in_single_quotes = !in_single_quotes;
        else if (*p == '"' && !in_single_quotes) in_double_quotes = !in_double_quotes;
        else if (*p == '#' && !in_single_quotes && !in_double_quotes) {
            *p = '\0';
            break;
        }
        p++;
    }
    char *trimmed_val_str = trim_whitespace(val_str);

    if (*trimmed_val_str == '\0') {
        // Value is empty. This implies a block follows. Peek ahead.
        int next_content_indent = -1;
        char first_char_of_next_content = '\0';
        for (int j = line_idx + 1; j < state->num_lines; j++) {
            // Make a copy of the line to safely trim it
            char next_line_copy[YAML_PARSER_MAX_LINE_LEN];
            strncpy(next_line_copy, state->lines[j], sizeof(next_line_copy) - 1);
            next_line_copy[sizeof(next_line_copy) - 1] = '\0';

            int current_indent = get_indent(next_line_copy);
            char* current_content = trim_whitespace(next_line_copy + current_indent);

            if (*current_content == '\0' || *current_content == '#') {
                continue; // Skip empty/comment lines
            }
            next_content_indent = current_indent;
            first_char_of_next_content = *current_content;
            break;
        }

        if (next_content_indent > indent) {
            // Indented block follows.
            bool is_list = (first_char_of_next_content == '-');
            cJSON *new_container = is_list ? cJSON_CreateArray() : cJSON_CreateObject();
            cjson_add_item_to_object_with_duplicates(parent, key, new_container);
            push_stack(state, new_container, indent);
        } else {
            // No indented block, it's just a key with a null value.
            cjson_add_item_to_object_with_duplicates(parent, key, cJSON_CreateNull());
        }
    } else {
        // Value is on the same line.
        cJSON *val_node = parse_value_string(trimmed_val_str, state);
        cjson_add_item_to_object_with_duplicates(parent, key, val_node);
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

    char* content_copy = strdup(yaml_content);
    if (!content_copy) {
         if (error_message) *error_message = strdup("Failed to allocate memory for YAML content copy.");
         return NULL;
    }
    split_lines(&state, content_copy);

    // Find the first non-empty, non-comment line to determine the root type.
    int first_content_line_idx = -1;
    bool root_is_sequence = false;
    cJSON *root = NULL;

    for (int i = 0; i < state.num_lines; i++) {
        char temp_line[YAML_PARSER_MAX_LINE_LEN];
        strncpy(temp_line, state.lines[i], sizeof(temp_line) - 1);
        temp_line[sizeof(temp_line) - 1] = '\0';
        char* content = trim_whitespace(temp_line);
        if (*content == '\0' || *content == '#') continue;

        first_content_line_idx = i;
        if (*content == '-') {
            root_is_sequence = true;
        }
        break;
    }

    if (first_content_line_idx == -1) { // Empty file
        free(content_copy);
        return cJSON_CreateArray(); // Return empty array
    }

    if (root_is_sequence) {
        root = cJSON_CreateArray();
        push_stack(&state, root, -1);
    } else {
        root = cJSON_CreateObject();
        push_stack(&state, root, -1);
    }

    for (int i = first_content_line_idx; i < state.num_lines; i++) {
        parse_line(&state, i);
        if (state.error && *state.error) break;
    }

    free(content_copy);

    if (state.error && *state.error) {
        cJSON_Delete(root);
        return NULL;
    }

    // If the root was a mapping, wrap it in an array to satisfy the generator's contract.
    if (!root_is_sequence && root) {
        cJSON* array_wrapper = cJSON_CreateArray();
        cJSON_AddItemToArray(array_wrapper, root);
        return array_wrapper;
    }

    return root;
}

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
} ParserState;

// --- Forward Declarations ---
static cJSON *parse_value(const char **p_str, ParserState *state);
static void parse_line(ParserState *state, int line_idx);
static void set_error(ParserState *state, const char* token_start, size_t token_len, const char *format, ...);


// --- cJSON Helper ---
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
    state->original_content_buffer = strdup(yaml_content);
    if (!state->original_content_buffer) return;

    const char *start = state->original_content_buffer;
    const char *p = start;

    while (true) {
        if (*p == '\n' || *p == '\0') {
            if (state->num_lines >= 8192) {
                set_error(state, NULL, 0, "Exceeded maximum number of lines (8192)");
                return;
            }
            // strndup is safe for zero-length lines
            state->lines[state->num_lines++] = strndup(start, p - start);
            start = p + 1;
        }
        if (*p == '\0') {
            break;
        }
        p++;
    }
}

// --- Value Parser ---
static cJSON* parse_value(const char **p_str, ParserState *state) {
    const char *ptr = *p_str;
    while(isspace((unsigned char)*ptr)) ptr++;

    if (*ptr == '[' || *ptr == '{') {
        char start_char = *ptr;
        char end_char = (start_char == '[') ? ']' : '}';
        cJSON *root = (start_char == '[') ? cJSON_CreateArray() : cJSON_CreateObject();

        ptr++;

        while (*ptr) {
            while (isspace((unsigned char)*ptr)) ptr++;
            if (*ptr == end_char || *ptr == '\0') break;

            if (cJSON_IsArray(root)) {
                 cJSON_AddItemToArray(root, parse_value(&ptr, state));
            } else { // Object
                const char* key_start_ptr = ptr;
                cJSON* key_node = parse_value(&ptr, state);

                char key_string[128];
                if (cJSON_IsString(key_node)) {
                    strncpy(key_string, key_node->valuestring, sizeof(key_string) - 1);
                    key_string[sizeof(key_string) - 1] = '\0';
                } else if (cJSON_IsNumber(key_node)) {
                    snprintf(key_string, sizeof(key_string), "%g", key_node->valuedouble);
                } else {
                    set_error(state, key_start_ptr, (ptr - key_start_ptr), "Invalid key in flow-style map (must be a string or number)");
                    cJSON_Delete(key_node);
                    cJSON_Delete(root);
                    return cJSON_CreateNull();
                }

                while(isspace((unsigned char)*ptr)) ptr++;
                const char* colon_pos = ptr;
                if (*ptr != ':') {
                    set_error(state, colon_pos, 1, "Expected ':' in flow-style map");
                    cJSON_Delete(key_node);
                    cJSON_Delete(root);
                    return cJSON_CreateNull();
                }
                ptr++; // Skip ':'
                cjson_add_item_to_object_with_duplicates(root, key_string, parse_value(&ptr, state));
                cJSON_Delete(key_node);
            }

            while(isspace((unsigned char)*ptr)) ptr++;
            if (*ptr == ',') ptr++;
            else if (*ptr != end_char) {
                set_error(state, ptr, 1, "Expected ',' or '%c' in flow collection", end_char);
                cJSON_Delete(root);
                return cJSON_CreateNull();
            }
        }
        if (*ptr == end_char) ptr++;
        *p_str = ptr;
        return root;
    }

    if (*ptr == '"' || *ptr == '\'') {
        char quote = *ptr;
        ptr++;
        const char* start = ptr;
        while (*ptr && *ptr != quote) {
            if (*ptr == '\\') ptr++;
            ptr++;
        }
        char* val = strndup(start, ptr - start);
        if (*ptr == quote) ptr++;
        *p_str = ptr;
        cJSON* node = cJSON_CreateString(val);
        free(val);
        return node;
    }

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

static cJSON* parse_value_string(const char *str, ParserState* state) {
    const char *p = str;
    return parse_value(&p, state);
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
    if (state->stack_top > -1) {
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
    if (token_start && state->line_num > 0 && state->line_num <= state->num_lines) {
        const char* line_start = state->lines[state->line_num - 1];
        int col = token_start - line_start;
        size_t len = (token_len > 0) ? token_len : 1;

        if (col < 0 || col >= YAML_PARSER_MAX_LINE_LEN) {
            col = 0; // Fallback
        }

        char underline[YAML_PARSER_MAX_LINE_LEN] = {0};
        memset(underline, ' ', col);
        memset(underline + col, '^', len);

        char temp_line_buffer[YAML_PARSER_MAX_LINE_LEN + 128];
        snprintf(temp_line_buffer, sizeof(temp_line_buffer), "%.*s%s%.*s%s%s",
                 col, line_start,                                     // Part before token
                 ANSI_BOLD ANSI_COLOR_RED, (int)len, token_start,      // The token, highlighted
                 ANSI_COLOR_RESET,                                     // Reset color
                 token_start + len                                     // Part after token
        );

        snprintf(context_buffer, sizeof(context_buffer),
                 "\n\n%s    ---> Error context (Line %d, Col %d):%s\n"
                 "%s%4d |%s %s\n"
                 "       | %s%s%s\n",
                 ANSI_BOLD, state->line_num, col + 1, ANSI_COLOR_RESET,
                 ANSI_BOLD, state->line_num, ANSI_COLOR_RESET, temp_line_buffer,
                 ANSI_COLOR_RED, underline, ANSI_COLOR_RESET
                );
    }

    size_t total_len = strlen(message_buffer) + strlen(context_buffer) + 64;
    char *err_str = malloc(total_len);
    if (err_str) {
        snprintf(err_str, total_len, "YAML Parse Error: %s%s", message_buffer, context_buffer);
        *state->error = err_str;
    } else {
        char* fallback_err = malloc(strlen(message_buffer) + 32);
        if (fallback_err) {
             sprintf(fallback_err, "YAML Error on line %d: %s", state->line_num, message_buffer);
            *state->error = fallback_err;
        }
    }
}

// --- Main Parsing Logic ---

void parse_line(ParserState *state, int line_idx) {
    state->line_num = line_idx + 1;
    char* original_line = state->lines[line_idx];

    int indent = get_indent(original_line);
    char* content = trim_whitespace(original_line + indent);
    if (*content == '\0' || *content == '#') {
        return;
    }

    const char* error_pos = content;

    while (state->stack_top > 0 && indent <= state->stack[state->stack_top].indent) {
        pop_stack(state);
    }

    cJSON *parent = state->stack[state->stack_top].node;

    if (*content == '-') {
        if (!cJSON_IsArray(parent)) {
            set_error(state, error_pos, 1, "Found list item '-' in a non-array context.");
            return;
        }

        char *item_content = trim_whitespace(content + 1);
        const char* item_content_error_pos = item_content;

        if (*item_content == '{' || *item_content == '[') {
            state->ptr = item_content_error_pos;
            cJSON* flow_node = parse_value_string(item_content, state);
            cJSON_AddItemToArray(parent, flow_node);
            return;
        }

        cJSON *new_obj = cJSON_CreateObject();
        cJSON_AddItemToArray(parent, new_obj);
        push_stack(state, new_obj, indent);

        parent = new_obj;

        if (*item_content != '\0') {
            content = item_content;
            error_pos = item_content_error_pos;
        } else {
            return;
        }
    }

    if (!cJSON_IsObject(parent)) {
        set_error(state, error_pos, strlen(content), "Found key-value pair in a non-object context.");
        return;
    }

    char *colon = strchr(content, ':');
    if (!colon) {
        set_error(state, error_pos, strlen(content), "Invalid mapping (missing ':')");
        return;
    }
    *colon = '\0';
    char *key = trim_whitespace(content);
    char *val_str = colon + 1;

    bool in_quotes = false;
    char *p = val_str;
    while(*p) {
        if (*p == '"' || *p == '\'') in_quotes = !in_quotes;
        else if (*p == '#' && !in_quotes) { *p = '\0'; break; }
        p++;
    }
    char *trimmed_val_str = trim_whitespace(val_str);
    const char* val_error_pos = trimmed_val_str;

    if (*trimmed_val_str == '\0') {
        int next_content_indent = -1;
        char first_char_of_next_content = '\0';
        for (int j = line_idx + 1; j < state->num_lines; j++) {
            char* next_line = state->lines[j];
            int current_indent = get_indent(next_line);
            char* current_content = trim_whitespace(next_line + current_indent);

            if (*current_content == '\0' || *current_content == '#') continue;
            next_content_indent = current_indent;
            first_char_of_next_content = *current_content;
            break;
        }

        if (next_content_indent > indent) {
            cJSON *new_container = (first_char_of_next_content == '-') ? cJSON_CreateArray() : cJSON_CreateObject();
            cjson_add_item_to_object_with_duplicates(parent, key, new_container);
            push_stack(state, new_container, indent);
        } else {
            cjson_add_item_to_object_with_duplicates(parent, key, cJSON_CreateNull());
        }
    } else {
        state->ptr = val_error_pos;
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

    split_lines(&state, yaml_content);
    if (state.error && *state.error) {
        free(state.original_content_buffer);
        // Free the lines that were allocated before the error
        for(int i=0; i<state.num_lines; i++) free(state.lines[i]);
        return NULL;
    }

    int first_content_line_idx = -1;
    bool root_is_sequence = false;
    cJSON *root = NULL;

    for (int i = 0; i < state.num_lines; i++) {
        char* content = trim_whitespace(state.lines[i]);
        if (*content == '\0' || *content == '#') continue;

        first_content_line_idx = i;
        if (*content == '-') {
            root_is_sequence = true;
        }
        break;
    }

    if (first_content_line_idx == -1) {
        root = cJSON_CreateArray();
    } else {
        root = root_is_sequence ? cJSON_CreateArray() : cJSON_CreateObject();
        push_stack(&state, root, -1);
        for (int i = first_content_line_idx; i < state.num_lines; i++) {
            parse_line(&state, i);
            if (state.error && *state.error) break;
        }
    }

    if (state.error && *state.error) {
        cJSON_Delete(root);
        root = NULL;
    } else if (root && !root_is_sequence) {
        cJSON* array_wrapper = cJSON_CreateArray();
        cJSON_AddItemToArray(array_wrapper, root);
        root = array_wrapper;
    }

    // Free all allocated memory
    free(state.original_content_buffer);
    for(int i=0; i<state.num_lines; i++) free(state.lines[i]);

    return root;
}

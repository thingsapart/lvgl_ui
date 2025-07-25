#include "registry.h"
#include <stdlib.h> // For malloc, calloc, free, NULL
#include <string.h> // For strdup, strcmp
#include <stdio.h> // For perror, fprintf, stderr
#include "utils.h" // For render_abort, print_warning, levenshtein_distance
#include "debug_log.h"

// --- Global Configuration (from main.c) ---
extern bool g_strict_mode;
extern bool g_strict_registry_mode;

// --- Data structure for sorting suggestions ---
typedef struct {
    const char* key;
    int distance;
} Suggestion;

// --- qsort comparison function ---
static int compare_suggestions(const void *a, const void *b) {
    const Suggestion *sug_a = (const Suggestion *)a;
    const Suggestion *sug_b = (const Suggestion *)b;
    return sug_a->distance - sug_b->distance;
}

// --- Forward Declarations ---
static void registry_dump_suggestions(const Registry* reg, const char* misspelled_key);


// --- Implementation ---

Registry* registry_create() {
    Registry* reg = (Registry*)calloc(1, sizeof(Registry));
    if (!reg) {
        perror("Failed to allocate registry");
        return NULL;
    }
    return reg;
}

void registry_free(Registry* reg) {
    if (!reg) return;

    ComponentRegistryNode* current_comp = reg->components;
    while (current_comp) {
        ComponentRegistryNode* next_comp = current_comp->next;
        free(current_comp->name);
        // Note: component_root is a pointer to a node in the main cJSON doc,
        // so we do NOT free it here. It's freed when cJSON_Delete is called on the root.
        free(current_comp);
        current_comp = next_comp;
    }

    VarRegistryNode* current_var = reg->generated_vars;
    while (current_var) {
        VarRegistryNode* next_var = current_var->next;
        free(current_var->name);
        free(current_var->c_var_name);
        free(current_var->c_type);
        free(current_var);
        current_var = next_var;
    }

    PointerRegistryNode* current_ptr_node = reg->pointers;
    while (current_ptr_node) {
        PointerRegistryNode* next_ptr_node = current_ptr_node->next;
        free(current_ptr_node->id);
        free(current_ptr_node->json_type);
        free(current_ptr_node->c_type);
        free(current_ptr_node);
        current_ptr_node = next_ptr_node;
    }

    StringRegistryNode* current_str_node = reg->strings;
    while (current_str_node) {
        StringRegistryNode* next_str_node = current_str_node->next;
        free(current_str_node->value);
        free(current_str_node);
        current_str_node = next_str_node;
    }

    StaticArrayRegistryNode* current_arr_node = reg->static_arrays;
    while (current_arr_node) {
        StaticArrayRegistryNode* next_arr_node = current_arr_node->next;
        if (current_arr_node->ptr) {
            free(current_arr_node->ptr);
        }
        free(current_arr_node);
        current_arr_node = next_arr_node;
    }

    free(reg);
}

static void registry_dump_suggestions(const Registry* reg, const char* misspelled_key) {
    if (!reg || !misspelled_key) return;

    // Count the number of keys to allocate memory for suggestions
    int key_count = 0;
    for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
        key_count++;
    }

    if (key_count == 0) {
        print_hint("Registry is empty, no suggestions available.");
        return;
    }

    // Allocate memory for suggestions
    Suggestion* suggestions = (Suggestion*)malloc(key_count * sizeof(Suggestion));
    if (!suggestions) {
        // Fallback to a simple unsorted dump if malloc fails
        print_hint("Could not allocate memory for suggestions, dumping unsorted keys:");
        fprintf(stderr, "      [ ");
        bool first = true;
        for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
            if (!first) fprintf(stderr, ", ");
            fprintf(stderr, "'@%s'", node->id);
            first = false;
        }
        fprintf(stderr, " ]\n");
        return;
    }

    // Populate the suggestions array
    int i = 0;
    for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
        suggestions[i].key = node->id;
        suggestions[i].distance = levenshtein_distance(misspelled_key, node->id);
        i++;
    }

    // Sort the suggestions by Levenshtein distance
    qsort(suggestions, key_count, sizeof(Suggestion), compare_suggestions);

    // Print the sorted suggestions
    print_hint("Did you mean one of these? (Sorted by similarity)");
    fprintf(stderr, "      [ ");
    int suggestions_to_show = (key_count < 10) ? key_count : 10;
    for (i = 0; i < suggestions_to_show; i++) {
        if (i > 0) fprintf(stderr, ", ");
        fprintf(stderr, "'@%s'", suggestions[i].key);
    }
    if (key_count > suggestions_to_show) fprintf(stderr, ", ...");
    fprintf(stderr, " ]\n");

    // Free the temporary array
    free(suggestions);
}


// --- Pointer Registration ---

void registry_add_pointer(Registry* reg, void* ptr, const char* id, const char* json_type, const char* c_type) {
    if (!reg || !id) return; // Allow adding NULL pointers
    const char* key = (id[0] == '@') ? id + 1 : id;

    PointerRegistryNode* new_node = (PointerRegistryNode*)calloc(1, sizeof(PointerRegistryNode));
    if (!new_node) render_abort("Failed to allocate pointer registry node");

    new_node->id = strdup(key);
    new_node->json_type = strdup(json_type);
    new_node->c_type = strdup(c_type);
    new_node->ptr = ptr;
    new_node->next = reg->pointers;
    reg->pointers = new_node;
}

void* registry_get_pointer(const Registry* reg, const char* id, const char* type) {
    if (!reg || !id) return NULL;
    const char* key = (id[0] == '@') ? id + 1 : id;
    for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
        if (strcmp(node->id, key) == 0) {
            if (type && node->json_type && strcmp(node->json_type, type) == 0) {
                return node->ptr;
            } else if (!type) {
                return node->ptr;
            }
        }
    }

    char error_buf[256];
    snprintf(error_buf, sizeof(error_buf), "Reference Error: Object with ID '%s' not found in the registry.", id);

    if (g_strict_mode || g_strict_registry_mode) {
        render_abort(error_buf);
    } else {
        print_warning("%s", error_buf);
        registry_dump_suggestions(reg, key);
    }

    return NULL;
}

const char* registry_get_id_from_pointer(const Registry* reg, const void* ptr) {
    if (!reg || !ptr) return NULL;
    for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
        if (node->ptr == ptr) {
            return node->id;
        }
    }
    return NULL;
}


// --- String Registration ---

const char* registry_add_str(Registry* reg, const char* value) {
    if (!reg || !value) return NULL;
    for (StringRegistryNode* node = reg->strings; node; node = node->next) {
        if (strcmp(node->value, value) == 0) return node->value;
    }

    StringRegistryNode* new_node = (StringRegistryNode*)malloc(sizeof(StringRegistryNode));
    if (!new_node) render_abort("Failed to allocate string registry node");

    new_node->value = strdup(value);
    new_node->next = reg->strings;
    reg->strings = new_node;
    return new_node->value;
}


void registry_add_component(Registry* reg, const char* name, const cJSON* component_root) {
    if (!reg || !name || !component_root) return;
    const char* key = (name[0] == '@') ? name + 1 : name;

    ComponentRegistryNode* new_node = (ComponentRegistryNode*)malloc(sizeof(ComponentRegistryNode));
    if (!new_node) render_abort("Failed to allocate component registry node");
    new_node->name = strdup(key);
    new_node->component_root = component_root;
    new_node->next = reg->components;
    reg->components = new_node;
}

void registry_print_components(const Registry* reg) {
    if (!reg) return;
    for (ComponentRegistryNode* node = reg->components; node; node = node->next) {
      printf("Component %s:\n", node->name);
      cJSON_Print(node->component_root);
    }
}

const cJSON* registry_get_component(const Registry* reg, const char* name) {
    if (!reg || !name) return NULL;
    const char* key = (name[0] == '@') ? name + 1 : name;
    for (ComponentRegistryNode* node = reg->components; node; node = node->next) {
        if (strcmp(node->name, key) == 0) return node->component_root;
    }
    return NULL;
}

void registry_add_generated_var(Registry* reg, const char* name, const char* c_var_name, const char* c_type) {
    if (!reg || !name || !c_var_name) return; // c_type can be null
    const char* key = (name[0] == '@') ? name + 1 : name;

    VarRegistryNode* new_node = (VarRegistryNode*)malloc(sizeof(VarRegistryNode));
    if (!new_node) render_abort("Failed to allocate var registry node");
    new_node->name = strdup(key);
    new_node->c_var_name = strdup(c_var_name);
    new_node->c_type = c_type ? strdup(c_type) : NULL;

    new_node->next = reg->generated_vars;
    reg->generated_vars = new_node;
}

const char* registry_get_generated_var(const Registry* reg, const char* name) {
    if (!reg || !name) return NULL;
    const char* key = (name[0] == '@') ? name + 1 : name;
    for (VarRegistryNode* node = reg->generated_vars; node; node = node->next) {
        if (strcmp(node->name, key) == 0) return node->c_var_name;
    }
    return NULL;
}

const char* registry_get_c_type_for_id(const Registry* reg, const char* name) {
    if (!reg || !name) return NULL;
    const char* key = (name[0] == '@') ? name + 1 : name;
    // Check generated variables first
    for (VarRegistryNode* node = reg->generated_vars; node; node = node->next) {
        if (strcmp(node->name, key) == 0) return node->c_type;
    }
    // Check runtime pointers
    for (PointerRegistryNode* node = reg->pointers; node; node = node->next) {
        if (strcmp(node->id, key) == 0) return node->c_type;
    }
    return NULL; // Not found
}

void registry_add_static_array(Registry* reg, void* ptr) {
    if (!reg || !ptr) return;
    StaticArrayRegistryNode* new_node = malloc(sizeof(StaticArrayRegistryNode));
    if (!new_node) render_abort("Failed to allocate static array node");
    new_node->ptr = ptr;
    new_node->next = reg->static_arrays;
    reg->static_arrays = new_node;
}

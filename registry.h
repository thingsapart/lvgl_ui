#ifndef REGISTRY_H
#define REGISTRY_H

#include <cJSON/cJSON.h>
#include "lvgl.h" // For lv_coord_t etc.

// --- Data structures for pointer and string registration ---
typedef struct PointerRegistryNode {
    char* id;           // The ID used in JSON (e.g., "my_button")
    char* json_type;    // Specific JSON type from the "type" field (e.g. "label", "button", "style")
    char* c_type;       // The C type of the pointer (e.g. "lv_obj_t*", "lv_style_t*")
    void* ptr;          // Pointer to the actual C object/style (not owned by registry)
    struct PointerRegistryNode* next;
} PointerRegistryNode;

typedef struct StringRegistryNode {
    char* value;
    struct StringRegistryNode* next;
} StringRegistryNode;

typedef struct StaticArrayRegistryNode {
    void* ptr;
    struct StaticArrayRegistryNode* next;
} StaticArrayRegistryNode;

// Forward declaration for Registry to be used in ComponentRegistryNode etc. if needed
// struct Registry; // Not strictly needed here as they are just members

// --- Internal node types for original registry functions ---
// These were previously only in registry.c but are needed for the full Registry struct definition
typedef struct ComponentRegistryNode {
    char* name;                 // Key (e.g., "comp_name" from "@comp_name")
    const cJSON* component_root; // Value (pointer to the cJSON object)
    struct ComponentRegistryNode* next;
} ComponentRegistryNode;

typedef struct VarRegistryNode {
    char* name;        // Key (e.g., "style_id" from "@style_id")
    char* c_var_name;  // Value (e.g., "style_0" or "button_1")
    char* c_type;      // The C type of the variable (e.g. "lv_style_t*", "lv_obj_t*")
    struct VarRegistryNode* next;
} VarRegistryNode;


// --- Opaque pointer for the registry ---
typedef struct Registry Registry; // Keep this for API type safety if Registry struct is defined below

// --- Lifecycle ---
Registry* registry_create();
void registry_free(Registry* reg);

// --- Component Management ---
void registry_add_component(Registry* reg, const char* name, const cJSON* component_root);
const cJSON* registry_get_component(const Registry* reg, const char* name);
void registry_print_components(const Registry* reg);

// --- Generated Variable Name Management ---
void registry_add_generated_var(Registry* reg, const char* name, const char* c_var_name, const char* c_type);
const char* registry_get_generated_var(const Registry* reg, const char* name);
const char* registry_get_c_type_for_id(const Registry* reg, const char* name);


// --- Pointer Registry ---
// Note: The registry does NOT take ownership of the 'ptr' itself.
// The 'id', 'json_type', and 'c_type' strings are duplicated by the registry.
void registry_add_pointer(Registry* reg, void *ptr, const char *id, const char *json_type, const char* c_type);
void *registry_get_pointer(const Registry* reg, const char *id, const char *type);
const char* registry_get_id_from_pointer(const Registry* reg, const void* ptr);

// --- String Registry ---
const char *registry_add_str(Registry* reg, const char *value);

// --- Static Array Registry ---
void registry_add_static_array(Registry* reg, void* ptr);

// --- Debugging ---
void registry_dump(const Registry* reg);
void registry_dump_suggestions(const Registry* reg, const char* misspelled_key);


// --- Registry struct ---
// This is the full definition of the Registry.
struct Registry {
    ComponentRegistryNode* components;
    VarRegistryNode* generated_vars;
    PointerRegistryNode* pointers;
    StringRegistryNode* strings;
    StaticArrayRegistryNode* static_arrays;
};

#endif // REGISTRY_H

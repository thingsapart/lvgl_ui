#ifndef REGISTRY_H
#define REGISTRY_H

#include <cJSON/cJSON.h>

// --- Data structures for pointer and string registration ---
typedef struct PointerRegistryNode {
    char* id;
    char* type;
    void* ptr;
    struct PointerRegistryNode* next;
} PointerRegistryNode;

typedef struct StringRegistryNode {
    char* value;
    struct StringRegistryNode* next;
} StringRegistryNode;

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
    struct VarRegistryNode* next;
} VarRegistryNode;


// --- Opaque pointer for the registry ---
typedef struct Registry Registry; // Keep this for API type safety if Registry struct is defined below

// --- Lifecycle ---
Registry* registry_create();
void registry_free(Registry* reg);

// --- Component Management ---
// Components are top-level objects from the "components" section of the UI spec.
// They are identified by a name (e.g., "my_custom_button").
// The `component_root` is a pointer to the cJSON object defining the component.
// This cJSON object is owned by the main parsed document and should NOT be freed by the registry.
void registry_add_component(Registry* reg, const char* name, const cJSON* component_root);
const cJSON* registry_get_component(const Registry* reg, const char* name);

// --- Generated Variable Name Management ---
// Manages mappings from a UI-spec name (e.g., "style_main_button", "widget_login_button")
// to a generated C variable name (e.g., "style_0", "button_1").
// This is used to ensure that if a style or widget is referenced by its name multiple times,
// we re-use the same generated C variable.
// Both `name` and `c_var_name` are duplicated by the registry and freed by `registry_free`.
void registry_add_generated_var(Registry* reg, const char* name, const char* c_var_name);
const char* registry_get_generated_var(const Registry* reg, const char* name);

// --- Pointer Registry ---
// Note: The registry does NOT take ownership of the 'ptr' itself.
// The 'id' and 'type' strings are duplicated by the registry.
void registry_add_pointer(Registry* reg, void *ptr, const char *id, const char *type);
void *registry_get_pointer(const Registry* reg, const char *id, const char *type);

// --- String Registry ---
// Adds a string to an internal registry. If the string already exists,
// it returns a pointer to the existing string. Otherwise, it duplicates
// the input string, stores it, and returns a pointer to the new copy.
// The registry owns the memory for these stored strings.
const char *registry_add_str(Registry* reg, const char *value);

// --- Registry struct ---
// This is the full definition of the Registry.
struct Registry {
    ComponentRegistryNode* components;   // For original component management
    VarRegistryNode* generated_vars; // For original generated variable name management
    PointerRegistryNode* pointers;     // For new pointer registration
    StringRegistryNode* strings;      // For new string registration
};

#endif // REGISTRY_H

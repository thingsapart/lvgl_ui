#ifndef API_SPEC_H
#define API_SPEC_H

#include <stdbool.h>

#include <cJSON/cJSON.h>

// --- Data Structures for API Spec ---

// Holds information about a single property (e.g., width, text, bg_color)
typedef struct {
    char* name;                   // Property name (e.g., "width")
    char* c_type;                 // Corresponding C type (e.g., "int", "const char*", "lv_color_t")
    char* setter;                 // Name of the LVGL setter function (e.g., "lv_obj_set_width")
    char* widget_type_hint;       // For which widget type this setter is primarily for (e.g. "obj", "label", "style") helps construct setter if not explicit
    int num_style_args;           // For style properties, indicates number of leading lv_style_selector_t/lv_part_t args (0, 1, or 2 typically)
                                  // e.g. lv_style_set_radius(style, state, value) -> 1 (state)
                                  // e.g. lv_obj_set_style_local_radius(obj, part, state, value) -> 2 (part, state)
    char* style_part_default;     // Default part for style properties (e.g. "LV_PART_MAIN", "LV_PART_SCROLLBAR")
    char* style_state_default;    // Default state for style properties (e.g. "LV_STATE_DEFAULT", "LV_STATE_PRESSED")
    bool is_style_prop;           // True if this property is generally set on style objects or via local style setters
} PropertyDefinition; // Renamed from PropertyInfo

typedef struct PropertyDefinitionNode {
    PropertyDefinition* prop;
    struct PropertyDefinitionNode* next;
} PropertyDefinitionNode;

typedef struct WidgetDefinition {
    char* name;         // Type name from JSON key (e.g., "button", "style")
    char* inherits;     // Optional parent type name
    char* create;       // Optional LVGL creation function name (e.g., "lv_btn_create")
    char* c_type;       // ADDED: C type for objects (e.g., "lv_style_t", "lv_anim_t")
    char* init_func;    // ADDED: Initialization function for objects (e.g., "lv_style_init")
    PropertyDefinitionNode* properties; // Linked list of applicable properties
    // char* parent_type; // Optional: expected parent type
} WidgetDefinition;

// Node for the linked list of widget definitions (maps widget type name to its definition)
typedef struct WidgetMapNode {
    char* name;                             // Name of the widget type (key, e.g., "button")
    WidgetDefinition* widget;               // Pointer to the actual widget definition
    struct WidgetMapNode* next;
} WidgetMapNode;

// Assuming FunctionMapNode might be defined elsewhere or needed later for consistency
// For now, let's assume it's similar if used.
// typedef struct FunctionMapNode {
//     struct FunctionDefinition* func; // Assuming FunctionDefinition struct exists
//     struct FunctionMapNode* next;
// } FunctionMapNode;


// Represents the parsed API specification (e.g., from api_spec.json)
typedef struct ApiSpec {
    WidgetMapNode* widgets_list_head;                   // Head of the linked list for widget definitions
    // GlobalPropertyDefinitionNode* global_properties_list_head; // Removed
    struct FunctionMapNode* functions;                  // Head of linked list for functions
    const cJSON* constants;                             // Reference to parsed constants from JSON (owned by main cJSON doc)
    const cJSON* enums;                                 // Reference to parsed enums from JSON (owned by main cJSON doc)
} ApiSpec;


// --- Function Declarations ---

// Parses the API specification from a cJSON object.
// Returns a pointer to an ApiSpec structure, or NULL on error.
ApiSpec* api_spec_parse(const cJSON* root_json);

// Frees the ApiSpec structure and all its contents.
void api_spec_free(ApiSpec* spec);

const WidgetDefinition* api_spec_find_widget(const ApiSpec* spec, const char* widget_name);

// Retrieves property definition for a specific widget type or API object type.
// type_name: e.g., "button", "label", "obj" (for common ones), "style"
// prop_name: the name of the property.
// Returns NULL if the property is not found for that type.
const PropertyDefinition* api_spec_find_property(const ApiSpec* spec, const char* type_name, const char* prop_name);

// Retrieves the raw cJSON object for constants.
const cJSON* api_spec_get_constants(const ApiSpec* spec);

// Retrieves the raw cJSON object for enums.
const cJSON* api_spec_get_enums(const ApiSpec* spec);

// Retrieves the create function name for a given widget definition.
const char* widget_get_create_func(const WidgetDefinition* widget);

#endif // API_SPEC_H

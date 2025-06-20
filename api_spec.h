#ifndef API_SPEC_H
#define API_SPEC_H

#include <stdbool.h>
#include <cJSON/cJSON.h>

// --- Function Definition Structures ---
typedef struct FunctionArg {
    const char* name;             // Argument name (e.g., "val", "obj", "p")
    const char* type;             // Argument C type (e.g., "lv_obj_t*", "int32_t", "lv_opa_t")
    const char* enum_type_name;   // If type is an enum, its name (e.g., "lv_opa_t")
    struct FunctionArg* next;
} FunctionArg;

typedef struct FunctionDefinition {
    const char* name;        // Function name (e.g., "lv_obj_set_width")
    const char* return_type; // Return C type (e.g., "void", "lv_obj_t*")
    FunctionArg* args;       // Linked list of argument types/names
    int num_args;            // Total number of arguments
    int min_args;            // Minimum number of required arguments
} FunctionDefinition;

// Node for a linked list acting as a map/list of functions
typedef struct FunctionMapNode {
    const char* name; // Key for the map (function name, e.g. "lv_obj_set_width")
    FunctionDefinition* func_def; // Pointer to the actual function definition
    struct FunctionMapNode* next;
} FunctionMapNode;


// --- Data Structures for API Spec ---

// Holds information about a single property (e.g., width, text, bg_color)
typedef struct {
    const char* name;                   // Property name (e.g., "width")
    const char* c_type;                 // Corresponding C type (e.g., "int", "const char*", "lv_color_t")
    const char* enum_type_name;         // If c_type is an enum, its name (e.g., "lv_opa_t")
    const char* setter;                 // Name of the LVGL setter function (e.g., "lv_obj_set_width")
    const char* widget_type_hint;       // For which widget type this setter is primarily for (e.g. "obj", "label", "style")
    int num_style_args;                 // For style properties, indicates number of leading lv_style_selector_t/lv_part_t args
    const char* style_part_default;     // Default part for style properties
    const char* style_state_default;    // Default state for style properties
    bool is_style_prop;                 // True if this property is generally set on style objects
    const char* obj_setter_prefix;      // For global properties, e.g. "lv_obj_set_style" for "text_color"
    FunctionArg* func_args;             // Arguments if this property is a function with a known signature.
} PropertyDefinition;

typedef struct PropertyDefinitionNode {
    PropertyDefinition* prop;
    struct PropertyDefinitionNode* next;
} PropertyDefinitionNode;

typedef struct WidgetDefinition {
    const char* name;                   // Type name from JSON key (e.g., "button", "style")
    const char* inherits;               // Optional parent type name
    const char* create;                 // Optional LVGL creation function name (e.g., "lv_btn_create")
    const char* c_type;                 // C type for objects (e.g., "lv_style_t", "lv_anim_t")
    const char* init_func;              // Initialization function for objects (e.g., "lv_style_init")
    const char* json_type_override;     // If the type for runtime registry differs from 'name' (e.g. components)
    PropertyDefinitionNode* properties; // Linked list of applicable properties
    FunctionMapNode* methods;           // Linked list of methods specific to this widget
} WidgetDefinition;

// Node for the linked list of widget definitions
typedef struct WidgetMapNode {
    const char* name;                   // Name of the widget type (key, e.g., "button")
    WidgetDefinition* widget;           // Pointer to the actual widget definition
    struct WidgetMapNode* next;
} WidgetMapNode;


// Represents the parsed API specification
typedef struct ApiSpec {
    WidgetMapNode* widgets_list_head;           // Head of the linked list for widget definitions
    FunctionMapNode* functions_list_head;       // Head of linked list for global functions
    const cJSON* constants_json_node;           // Reference to parsed constants from JSON
    const cJSON* enums_json_node;               // Reference to parsed enums from JSON
    const cJSON* global_properties_json_node;   // Reference to global #/properties from JSON
} ApiSpec;


// --- Function Declarations ---

ApiSpec* api_spec_parse(const cJSON* root_json);
void api_spec_free(ApiSpec* spec);

const WidgetDefinition* api_spec_find_widget(const ApiSpec* spec, const char* widget_name);
const PropertyDefinition* api_spec_find_property(const ApiSpec* spec, const char* type_name, const char* prop_name);

const cJSON* api_spec_get_constants(const ApiSpec* spec);
const cJSON* api_spec_get_enums(const ApiSpec* spec);
const cJSON* api_spec_get_enum(const ApiSpec* spec, const char* enum_name); // Get specific enum definition

const char* widget_get_create_func(const WidgetDefinition* widget);

// Function lookups
const FunctionDefinition* api_spec_get_function(const ApiSpec* spec, const char* func_name);
const char* api_spec_get_function_return_type(const ApiSpec* spec, const char* func_name); // Can be derived from api_spec_get_function

// Type checking helpers
bool api_spec_is_enum_value(const ApiSpec* spec, const char* value_str);
bool api_spec_is_enum_type(const ApiSpec* spec, const char* type_name);
bool api_spec_is_constant(const ApiSpec* spec, const char* key);

#endif // API_SPEC_H

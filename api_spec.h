#ifndef API_SPEC_H
#define API_SPEC_H

#include <stdbool.h>
#include <cJSON.h> // Standardized include path

// --- Function Definition Structures ---
// (Moved before PropertyDefinition and WidgetDefinition)
typedef struct FunctionArg {
    char* type; // Argument type as string (e.g., "lv_obj_t*", "int32_t")
    char* name; // Optional: if argument names are available in JSON in the future
    char* expected_enum_type; // Expected enum type for this argument, if applicable
    struct FunctionArg* next;
} FunctionArg;

typedef struct {
    char* name;         // Function name (e.g., "lv_obj_set_width")
    char* return_type;
    FunctionArg* args_head;  // Linked list of argument types
} FunctionDefinition;

// Node for a linked list acting as a map/list of functions
typedef struct FunctionMapNode {
    char* name; // Key for the map (function name, e.g. "lv_obj_set_width")
    FunctionDefinition* func_def; // Pointer to the actual function definition
    struct FunctionMapNode* next;
} FunctionMapNode;


// --- Data Structures for API Spec ---

// Holds information about a single property (e.g., width, text, bg_color)
typedef struct {
    char* name;                   // Property name (e.g., "width")
    char* c_type;                 // Corresponding C type (e.g., "int", "const char*", "lv_color_t")
    char* setter;                 // Name of the LVGL setter function (e.g., "lv_obj_set_width")
    char* widget_type_hint;       // For which widget type this setter is primarily for (e.g. "obj", "label", "style") helps construct setter if not explicit
    bool is_style_prop;           // True if this property is generally set on style objects or via local style setters
    char* obj_setter_prefix;      // For global properties, e.g. "lv_obj_set_style" for "text_color" -> "lv_obj_set_style_text_color"
    const FunctionArg* func_args; // NEW FIELD: Linked list of arguments if this property resolves to a function/method with a known signature.
    char* expected_enum_type;     // MODIFIED: Name of the enum type expected by this property (now non-const).
    bool is_heap_allocated;       // NEW: True if this struct was allocated on the heap and must be freed by the caller.
} PropertyDefinition;

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
    FunctionMapNode* methods; // Linked list of methods specific to this widget
} WidgetDefinition;

// Node for the linked list of widget definitions (maps widget type name to its definition)
typedef struct WidgetMapNode {
    char* name;                             // Name of the widget type (key, e.g., "button")
    WidgetDefinition* widget;               // Pointer to the actual widget definition
    struct WidgetMapNode* next;
} WidgetMapNode;


// Represents the parsed API specification (e.g., from api_spec.json)
typedef struct ApiSpec {
    WidgetMapNode* widgets_list_head;                   // Head of the linked list for widget definitions
    FunctionMapNode* functions;                  // Head of linked list for functions
    const cJSON* constants;                             // Reference to parsed constants from JSON (owned by main cJSON doc)
    const cJSON* enums;                                 // Reference to parsed enums from JSON (owned by main cJSON doc)
    const cJSON* global_properties_json_node;           // Reference to global #/properties from JSON (owned by main cJSON doc)
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
// Returns a const pointer to a property definition. If the property is synthesized
// on-the-fly (from a method or global function), it will be heap-allocated.
// Use `prop->is_heap_allocated` to check and free it with `api_spec_free_property`
// when you are done.
const PropertyDefinition* api_spec_find_property(const ApiSpec* spec, const char* type_name, const char* prop_name);

// Frees a PropertyDefinition struct, but only if it was heap-allocated.
void api_spec_free_property(const PropertyDefinition* prop);

// Suggests a valid property name based on a misspelled one using Levenshtein distance.
// Returns a pointer to a static buffer containing the suggestion, or NULL if no good match is found.
const char* api_spec_suggest_property(const ApiSpec* spec, const char* type_name, const char* misspelled_prop);

// Retrieves the raw cJSON object for constants.
const cJSON* api_spec_get_constants(const ApiSpec* spec);

// Retrieves the raw cJSON object for enums.
const cJSON* api_spec_get_enums(const ApiSpec* spec);

// Retrieves the create function name for a given widget definition.
const char* widget_get_create_func(const WidgetDefinition* widget);

// Retrieves the C return type string of a function.
// Returns a default type (e.g., "lv_obj_t*") if the function is not found or has no return type.
const char* api_spec_get_function_return_type(const ApiSpec* spec, const char* func_name);

// Retrieves the FunctionArg list for a given function name from the global functions or widget methods.
// The caller should NOT free the returned list as it points to existing data.
const FunctionArg* api_spec_get_function_args_by_name(const ApiSpec* spec, const char* func_name);

// Checks if a given integer value is a valid value for any member of the specified enum type.
bool api_spec_is_valid_enum_int_value(const ApiSpec* spec, const char* enum_type_name, int int_value);

// Checks if a string is a member of a specific enum type.
bool api_spec_is_enum_member(const ApiSpec* spec, const char* enum_name, const char* member_name);

// Finds the integer value of a named enum member.
bool api_spec_find_enum_value(const ApiSpec* spec, const char* enum_name, const char* member_name, long* out_value);

// NEW: Finds the symbolic name of an enum member by its integer value.
// Returns a const char* to the symbol name if found, otherwise NULL. The returned string is owned by the cJSON object and should not be freed.
const char* api_spec_find_enum_symbol_by_value(const ApiSpec* spec, const char* enum_type_name, long value);


// Checks if a string is a member of any enum type defined in the spec.
bool api_spec_is_global_enum_member(const ApiSpec* spec, const char* member_name);

// Searches all enums and returns the type name for a given member.
const char* api_spec_find_global_enum_type(const ApiSpec* spec, const char* member_name);

// Checks if a string is a defined constant in the spec.
bool api_spec_is_constant(const ApiSpec* spec, const char* const_name);

// Finds the integer value of a named constant.
// Returns true if found and out_value is set, false otherwise.
bool api_spec_find_constant_value(const ApiSpec* spec, const char* const_name, long* out_value);

// Finds the string value of a named constant (e.g., for LV_SYMBOL_...).
// The string is cleaned of comments and surrounding quotes.
// Returns a heap-allocated string that the caller must free, or NULL if not found/not a string.
char* api_spec_find_constant_string(const ApiSpec* spec, const char* const_name);

// Checks if a function name exists in the API spec's global functions.
bool api_spec_has_function(const ApiSpec* spec, const char* func_name);

// Finds the integer value of a specific enum member within a given enum type.
// Returns true if found and out_value is set, false otherwise.
bool api_spec_find_enum_value(const ApiSpec* spec, const char* enum_name, const char* member_name, long* out_value);
//
// Finds a function definition in the API spec.
const FunctionDefinition* api_spec_find_function(const ApiSpec* spec, const char* func_name);


#endif // API_SPEC_H

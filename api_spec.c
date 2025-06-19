#include "api_spec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to safely strdup, returning NULL if input is NULL
static char* safe_strdup(const char* s) {
    return s ? strdup(s) : NULL;
}

ApiSpec* api_spec_parse(const cJSON* root_json) {
    if (!root_json) return NULL;

    ApiSpec* spec = (ApiSpec*)calloc(1, sizeof(ApiSpec));
    if (!spec) {
        perror("Failed to allocate ApiSpec");
        return NULL;
    }

    // --- Parse "properties" ---
    cJSON* props_json = cJSON_GetObjectItemCaseSensitive(root_json, "properties");
    if (!cJSON_IsObject(props_json)) {
        fprintf(stderr, "Error: 'properties' section is missing or not an object in API spec.\n");
        api_spec_free(spec);
        return NULL;
    }
    spec->properties_map = cJSON_CreateObject();

    cJSON* prop_item_json = NULL;
    cJSON_ArrayForEach(prop_item_json, props_json) {
        PropertyDefinition* p_def = (PropertyDefinition*)calloc(1, sizeof(PropertyDefinition)); // Renamed struct
        if (!p_def) {
            perror("Failed to allocate PropertyDefinition");
            api_spec_free(spec);
            return NULL;
        }
        p_def->name = safe_strdup(prop_item_json->string);

        cJSON* details = prop_item_json;
        p_def->c_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "c_type")));
        p_def->setter = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "setter"))); // Renamed field
        p_def->widget_type_hint = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "widget_hint")));

        cJSON* style_args_json = cJSON_GetObjectItem(details, "style_args");
        if (cJSON_IsNumber(style_args_json)) {
            p_def->num_style_args = style_args_json->valueint;
        } else {
            p_def->num_style_args = 0;
        }
        p_def->style_part_default = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "style_part_default")));
        p_def->style_state_default = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "style_state_default")));

        cJSON* is_style_json = cJSON_GetObjectItem(details, "is_style_prop");
        p_def->is_style_prop = cJSON_IsTrue(is_style_json);

        // Corrected storage using cJSON_CreateLightUserData
        cJSON_AddItemToObject(spec->properties_map, p_def->name, cJSON_CreateLightUserData(p_def));
    }

    spec->widgets = cJSON_GetObjectItemCaseSensitive(root_json, "widgets");
    if (!cJSON_IsObject(spec->widgets)) {
        fprintf(stderr, "Warning: 'widgets' section is missing or not an object in API spec.\n");
        spec->widgets = NULL;
    }

    spec->constants = cJSON_GetObjectItemCaseSensitive(root_json, "constants");
    spec->enums = cJSON_GetObjectItemCaseSensitive(root_json, "enums");
    spec->functions_map = NULL;

    return spec;
}

void api_spec_free(ApiSpec* spec) {
    if (!spec) return;

    if (spec->properties_map) {
        if (spec->properties_map->type == cJSON_Object) {
             cJSON* prop_map_entry = spec->properties_map->child;
             while(prop_map_entry) {
                if (prop_map_entry->type == cJSON_LightUserData) {
                    PropertyDefinition* p_def = (PropertyDefinition*)((cJSON_LightUserData*)prop_map_entry)->ptr; // Renamed struct
                    if (p_def) {
                        free(p_def->name);
                        free(p_def->c_type);
                        free(p_def->setter); // Renamed field
                        free(p_def->widget_type_hint);
                        free(p_def->style_part_default);
                        free(p_def->style_state_default);
                        free(p_def);
                    }
                }
                prop_map_entry = prop_map_entry->next;
             }
        }
        cJSON_Delete(spec->properties_map);
    }
    free(spec);
}

// Renamed function, combines logic of api_spec_get_property_info and api_spec_get_property_info_for_type
const PropertyDefinition* api_spec_find_property(const ApiSpec* spec, const char* type_name, const char* prop_name) {
    if (!spec || !type_name || !prop_name || !spec->properties_map) { // spec->widgets might be NULL if not defined
        return NULL;
    }

    // For "style" type, properties are directly in properties_map and marked with is_style_prop
    if (strcmp(type_name, "style") == 0) {
        cJSON* prop_item_wrapper = cJSON_GetObjectItem(spec->properties_map, prop_name);
        if (prop_item_wrapper && prop_item_wrapper->type == cJSON_LightUserData) {
            const PropertyDefinition* p_def = (const PropertyDefinition*)((cJSON_LightUserData*)prop_item_wrapper)->ptr;
            if (p_def && p_def->is_style_prop) {
                return p_def;
            }
        }
        // If not found or not a style prop, style lookups don't fallback to "obj" list.
        return NULL;
    }

    // For other types, check if the property is listed under the specific widget type in spec->widgets
    if (spec->widgets) { // Check if spec->widgets exists
        cJSON* widget_props_array = cJSON_GetObjectItem(spec->widgets, type_name);
        if (cJSON_IsArray(widget_props_array)) {
            cJSON* prop_name_in_array = NULL;
            cJSON_ArrayForEach(prop_name_in_array, widget_props_array) {
                if (cJSON_IsString(prop_name_in_array) && strcmp(prop_name_in_array->valuestring, prop_name) == 0) {
                    // Property is valid for this type, now get its full info from properties_map
                    cJSON* prop_item_wrapper = cJSON_GetObjectItem(spec->properties_map, prop_name);
                    if (prop_item_wrapper && prop_item_wrapper->type == cJSON_LightUserData) {
                         return (const PropertyDefinition*)((cJSON_LightUserData*)prop_item_wrapper)->ptr;
                    }
                    fprintf(stderr, "Warning: Property '%s' listed for type '%s' but not found in global properties_map.\n", prop_name, type_name);
                    return NULL;
                }
            }
        }
    }

    // If not found for specific type (and type is not "obj" itself), try common "obj" properties
    if (strcmp(type_name, "obj") != 0 && spec->widgets) { // Check spec->widgets again
        cJSON* obj_props_array = cJSON_GetObjectItem(spec->widgets, "obj");
        if (cJSON_IsArray(obj_props_array)) {
            cJSON* prop_name_in_array = NULL;
            cJSON_ArrayForEach(prop_name_in_array, obj_props_array) {
                if (cJSON_IsString(prop_name_in_array) && strcmp(prop_name_in_array->valuestring, prop_name) == 0) {
                    cJSON* prop_item_wrapper = cJSON_GetObjectItem(spec->properties_map, prop_name);
                    if (prop_item_wrapper && prop_item_wrapper->type == cJSON_LightUserData) {
                        return (const PropertyDefinition*)((cJSON_LightUserData*)prop_item_wrapper)->ptr;
                    }
                     fprintf(stderr, "Warning: Common property '%s' (for obj) not found in global properties_map.\n", prop_name);
                    return NULL;
                }
            }
        }
    }

    // Fallback for properties not explicitly listed under a type in "widgets" but existing in "properties_map"
    // This might be for generic properties not assigned to "obj" type list or if "widgets" section is missing.
    cJSON* prop_item_wrapper = cJSON_GetObjectItem(spec->properties_map, prop_name);
    if (prop_item_wrapper && prop_item_wrapper->type == cJSON_LightUserData) {
        // Should we verify if this property can be applied to the given type_name?
        // For now, if it's in the global map, and not found via specific type lists, return it.
        // This makes it behave like the old api_spec_get_property_info if other lookups fail.
        return (const PropertyDefinition*)((cJSON_LightUserData*)prop_item_wrapper)->ptr;
    }

    return NULL;
}

const cJSON* api_spec_get_constants(const ApiSpec* spec) {
    return spec ? spec->constants : NULL;
}

const cJSON* api_spec_get_enums(const ApiSpec* spec) {
    return spec ? spec->enums : NULL;
}

#include "api_spec.h" // Uses PropertyDefinition, WidgetDefinition
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

    // --- Parse global "properties" definitions ---
    cJSON* global_props_json = cJSON_GetObjectItemCaseSensitive(root_json, "properties");
    spec->properties_map = cJSON_CreateObject(); // Global map for all unique PropertyDefinitions
    if (cJSON_IsObject(global_props_json)) {
        cJSON* prop_item_json = NULL;
        cJSON_ArrayForEach(prop_item_json, global_props_json) {
            PropertyDefinition* p_def = (PropertyDefinition*)calloc(1, sizeof(PropertyDefinition));
            if (!p_def) {
                perror("Failed to allocate global PropertyDefinition");
                api_spec_free(spec); // Basic cleanup
                return NULL;
            }
            p_def->name = safe_strdup(prop_item_json->string);

            cJSON* details = prop_item_json;
            p_def->c_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "type"))); // JSON "type" is C type here
            p_def->setter = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "setter")));
            p_def->widget_type_hint = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "widget_hint")));

            cJSON* style_args_json = cJSON_GetObjectItem(details, "style_args");
            if (cJSON_IsNumber(style_args_json)) p_def->num_style_args = style_args_json->valueint;

            p_def->style_part_default = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "style_part_default")));
            p_def->style_state_default = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "style_state_default")));

            cJSON* is_style_json = cJSON_GetObjectItem(details, "is_style_prop");
            p_def->is_style_prop = cJSON_IsTrue(is_style_json);

            cJSON_AddItemToObject(spec->properties_map, p_def->name, cJSON_CreateLightUserData(p_def));
        }
    } else {
        fprintf(stderr, "Warning: Global 'properties' section is missing or not an object in API spec.\n");
    }

    // --- Parse "widgets" section for WidgetDefinitions ---
    cJSON* widgets_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "widgets");
    spec->widgets_map = (struct HashTable*)cJSON_CreateObject(); // Using cJSON as a simple map for WidgetDefinition*

    if (cJSON_IsObject(widgets_json_obj)) {
        cJSON* widget_json_node = NULL; // Represents a widget type, e.g., "button": { ... }
        cJSON_ArrayForEach(widget_json_node, widgets_json_obj) {
            const char* widget_type_name = widget_json_node->string;

            WidgetDefinition* wd = (WidgetDefinition*)calloc(1, sizeof(WidgetDefinition));
            if (!wd) {
                perror("Failed to allocate WidgetDefinition");
                continue;
            }
            wd->name = safe_strdup(widget_type_name);
            wd->properties = NULL;
            wd->inherits = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(widget_json_node, "inherits")));

            // 'widget_json_node' is the JSON object for the widget type.
            // It should contain a "properties" object listing its specific properties.
            cJSON* widget_specific_props_obj = cJSON_GetObjectItemCaseSensitive(widget_json_node, "properties");
            if (cJSON_IsObject(widget_specific_props_obj)) {
                cJSON* prop_detail_json = NULL; // Represents a property of the widget, e.g., "width": { ... }
                struct PropertyDefinitionNode** current_prop_node_tail = &wd->properties;

                cJSON_ArrayForEach(prop_detail_json, widget_specific_props_obj) {
                    const char* prop_key_name = prop_detail_json->string;

                    // For widget-specific properties, we create COPIES of PropertyDefinitions.
                    // This allows a widget to potentially override details of a global property,
                    // though for now, we assume they are full definitions.
                    PropertyDefinition* pd = (PropertyDefinition*)calloc(1, sizeof(PropertyDefinition));
                    if (!pd) {
                        perror("Failed to allocate PropertyDefinition for widget property list");
                        continue;
                    }
                    pd->name = safe_strdup(prop_key_name);

                    cJSON* setter_item = cJSON_GetObjectItem(prop_detail_json, "setter");
                    if (cJSON_IsString(setter_item)) pd->setter = safe_strdup(setter_item->valuestring);

                    cJSON* type_item = cJSON_GetObjectItem(prop_detail_json, "type");
                    if (cJSON_IsString(type_item)) pd->c_type = safe_strdup(type_item->valuestring);

                    pd->widget_type_hint = safe_strdup(widget_type_name);

                    cJSON* style_args_item = cJSON_GetObjectItem(prop_detail_json, "style_args");
                    if (cJSON_IsNumber(style_args_item)) pd->num_style_args = style_args_item->valueint;

                    cJSON* part_default_item = cJSON_GetObjectItem(prop_detail_json, "style_part_default");
                    if (cJSON_IsString(part_default_item)) pd->style_part_default = safe_strdup(part_default_item->valuestring);

                    cJSON* state_default_item = cJSON_GetObjectItem(prop_detail_json, "style_state_default");
                    if (cJSON_IsString(state_default_item)) pd->style_state_default = safe_strdup(state_default_item->valuestring);

                    cJSON* is_style_item = cJSON_GetObjectItem(prop_detail_json, "is_style_prop");
                    if (cJSON_IsBool(is_style_item)) pd->is_style_prop = cJSON_IsTrue(is_style_item);

                    *current_prop_node_tail = (struct PropertyDefinitionNode*)calloc(1, sizeof(struct PropertyDefinitionNode));
                    if (!*current_prop_node_tail) {
                        perror("Failed to allocate PropertyDefinitionNode");
                        free(pd->name); free(pd->setter); free(pd->c_type); free(pd->widget_type_hint);
                        free(pd->style_part_default); free(pd->style_state_default); free(pd);
                        continue;
                    }
                    (*current_prop_node_tail)->prop = pd;
                    (*current_prop_node_tail)->next = NULL;
                    current_prop_node_tail = &(*current_prop_node_tail)->next;
                }
            }
            cJSON_AddItemToObject((cJSON*)spec->widgets_map, wd->name, cJSON_CreateLightUserData(wd));
        }
    } else {
         fprintf(stderr, "Warning: 'widgets' section is missing or not an object in API spec. Widget-specific properties not parsed.\n");
    }

    spec->constants = cJSON_GetObjectItemCaseSensitive(root_json, "constants");
    spec->enums = cJSON_GetObjectItemCaseSensitive(root_json, "enums");
    spec->functions_map = NULL; // Not parsed

    return spec;
}

void api_spec_free(ApiSpec* spec) {
    if (!spec) return;

    // Free global PropertyDefinition objects stored in properties_map
    if (spec->properties_map) {
        if (spec->properties_map->type == cJSON_Object) {
             cJSON* prop_map_entry = spec->properties_map->child;
             while(prop_map_entry) {
                if (prop_map_entry->type == cJSON_LightUserData) {
                    PropertyDefinition* p_def = (PropertyDefinition*)((cJSON_LightUserData*)prop_map_entry)->ptr;
                    if (p_def) {
                        free(p_def->name); free(p_def->c_type); free(p_def->setter);
                        free(p_def->widget_type_hint); free(p_def->style_part_default);
                        free(p_def->style_state_default); free(p_def);
                    }
                }
                prop_map_entry = prop_map_entry->next;
             }
        }
        cJSON_Delete(spec->properties_map);
    }

    // Free WidgetDefinition objects and their PropertyDefinition lists stored in widgets_map
    if (spec->widgets_map) {
        if (((cJSON*)spec->widgets_map)->type == cJSON_Object) { // Cast because widgets_map is HashTable* but used as cJSON*
            cJSON* widget_map_entry = ((cJSON*)spec->widgets_map)->child;
            while(widget_map_entry) {
                if (widget_map_entry->type == cJSON_LightUserData) {
                    WidgetDefinition* wd = (WidgetDefinition*)((cJSON_LightUserData*)widget_map_entry)->ptr;
                    if (wd) {
                        free(wd->name);
                        PropertyDefinitionNode* current_prop_node = wd->properties;
                        while(current_prop_node) {
                            PropertyDefinitionNode* next_prop_node = current_prop_node->next;
                            if (current_prop_node->prop) {
                                // Assuming PropertyDefinitions in WidgetDefinition lists are distinct copies
                                // and need to be freed here.
                                free(current_prop_node->prop->name); free(current_prop_node->prop->c_type);
                                free(current_prop_node->prop->setter); free(current_prop_node->prop->widget_type_hint);
                                free(current_prop_node->prop->style_part_default); free(current_prop_node->prop->style_state_default);
                                free(current_prop_node->prop);
                            }
                            free(current_prop_node);
                            current_prop_node = next_prop_node;
                        }
                        free(wd);
                    }
                }
                widget_map_entry = widget_map_entry->next;
            }
        }
        cJSON_Delete((cJSON*)spec->widgets_map);
    }
    free(spec);
}

// Helper function to free a linked list of PropertyDefinitionNodes
static void free_property_definition_list(PropertyDefinitionNode* head) {
    PropertyDefinitionNode* current = head;
    while (current) {
        PropertyDefinitionNode* next = current->next;
        if (current->prop) {
            free(current->prop->name);
            free(current->prop->c_type);
            free(current->prop->setter);
            free(current->prop->widget_type_hint);
            free(current->prop->style_part_default);
            free(current->prop->style_state_default);
            free(current->prop);
        }
        free(current);
        current = next;
    }
}

void api_spec_free(ApiSpec* spec) { // Re-listing api_spec_free to replace its entire body
    if (!spec) return;

    // Free global PropertyDefinition objects stored in properties_map
    if (spec->properties_map) {
        if (spec->properties_map->type == cJSON_Object) {
             cJSON* prop_map_entry = spec->properties_map->child;
             while(prop_map_entry) {
                cJSON* next_entry = prop_map_entry->next; // Get next before potential modification/free
                if (prop_map_entry->type == cJSON_LightUserData) {
                    PropertyDefinition* p_def = (PropertyDefinition*)((cJSON_LightUserData*)prop_map_entry)->ptr;
                    if (p_def) {
                        free(p_def->name); free(p_def->c_type); free(p_def->setter);
                        free(p_def->widget_type_hint); free(p_def->style_part_default);
                        free(p_def->style_state_default); free(p_def);
                    }
                }
                prop_map_entry = next_entry;
             }
        }
        cJSON_Delete(spec->properties_map);
    }

    // Free WidgetDefinition objects and their PropertyDefinition lists stored in widgets_map
    if (spec->widgets_map) {
        // Assuming widgets_map is a cJSON object where values are LightUserData pointing to WidgetDefinition
        cJSON* cjson_widgets_map = (cJSON*)spec->widgets_map;
        if (cjson_widgets_map->type == cJSON_Object) {
            cJSON* widget_map_entry = cjson_widgets_map->child;
            while(widget_map_entry) {
                cJSON* next_entry = widget_map_entry->next;
                if (widget_map_entry->type == cJSON_LightUserData) {
                    WidgetDefinition* wd = (WidgetDefinition*)((cJSON_LightUserData*)widget_map_entry)->ptr;
                    if (wd) {
                        free(wd->name);
                        free_property_definition_list(wd->properties); // Use helper
                        free(wd);
                    }
                }
                widget_map_entry = next_entry;
            }
        }
        cJSON_Delete(cjson_widgets_map);
    }

    // spec->constants and spec->enums are not owned by ApiSpec, they are part of the parsed cJSON document
    // which is freed in main.c. No need to free them here.
    // spec->functions_map is NULL and not used.

    free(spec);
}

const PropertyDefinition* api_spec_find_property(const ApiSpec* spec, const char* type_name, const char* prop_name) {
    if (!spec || !type_name || !prop_name) return NULL;

    const char* current_type_to_check = type_name;

    while (current_type_to_check != NULL) {
        // 1. Try to find in widgets_map (widget-specific definitions for current_type_to_check)
        if (spec->widgets_map) {
            cJSON* wd_wrapper = cJSON_GetObjectItem((cJSON*)spec->widgets_map, current_type_to_check);
            if (wd_wrapper && wd_wrapper->type == cJSON_LightUserData) {
                WidgetDefinition* wd = (WidgetDefinition*)((cJSON_LightUserData*)wd_wrapper)->ptr;
                if (wd) { // Ensure wd pointer is not null
                    PropertyDefinitionNode* current_prop_node = wd->properties;
                    while(current_prop_node) {
                        if (current_prop_node->prop && strcmp(current_prop_node->prop->name, prop_name) == 0) {
                            return current_prop_node->prop; // Found in this widget type's specific list
                        }
                        current_prop_node = current_prop_node->next;
                    }
                    // Not found in specific list, prepare to check inherited type
                    current_type_to_check = wd->inherits;
                    if (current_type_to_check && strlen(current_type_to_check) == 0) { // Treat empty inherits string as NULL
                        current_type_to_check = NULL;
                    }
                    continue; // Continue to check parent type or fallback to global if inherits is NULL
                }
            }
        }
        // If WidgetDefinition not found for current_type_to_check, or no spec->widgets_map,
        // or if current_type_to_check became NULL (no more inheritance).
        break;
    }

    // 2. Fallback to global properties_map if not found through widget-specific lists and inheritance
    if (spec->properties_map) {
        cJSON* prop_item_wrapper = cJSON_GetObjectItem(spec->properties_map, prop_name);
        if (prop_item_wrapper && prop_item_wrapper->type == cJSON_LightUserData) {
            const PropertyDefinition* p_def = (const PropertyDefinition*)((cJSON_LightUserData*)prop_item_wrapper)->ptr;
            if (p_def) {
                // Apply filtering based on the *original* requested type_name
                if (strcmp(type_name, "style") == 0) {
                    return p_def->is_style_prop ? p_def : NULL;
                }
                // For "obj" type, or if the property's hint matches the original type_name or "obj",
                // it can be considered a valid fallback.
                if (strcmp(type_name, "obj") == 0 && !p_def->is_style_prop) {
                    return p_def;
                }
                // If the property is globally defined, and its hint suggests it's for this type_name or a generic "obj" property
                if (p_def->widget_type_hint &&
                    (strcmp(p_def->widget_type_hint, type_name) == 0 || strcmp(p_def->widget_type_hint, "obj") == 0)) {
                    // And it's not a style property being misapplied to a non-style object type
                    if (strcmp(type_name, "style") != 0 && p_def->is_style_prop) {
                        return NULL; // Cannot apply a style-only global property to a non-style object this way
                    }
                    return p_def;
                }
                 // If we reached here via inheritance (current_type_to_check is different from original type_name)
                 // and original type was not "obj" or "style", we might be more restrictive.
                 // However, the current logic allows if widget_type_hint matches.
            }
        }
    }
    return NULL;
}

const cJSON* api_spec_get_constants(const ApiSpec* spec) {
    return spec ? spec->constants : NULL;
}

const cJSON* api_spec_get_enums(const ApiSpec* spec) {
    return spec ? spec->enums : NULL;
}

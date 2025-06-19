#include "api_spec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to safely strdup, returning NULL if input is NULL
static char* safe_strdup(const char* s) {
    return s ? strdup(s) : NULL;
}

// Forward declaration for helper
static void free_property_definition_list(PropertyDefinitionNode* head);
static const WidgetDefinition* api_spec_find_widget(const ApiSpec* spec, const char* widget_name); // New helper

ApiSpec* api_spec_parse(const cJSON* root_json) {
    if (!root_json) return NULL;

    ApiSpec* spec = (ApiSpec*)calloc(1, sizeof(ApiSpec));
    if (!spec) {
        perror("Failed to allocate ApiSpec");
        return NULL;
    }
    spec->global_properties_list_head = NULL;
    spec->widgets_list_head = NULL;
    spec->functions = NULL; // Initialize functions list head

    // --- Parse global "properties" definitions ---
    cJSON* global_props_json = cJSON_GetObjectItemCaseSensitive(root_json, "properties");
    if (cJSON_IsObject(global_props_json)) {
        cJSON* prop_item_json = NULL;
        cJSON_ArrayForEach(prop_item_json, global_props_json) {
            PropertyDefinition* p_def = (PropertyDefinition*)calloc(1, sizeof(PropertyDefinition));
            if (!p_def) { /* error handling */ api_spec_free(spec); return NULL; }
            p_def->name = safe_strdup(prop_item_json->string);
            cJSON* details = prop_item_json;
            p_def->c_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "type")));
            p_def->setter = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "setter")));
            p_def->widget_type_hint = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "widget_hint")));
            cJSON* style_args_json = cJSON_GetObjectItem(details, "style_args");
            if (cJSON_IsNumber(style_args_json)) p_def->num_style_args = style_args_json->valueint;
            p_def->style_part_default = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "style_part_default")));
            p_def->style_state_default = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(details, "style_state_default")));
            cJSON* is_style_json = cJSON_GetObjectItem(details, "is_style_prop");
            p_def->is_style_prop = cJSON_IsTrue(is_style_json);

            GlobalPropertyDefinitionNode* new_global_node = (GlobalPropertyDefinitionNode*)calloc(1, sizeof(GlobalPropertyDefinitionNode));
            if (!new_global_node) { /* error handling */ free(p_def->name); free(p_def); api_spec_free(spec); return NULL; }
            new_global_node->name = safe_strdup(p_def->name);
            new_global_node->prop_def = p_def;
            new_global_node->next = spec->global_properties_list_head;
            spec->global_properties_list_head = new_global_node;
        }
    } else {
        fprintf(stderr, "Warning: Global 'properties' section is missing or not an object in API spec.\n");
    }

    // --- Parse "widgets" section for WidgetDefinitions ---
    cJSON* widgets_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "widgets");
    if (cJSON_IsObject(widgets_json_obj)) {
        cJSON* widget_json_node = NULL;
        cJSON_ArrayForEach(widget_json_node, widgets_json_obj) {
            const char* widget_type_name = widget_json_node->string;
            WidgetDefinition* wd = (WidgetDefinition*)calloc(1, sizeof(WidgetDefinition));
            if (!wd) { /* error handling */ continue; }
            wd->name = safe_strdup(widget_type_name);
            wd->inherits = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(widget_json_node, "inherits")));
            wd->properties = NULL;

            cJSON* widget_specific_props_obj = cJSON_GetObjectItemCaseSensitive(widget_json_node, "properties");
            if (cJSON_IsObject(widget_specific_props_obj)) {
                cJSON* prop_detail_json = NULL;
                struct PropertyDefinitionNode** current_prop_node_tail = &wd->properties;
                cJSON_ArrayForEach(prop_detail_json, widget_specific_props_obj) {
                    const char* prop_key_name = prop_detail_json->string;
                    PropertyDefinition* pd = (PropertyDefinition*)calloc(1, sizeof(PropertyDefinition));
                    if (!pd) { /* error handling */ continue; }
                    pd->name = safe_strdup(prop_key_name);
                    cJSON* setter_item = cJSON_GetObjectItem(prop_detail_json, "setter");
                    if (cJSON_IsString(setter_item)) pd->setter = safe_strdup(setter_item->valuestring);
                    cJSON* type_item = cJSON_GetObjectItem(prop_detail_json, "type");
                    if (cJSON_IsString(type_item)) pd->c_type = safe_strdup(type_item->valuestring);
                    pd->widget_type_hint = safe_strdup(widget_type_name);
                    // ... (populate other pd fields: num_style_args, defaults, is_style_prop) ...
                    cJSON* style_args_item = cJSON_GetObjectItem(prop_detail_json, "style_args");
                    if (cJSON_IsNumber(style_args_item)) pd->num_style_args = style_args_item->valueint;
                    cJSON* part_default_item = cJSON_GetObjectItem(prop_detail_json, "style_part_default");
                    if (cJSON_IsString(part_default_item)) pd->style_part_default = safe_strdup(part_default_item->valuestring);
                    cJSON* state_default_item = cJSON_GetObjectItem(prop_detail_json, "style_state_default");
                    if (cJSON_IsString(state_default_item)) pd->style_state_default = safe_strdup(state_default_item->valuestring);
                    cJSON* is_style_item = cJSON_GetObjectItem(prop_detail_json, "is_style_prop");
                    if (cJSON_IsBool(is_style_item)) pd->is_style_prop = cJSON_IsTrue(is_style_item);

                    *current_prop_node_tail = (struct PropertyDefinitionNode*)calloc(1, sizeof(struct PropertyDefinitionNode));
                    if (!*current_prop_node_tail) { /* error handling */ /* free pd */ continue; }
                    (*current_prop_node_tail)->prop = pd;
                    (*current_prop_node_tail)->next = NULL;
                    current_prop_node_tail = &(*current_prop_node_tail)->next;
                }
            }
            WidgetMapNode* new_widget_node = (WidgetMapNode*)calloc(1, sizeof(WidgetMapNode));
            if(!new_widget_node){ /* error handling */ /* free wd */ continue; }
            new_widget_node->name = safe_strdup(wd->name);
            new_widget_node->widget = wd;
            new_widget_node->next = spec->widgets_list_head;
            spec->widgets_list_head = new_widget_node;
        }
    } else {
         fprintf(stderr, "Warning: 'widgets' section is missing or not an object in API spec. Widget-specific properties not parsed.\n");
    }

    spec->constants = cJSON_GetObjectItemCaseSensitive(root_json, "constants");
    spec->enums = cJSON_GetObjectItemCaseSensitive(root_json, "enums");
    // spec->functions needs parsing if FunctionMapNode is used
    spec->functions = NULL;

    return spec;
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

void api_spec_free(ApiSpec* spec) {
    if (!spec) return;

    // Free global properties
    GlobalPropertyDefinitionNode* current_global_prop = spec->global_properties_list_head;
    while (current_global_prop) {
        GlobalPropertyDefinitionNode* next_global_prop = current_global_prop->next;
        free(current_global_prop->name);
        if (current_global_prop->prop_def) {
            free(current_global_prop->prop_def->name);
            free(current_global_prop->prop_def->c_type);
            free(current_global_prop->prop_def->setter);
            free(current_global_prop->prop_def->widget_type_hint);
            free(current_global_prop->prop_def->style_part_default);
            free(current_global_prop->prop_def->style_state_default);
            free(current_global_prop->prop_def);
        }
        free(current_global_prop);
        current_global_prop = next_global_prop;
    }
    spec->global_properties_list_head = NULL;

    // Free widget definitions
    WidgetMapNode* current_widget_node = spec->widgets_list_head;
    while (current_widget_node) {
        WidgetMapNode* next_widget_node = current_widget_node->next;
        free(current_widget_node->name);
        if (current_widget_node->widget) {
            WidgetDefinition* wd = current_widget_node->widget;
            free(wd->name);
            free(wd->inherits);
            free_property_definition_list(wd->properties);
            free(wd);
        }
        free(current_widget_node);
        current_widget_node = next_widget_node;
    }
    spec->widgets_list_head = NULL;

    // Free function definitions (if FunctionMapNode and FunctionDefinition are defined and used)
    // struct FunctionMapNode* current_func_node = spec->functions;
    // while (current_func_node) {
    //     struct FunctionMapNode* next_func_node = current_func_node->next;
    //     if (current_func_node->func) {
    //         free(current_func_node->func->name); // Assuming FunctionDefinition has a name
    //         free(current_func_node->func->return_type); // Assuming FunctionDefinition has a return_type
    //         // Free other members of FunctionDefinition
    //         free(current_func_node->func);
    //     }
    //     free(current_func_node);
    //     current_func_node = next_func_node;
    // }
    // spec->functions = NULL;

    // spec->constants and spec->enums are NOT freed here.
    free(spec);
}

// (api_spec_find_property remains as previously defined, but will need updating later to use these new lists)
const PropertyDefinition* api_spec_find_property(const ApiSpec* spec, const char* type_name, const char* prop_name) {
    if (!spec || !type_name || !prop_name) return NULL;

    const char* current_type_to_check = type_name;

    while (current_type_to_check != NULL) {
        // This part needs to search spec->widgets_list_head
        WidgetMapNode* w_node = spec->widgets_list_head;
        while(w_node) {
            if (w_node->widget && strcmp(w_node->widget->name, current_type_to_check) == 0) {
                PropertyDefinitionNode* p_node = w_node->widget->properties;
                while(p_node) {
                    if (p_node->prop && strcmp(p_node->prop->name, prop_name) == 0) {
                        return p_node->prop;
                    }
                    p_node = p_node->next;
                }
                current_type_to_check = w_node->widget->inherits; // Prepare for next iteration
                if (current_type_to_check && strlen(current_type_to_check) == 0) current_type_to_check = NULL;
                goto next_widget_type_check; // Found widget, checked its props, now check parent or break
            }
            w_node = w_node->next;
        }
        // If widget type not found in list, or if current_type_to_check was NULL initially from inheritance
        current_type_to_check = NULL; // Stop inheritance search if type not in map

        next_widget_type_check:;
    }

    // Fallback to global properties_list_head
    GlobalPropertyDefinitionNode* gp_node = spec->global_properties_list_head;
    while(gp_node) {
        if (gp_node->prop_def && strcmp(gp_node->prop_def->name, prop_name) == 0) {
            const PropertyDefinition* p_def = gp_node->prop_def;
            // Apply original filtering logic for global properties
            if (strcmp(type_name, "style") == 0) {
                return p_def->is_style_prop ? p_def : NULL;
            }
            if (strcmp(type_name, "obj") == 0 && !p_def->is_style_prop) {
                return p_def;
            }
            if (p_def->widget_type_hint &&
                (strcmp(p_def->widget_type_hint, type_name) == 0 || strcmp(p_def->widget_type_hint, "obj") == 0)) {
                if (strcmp(type_name, "style") != 0 && p_def->is_style_prop) {
                    return NULL;
                }
                return p_def;
            }
            // If it's a global property and no specific type match, but it's requested by a non-"obj"/"style" type,
            // we might not want to return it unless widget_type_hint was very generic or matched.
            // The current logic is already quite permissive with widget_type_hint check.
        }
        gp_node = gp_node->next;
    }
    return NULL;
}


const cJSON* api_spec_get_constants(const ApiSpec* spec) {
    return spec ? spec->constants : NULL;
}

const cJSON* api_spec_get_enums(const ApiSpec* spec) {
    return spec ? spec->enums : NULL;
}

// Helper function to find a widget definition by its name
static const WidgetDefinition* api_spec_find_widget(const ApiSpec* spec, const char* widget_name) {
    if (!spec || !widget_name) return NULL;
    WidgetMapNode* current_wnode = spec->widgets_list_head;
    while (current_wnode) {
        if (current_wnode->name && strcmp(current_wnode->name, widget_name) == 0) {
            return current_wnode->widget;
        }
        current_wnode = current_wnode->next;
    }
    return NULL;
}

// (api_spec_find_property remains as previously defined, but will need updating later to use these new lists)
// The previous overwrite already included an updated version of api_spec_find_property.
// I will refine that version now.
const PropertyDefinition* api_spec_find_property(const ApiSpec* spec, const char* type_name, const char* prop_name) {
    if (!spec || !type_name || !prop_name) return NULL;

    const char* current_type_to_check = type_name;

    while (current_type_to_check != NULL && current_type_to_check[0] != '\0') { // Ensure not empty string
        const WidgetDefinition* widget_def = api_spec_find_widget(spec, current_type_to_check);
        if (widget_def) {
            PropertyDefinitionNode* p_node = widget_def->properties;
            while(p_node) {
                if (p_node->prop && p_node->prop->name && strcmp(p_node->prop->name, prop_name) == 0) {
                    return p_node->prop; // Found in widget-specific or inherited properties
                }
                p_node = p_node->next;
            }
            current_type_to_check = widget_def->inherits;
        } else {
            // If the specific type (or an inherited type) is not found in widgets_list_head,
            // it might be "obj" or "style" which are more like abstract types whose properties
            // might only live in the global list. Or it's an unknown type.
            // If current_type_to_check was the original type_name and it's not "obj" or "style",
            // then it's an error or unknown widget type.
            // If it's "obj" or "style", or if inheritance led to a type not explicitly in widgets_list_head,
            // we should then proceed to global lookup.
            // For simplicity, if a type in the chain isn't found, stop climbing that chain.
            break;
        }
    }

    // Fallback to global properties_list_head
    GlobalPropertyDefinitionNode* gp_node = spec->global_properties_list_head;
    while(gp_node) {
        // Check if the global property's name matches.
        // The gp_node->name is the key in the list (should match prop_def->name).
        // We should primarily match against gp_node->prop_def->name.
        if (gp_node->prop_def && gp_node->prop_def->name && strcmp(gp_node->prop_def->name, prop_name) == 0) {
            const PropertyDefinition* p_def = gp_node->prop_def;

            // Apply contextual filtering based on the *original* requested type_name
            if (strcmp(type_name, "style") == 0) {
                return p_def->is_style_prop ? p_def : NULL;
            }
            // For "obj" type, allow if it's not specifically a style-only property.
            if (strcmp(type_name, "obj") == 0) {
                if (!p_def->is_style_prop) return p_def;
                // If it IS a style prop, but "obj" is asking, it's likely for something like
                // lv_obj_set_style_local_radius, where "obj" is the type, but it takes style properties.
                // The PropertyDefinition's num_style_args might indicate this.
                // This part of the logic needs to be very robust.
                // For now, if type_name is "obj", we assume it's a general object property, not a style one.
                // This was the previous logic: if (strcmp(type_name, "obj") == 0 && !p_def->is_style_prop)
            }

            // For specific widget types (not "obj" or "style"):
            // If a global property is found, its widget_type_hint should ideally be considered.
            // A property hinted for "obj" can apply to any widget.
            // A property hinted for a specific type (e.g., "button") should apply to "button".
            if (p_def->widget_type_hint) {
                if (strcmp(p_def->widget_type_hint, type_name) == 0 || strcmp(p_def->widget_type_hint, "obj") == 0) {
                    // If it's a style property, ensure the target type_name is "style" or it's being applied appropriately
                    // (e.g. local style setter, which usually means the type_name for api_spec_find_property is the widget type itself, not "style")
                    if (p_def->is_style_prop && strcmp(type_name, "style") != 0) {
                        // This case means a global style property is being looked up for a non-style object type.
                        // This is valid for local styles like lv_obj_set_style_local_radius.
                        // The PropertyDefinition's num_style_args will be > 0 for these.
                        // So, just returning p_def here is likely correct.
                        return p_def;
                    }
                    if (!p_def->is_style_prop && strcmp(type_name, "style") == 0) {
                         return NULL; // Cannot apply non-style global prop to "style" type
                    }
                    return p_def;
                }
            } else if (strcmp(type_name, "obj") == 0 && !p_def->is_style_prop) {
                // If no hint, but type is "obj" and it's not a style prop, it's a general obj prop.
                return p_def;
            }
        }
        gp_node = gp_node->next;
    }
    return NULL;
}

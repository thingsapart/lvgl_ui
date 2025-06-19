#include "api_spec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to safely strdup, returning NULL if input is NULL
static char* safe_strdup(const char* s) {
    return s ? strdup(s) : NULL;
}

// Forward declarations for helpers
static void free_property_definition_list(PropertyDefinitionNode* head);
static const WidgetDefinition* api_spec_find_widget(const ApiSpec* spec, const char* widget_name);

ApiSpec* api_spec_parse(const cJSON* root_json) {
    if (!root_json) return NULL;

    ApiSpec* spec = (ApiSpec*)calloc(1, sizeof(ApiSpec));
    if (!spec) {
        perror("Failed to allocate ApiSpec");
        return NULL;
    }
    spec->widgets_list_head = NULL;
    spec->functions = NULL;
    // global_properties_list_head was removed from ApiSpec struct

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

                    cJSON* style_args_item = cJSON_GetObjectItem(prop_detail_json, "style_args");
                    if (cJSON_IsNumber(style_args_item)) pd->num_style_args = style_args_item->valueint;

                    cJSON* part_default_item = cJSON_GetObjectItem(prop_detail_json, "style_part_default");
                    if (cJSON_IsString(part_default_item)) pd->style_part_default = safe_strdup(part_default_item->valuestring);

                    cJSON* state_default_item = cJSON_GetObjectItem(prop_detail_json, "style_state_default");
                    if (cJSON_IsString(state_default_item)) pd->style_state_default = safe_strdup(state_default_item->valuestring);

                    cJSON* is_style_item = cJSON_GetObjectItem(prop_detail_json, "is_style_prop");
                    if (cJSON_IsBool(is_style_item)) pd->is_style_prop = cJSON_IsTrue(is_style_item);

                    *current_prop_node_tail = (struct PropertyDefinitionNode*)calloc(1, sizeof(struct PropertyDefinitionNode));
                    if (!*current_prop_node_tail) { /* error handling */ /* free pd */ free(pd->name); free(pd->setter); free(pd->c_type); free(pd->widget_type_hint); free(pd->style_part_default); free(pd->style_state_default); free(pd); continue; }
                    (*current_prop_node_tail)->prop = pd;
                    (*current_prop_node_tail)->next = NULL;
                    current_prop_node_tail = &(*current_prop_node_tail)->next;
                }
            }
            WidgetMapNode* new_widget_node = (WidgetMapNode*)calloc(1, sizeof(WidgetMapNode));
            if(!new_widget_node){ /* error handling */ /* free wd */ free_property_definition_list(wd->properties); free(wd->name); free(wd->inherits); free(wd); continue; }
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
    spec->functions = NULL;

    return spec;
}

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

    // Function list freeing would go here if functions were parsed
    // struct FunctionMapNode* current_func_node = spec->functions;
    // ... similar loop ...
    // spec->functions = NULL;

    free(spec);
}

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

const PropertyDefinition* api_spec_find_property(const ApiSpec* spec, const char* type_name, const char* prop_name) {
    if (!spec || !type_name || !prop_name) return NULL;

    const char* current_type_to_check = type_name;

    while (current_type_to_check != NULL && current_type_to_check[0] != '\0') {
        const WidgetDefinition* widget_def = api_spec_find_widget(spec, current_type_to_check);
        if (widget_def) {
            PropertyDefinitionNode* p_node = widget_def->properties;
            while(p_node) {
                if (p_node->prop && p_node->prop->name && strcmp(p_node->prop->name, prop_name) == 0) {
                    return p_node->prop;
                }
                p_node = p_node->next;
            }
            current_type_to_check = widget_def->inherits;
        } else {
            // If the type is not found in the explicit widget definitions (e.g. "obj" or "style" which might not be listed as widgets)
            // or if inheritance leads to a type not in the list, break and try global properties for some cases.
             if (strcmp(current_type_to_check, "obj") == 0 || strcmp(current_type_to_check, "style") == 0) {
                // These types might not be in widgets_list_head but can have properties (handled by global fallback)
                break;
            }
            // For other unknown types in inheritance chain, stop.
            return NULL;
        }
    }

    // If type_name itself was "obj" or "style", or if inheritance chain ended,
    // and property not found, it means it's not a widget-specific property.
    // No global property list to fall back to anymore.
    // However, "style" properties are conceptually global and identified by "is_style_prop".
    // And "obj" properties are also global.
    // The original api_spec.json had a global "properties" list.
    // If we assume all properties are now defined *only* under widgets, then this is the end.
    // But if there's an expectation that some "common" or "style" properties exist globally,
    // that logic is now missing.
    // The previous global fallback was removed. This function now ONLY finds properties defined
    // directly within a widget's definition or its ancestors' definitions.
    // This matches the change of removing global_properties_list_head.

    return NULL;
}


const cJSON* api_spec_get_constants(const ApiSpec* spec) {
    return spec ? spec->constants : NULL;
}

const cJSON* api_spec_get_enums(const ApiSpec* spec) {
    return spec ? spec->enums : NULL;
}

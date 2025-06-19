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
static WidgetDefinition* parse_widget_def(const char* def_name, const cJSON* def_json_node);


// Helper function to parse a widget/object definition from its cJSON node
static WidgetDefinition* parse_widget_def(const char* def_name, const cJSON* def_json_node) {
    if (!def_json_node || !cJSON_IsObject(def_json_node)) {
        fprintf(stderr, "Error: Invalid JSON node for definition '%s'.\n", def_name);
        return NULL;
    }

    WidgetDefinition* def = (WidgetDefinition*)calloc(1, sizeof(WidgetDefinition));
    if (!def) {
        perror("Failed to allocate WidgetDefinition");
        return NULL;
    }
    def->name = safe_strdup(def_name);
    def->inherits = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def_json_node, "inherits")));
    def->create = NULL;
    def->c_type = NULL;    // Initialize new field
    def->init_func = NULL; // Initialize new field
    def->properties = NULL;

    // Parse "create" field
    cJSON* create_item = cJSON_GetObjectItemCaseSensitive(def_json_node, "create");
    if (create_item && cJSON_IsString(create_item) && create_item->valuestring != NULL && create_item->valuestring[0] != '\0') {
        def->create = safe_strdup(create_item->valuestring);
    }

    // Parse "c_type" field
    cJSON* c_type_item = cJSON_GetObjectItemCaseSensitive(def_json_node, "c_type");
    if (c_type_item && cJSON_IsString(c_type_item) && c_type_item->valuestring != NULL && c_type_item->valuestring[0] != '\0') {
        def->c_type = safe_strdup(c_type_item->valuestring);
    }

    // Parse "init_func" field
    cJSON* init_func_item = cJSON_GetObjectItemCaseSensitive(def_json_node, "init");
    if (init_func_item && cJSON_IsString(init_func_item) && init_func_item->valuestring != NULL && init_func_item->valuestring[0] != '\0') {
        def->init_func = safe_strdup(init_func_item->valuestring);
    }

    cJSON* properties_obj = cJSON_GetObjectItemCaseSensitive(def_json_node, "properties");
    if (cJSON_IsObject(properties_obj)) {
        cJSON* prop_detail_json;
        struct PropertyDefinitionNode** current_prop_list_node = &def->properties;

        cJSON_ArrayForEach(prop_detail_json, properties_obj) {
            const char* prop_name_str = prop_detail_json->string;

            PropertyDefinition* pd = (PropertyDefinition*)calloc(1, sizeof(PropertyDefinition));
            if (!pd) {
                perror("Failed to allocate PropertyDefinition in parse_widget_def");
                continue;
            }
            pd->name = safe_strdup(prop_name_str);

            cJSON* setter_item = cJSON_GetObjectItem(prop_detail_json, "setter");
            if (cJSON_IsString(setter_item)) pd->setter = safe_strdup(setter_item->valuestring);

            cJSON* type_item = cJSON_GetObjectItem(prop_detail_json, "type");
            if (cJSON_IsString(type_item)) pd->c_type = safe_strdup(type_item->valuestring);

            pd->widget_type_hint = safe_strdup(def_name);

            cJSON* style_args_item = cJSON_GetObjectItem(prop_detail_json, "style_args");
            if (cJSON_IsNumber(style_args_item)) pd->num_style_args = style_args_item->valueint;

            cJSON* part_default_item = cJSON_GetObjectItem(prop_detail_json, "style_part_default");
            if (cJSON_IsString(part_default_item)) pd->style_part_default = safe_strdup(part_default_item->valuestring);

            cJSON* state_default_item = cJSON_GetObjectItem(prop_detail_json, "style_state_default");
            if (cJSON_IsString(state_default_item)) pd->style_state_default = safe_strdup(state_default_item->valuestring);

            cJSON* is_style_item = cJSON_GetObjectItem(prop_detail_json, "is_style_prop");
            pd->is_style_prop = cJSON_IsTrue(is_style_item);

            *current_prop_list_node = (struct PropertyDefinitionNode*)calloc(1, sizeof(struct PropertyDefinitionNode));
            if (!*current_prop_list_node) {
                perror("Failed to allocate PropertyDefinitionNode in parse_widget_def");
                free(pd->name); free(pd->setter); free(pd->c_type); free(pd->widget_type_hint);
                free(pd->style_part_default); free(pd->style_state_default); free(pd);
                continue;
            }

            (*current_prop_list_node)->prop = pd;
            (*current_prop_list_node)->next = NULL;
            current_prop_list_node = &(*current_prop_list_node)->next;
        }
    }
    return def;
}


ApiSpec* api_spec_parse(const cJSON* root_json) {
    if (!root_json) return NULL;

    ApiSpec* spec = (ApiSpec*)calloc(1, sizeof(ApiSpec));
    if (!spec) {
        perror("Failed to allocate ApiSpec");
        return NULL;
    }
    spec->widgets_list_head = NULL;
    spec->functions = NULL;

    // --- Parse Widgets ---
    cJSON* widgets_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "widgets");
    if (widgets_json_obj) {
        if (cJSON_IsObject(widgets_json_obj)) {
            cJSON* widget_json_item = NULL;
            cJSON_ArrayForEach(widget_json_item, widgets_json_obj) {
                const char* widget_type_name = widget_json_item->string;
                WidgetDefinition* wd = parse_widget_def(widget_type_name, widget_json_item); // Uses the helper
                if (wd) {
                    WidgetMapNode* new_wnode = (WidgetMapNode*)calloc(1, sizeof(WidgetMapNode));
                    if (new_wnode) {
                        new_wnode->name = safe_strdup(widget_type_name);
                        new_wnode->widget = wd;
                        new_wnode->next = spec->widgets_list_head;
                        spec->widgets_list_head = new_wnode;
                    } else {
                        fprintf(stderr, "Error: Failed to allocate WidgetMapNode for widget '%s'.\n", widget_type_name);
                        if (wd->name) free(wd->name);
                        if (wd->inherits) free(wd->inherits);
                        if (wd->create) free(wd->create);
                        free_property_definition_list(wd->properties);
                        free(wd);
                    }
                }
            }
        } else {
            fprintf(stderr, "Warning: 'widgets' section is not an object in API spec. Widget definitions not parsed.\n");
        }
    } else {
        fprintf(stderr, "Warning: 'widgets' section is missing in API spec. Widget definitions not parsed.\n");
    }

    // --- Parse Objects ---
    cJSON* objects_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "objects");
    if (objects_json_obj) {
        if (cJSON_IsObject(objects_json_obj)) {
            cJSON* object_json_item = NULL;
            cJSON_ArrayForEach(object_json_item, objects_json_obj) {
                const char* object_type_name = object_json_item->string;
                WidgetDefinition* od = parse_widget_def(object_type_name, object_json_item); // Uses the helper
                if (od) {
                    WidgetMapNode* new_onode = (WidgetMapNode*)calloc(1, sizeof(WidgetMapNode));
                    if (new_onode) {
                        new_onode->name = safe_strdup(object_type_name);
                        new_onode->widget = od;
                        new_onode->next = spec->widgets_list_head;
                        spec->widgets_list_head = new_onode;
                    } else {
                        fprintf(stderr, "Error: Failed to allocate WidgetMapNode for object '%s'.\n", object_type_name);
                        if (od->name) free(od->name);
                        if (od->inherits) free(od->inherits);
                        if (od->create) free(od->create);
                        free_property_definition_list(od->properties);
                        free(od);
                    }
                }
            }
        } else {
            fprintf(stderr, "Warning: 'objects' section is not an object in API spec. Object definitions not parsed.\n");
        }
    } else {
        fprintf(stderr, "Warning: 'objects' section is missing in API spec. Object definitions not parsed.\n");
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
            free(wd->create);
            if (wd->c_type) {
                free(wd->c_type);
            }
            if (wd->init_func) {
                free(wd->init_func);
            }
            free_property_definition_list(wd->properties);
            free(wd);
        }
        free(current_widget_node);
        current_widget_node = next_widget_node;
    }
    spec->widgets_list_head = NULL;
    free(spec);
}

const WidgetDefinition* api_spec_find_widget(const ApiSpec* spec, const char* widget_name) {
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
             if (strcmp(current_type_to_check, "obj") == 0 || strcmp(current_type_to_check, "style") == 0) {
                break;
            }
            return NULL;
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

const char* widget_get_create_func(const WidgetDefinition* widget) {
    return widget ? widget->create : NULL;
}

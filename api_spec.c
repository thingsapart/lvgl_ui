#include "api_spec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to safely strdup, returning NULL if input is NULL
static char* safe_strdup(const char* s) {
    return s ? strdup(s) : NULL;
}

static PropertyDefinition global_func_prop_def;
static char global_func_setter_name[128];
static char global_func_arg_type[64];

static PropertyDefinition global_prop_def_from_root;
static char global_prop_name_buf[128];
static char global_prop_c_type_buf[64];
static char global_prop_setter_buf[128];
static char global_prop_obj_setter_prefix_buf[128];
static char global_prop_widget_type_hint_buf[64];
static char global_prop_part_default_buf[64];
static char global_prop_state_default_buf[64];

// New static variables for method-derived properties
static PropertyDefinition method_prop_def;
static char method_setter_name[128];
static char method_arg_type[64];

static void free_property_definition_list(PropertyDefinitionNode* head);
static void free_function_arg_list(FunctionArg* head);
static void free_function_definition_list(FunctionMapNode* head);
static WidgetDefinition* parse_widget_def(const char* def_name, const cJSON* def_json_node);

static void free_function_arg_list(FunctionArg* head) {
    FunctionArg* current = head;
    while (current) {
        FunctionArg* next = current->next;
        free(current->type);
        free(current);
        current = next;
    }
}

static void free_function_definition_list(FunctionMapNode* head) {
    FunctionMapNode* current = head;
    while (current) {
        FunctionMapNode* next = current->next;
        free(current->name);
        if (current->func_def) {
            free(current->func_def->name);
            free(current->func_def->return_type);
            free_function_arg_list(current->func_def->args_head);
            free(current->func_def);
        }
        free(current);
        current = next;
    }
}

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
    def->c_type = NULL;
    def->init_func = NULL;
    def->properties = NULL;
    def->methods = NULL;    // ADDED: initialize methods list

    cJSON* create_item = cJSON_GetObjectItemCaseSensitive(def_json_node, "create");
    if (create_item && cJSON_IsString(create_item) && create_item->valuestring != NULL && create_item->valuestring[0] != '\0') {
        def->create = safe_strdup(create_item->valuestring);
    }

    cJSON* c_type_item = cJSON_GetObjectItemCaseSensitive(def_json_node, "c_type");
    if (c_type_item && cJSON_IsString(c_type_item) && c_type_item->valuestring != NULL && c_type_item->valuestring[0] != '\0') {
        def->c_type = safe_strdup(c_type_item->valuestring);
    }

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
            if (!pd) continue;

            pd->name = safe_strdup(prop_name_str);
            pd->setter = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(prop_detail_json, "setter")));
            pd->c_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(prop_detail_json, "type"))); // Use "type" from JSON
            pd->widget_type_hint = safe_strdup(def_name);
            pd->obj_setter_prefix = NULL;

            cJSON* style_args_item = cJSON_GetObjectItem(prop_detail_json, "style_args");
            if (!style_args_item) style_args_item = cJSON_GetObjectItem(prop_detail_json, "num_style_args");
            if (cJSON_IsNumber(style_args_item)) pd->num_style_args = style_args_item->valueint;
            else pd->num_style_args = 0;

            pd->style_part_default = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(prop_detail_json, "style_part_default")));
            pd->style_state_default = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(prop_detail_json, "style_state_default")));
            pd->is_style_prop = cJSON_IsTrue(cJSON_GetObjectItem(prop_detail_json, "is_style_prop"));
            pd->func_args = NULL; // Initialize new field

            *current_prop_list_node = (struct PropertyDefinitionNode*)calloc(1, sizeof(struct PropertyDefinitionNode));
            if (!*current_prop_list_node) {
                free(pd->name); free(pd->setter); free(pd->c_type); free(pd->widget_type_hint);
                free(pd->style_part_default); free(pd->style_state_default); free(pd);
                continue;
            }
            (*current_prop_list_node)->prop = pd;
            (*current_prop_list_node)->next = NULL;
            current_prop_list_node = &(*current_prop_list_node)->next;
        }
    }

    // Parse "methods"
    cJSON* methods_obj = cJSON_GetObjectItemCaseSensitive(def_json_node, "methods");
    if (cJSON_IsObject(methods_obj)) {
        cJSON* method_json_item;
        FunctionMapNode** current_method_node_ptr = &def->methods;
        cJSON_ArrayForEach(method_json_item, methods_obj) {
            const char* method_name_str = method_json_item->string;
            if (!cJSON_IsObject(method_json_item)) continue;

            FunctionDefinition* fd = (FunctionDefinition*)calloc(1, sizeof(FunctionDefinition));
            if (!fd) continue;
            fd->name = safe_strdup(method_name_str);
            fd->return_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(method_json_item, "return_type")));
            if(!fd->return_type) fd->return_type = safe_strdup("void");

            cJSON* args_array_json = cJSON_GetObjectItemCaseSensitive(method_json_item, "args");
            FunctionArg** current_arg_ptr = &fd->args_head;
            if (cJSON_IsArray(args_array_json)) {
                cJSON* arg_type_json_item = NULL;
                cJSON_ArrayForEach(arg_type_json_item, args_array_json) {
                    if (cJSON_IsString(arg_type_json_item)) {
                        FunctionArg* fa = (FunctionArg*)calloc(1, sizeof(FunctionArg));
                        if (!fa) break;
                        fa->type = safe_strdup(arg_type_json_item->valuestring);
                        *current_arg_ptr = fa;
                        current_arg_ptr = &fa->next;
                    }
                }
            }
            FunctionMapNode* new_mnode = (FunctionMapNode*)calloc(1, sizeof(FunctionMapNode));
            if (!new_mnode) {
                free(fd->name); free(fd->return_type); free_function_arg_list(fd->args_head); free(fd);
                continue;
            }
            new_mnode->name = safe_strdup(method_name_str);
            new_mnode->func_def = fd;
            new_mnode->next = NULL;
            *current_method_node_ptr = new_mnode;
            current_method_node_ptr = &(*current_method_node_ptr)->next;
        }
    }
    return def;
}

ApiSpec* api_spec_parse(const cJSON* root_json) {
    if (!root_json) return NULL;
    ApiSpec* spec = (ApiSpec*)calloc(1, sizeof(ApiSpec));
    if (!spec) return NULL;

    spec->widgets_list_head = NULL;
    spec->functions = NULL;

    cJSON* widgets_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "widgets");
    if (widgets_json_obj && cJSON_IsObject(widgets_json_obj)) {
        cJSON* widget_json_item = NULL;
        cJSON_ArrayForEach(widget_json_item, widgets_json_obj) {
            const char* widget_type_name = widget_json_item->string;
            WidgetDefinition* wd = parse_widget_def(widget_type_name, widget_json_item);
            if (wd) {
                WidgetMapNode* new_wnode = (WidgetMapNode*)calloc(1, sizeof(WidgetMapNode));
                if (new_wnode) {
                    new_wnode->name = safe_strdup(widget_type_name);
                    new_wnode->widget = wd;
                    new_wnode->next = spec->widgets_list_head;
                    spec->widgets_list_head = new_wnode;
                } else {
                    if (wd->name) free(wd->name); if (wd->inherits) free(wd->inherits); if (wd->create) free(wd->create);
                    free_property_definition_list(wd->properties); free(wd);
                }
            }
        }
    }

    cJSON* objects_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "objects");
    if (objects_json_obj && cJSON_IsObject(objects_json_obj)) {
        cJSON* object_json_item = NULL;
        cJSON_ArrayForEach(object_json_item, objects_json_obj) {
            const char* object_type_name = object_json_item->string;
            WidgetDefinition* od = parse_widget_def(object_type_name, object_json_item);
            if (od) {
                WidgetMapNode* new_onode = (WidgetMapNode*)calloc(1, sizeof(WidgetMapNode));
                if (new_onode) {
                    new_onode->name = safe_strdup(object_type_name);
                    new_onode->widget = od;
                    new_onode->next = spec->widgets_list_head;
                    spec->widgets_list_head = new_onode;
                } else {
                     if (od->name) free(od->name); if (od->inherits) free(od->inherits); if (od->create) free(od->create);
                    free_property_definition_list(od->properties); free(od);
                }
            }
        }
    }
    spec->constants = cJSON_GetObjectItemCaseSensitive(root_json, "constants");
    spec->enums = cJSON_GetObjectItemCaseSensitive(root_json, "enums");
    spec->global_properties_json_node = cJSON_GetObjectItemCaseSensitive(root_json, "properties");

    cJSON* functions_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "functions");
    if (functions_json_obj && cJSON_IsObject(functions_json_obj)) {
        cJSON* func_json_item = NULL;
        FunctionMapNode** current_func_node_ptr = &spec->functions;
        cJSON_ArrayForEach(func_json_item, functions_json_obj) {
            const char* func_name_str = func_json_item->string;
            if (!cJSON_IsObject(func_json_item)) continue;
            FunctionDefinition* fd = (FunctionDefinition*)calloc(1, sizeof(FunctionDefinition));
            if (!fd) continue;
            fd->name = safe_strdup(func_name_str);
            fd->return_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(func_json_item, "return_type")));
            if(!fd->return_type) fd->return_type = safe_strdup("void");

            cJSON* args_array_json = cJSON_GetObjectItemCaseSensitive(func_json_item, "args");
            FunctionArg** current_arg_ptr = &fd->args_head;
            if (cJSON_IsArray(args_array_json)) {
                cJSON* arg_type_json_item = NULL;
                cJSON_ArrayForEach(arg_type_json_item, args_array_json) {
                    if (cJSON_IsString(arg_type_json_item)) {
                        FunctionArg* fa = (FunctionArg*)calloc(1, sizeof(FunctionArg));
                        if (!fa) break;
                        fa->type = safe_strdup(arg_type_json_item->valuestring);
                        *current_arg_ptr = fa;
                        current_arg_ptr = &fa->next;
                    }
                }
            }
            FunctionMapNode* new_fnode = (FunctionMapNode*)calloc(1, sizeof(FunctionMapNode));
            if (!new_fnode) {
                free(fd->name); free(fd->return_type); free_function_arg_list(fd->args_head); free(fd);
                continue;
            }
            new_fnode->name = safe_strdup(func_name_str);
            new_fnode->func_def = fd;
            new_fnode->next = NULL;
            *current_func_node_ptr = new_fnode;
            current_func_node_ptr = &(*current_func_node_ptr)->next;
        }
    }
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
            free(current->prop->obj_setter_prefix);
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
            free(wd->name); free(wd->inherits); free(wd->create);
            if (wd->c_type) free(wd->c_type);
            if (wd->init_func) free(wd->init_func);
            free_property_definition_list(wd->properties);
            free_function_definition_list(wd->methods); // ADDED: free methods list
            free(wd);
        }
        free(current_widget_node);
        current_widget_node = next_widget_node;
    }
    spec->widgets_list_head = NULL;
    free_function_definition_list(spec->functions);
    spec->functions = NULL;
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
    char constructed_name[128]; // For lv_obj_<prop_name>

    // --- STEP 1: Iterate through widget type and its parents ---
    while (current_type_to_check != NULL && current_type_to_check[0] != '\0') {
        const WidgetDefinition* widget_def = api_spec_find_widget(spec, current_type_to_check);
        if (widget_def) {
            // 1.1 Check declared "properties"
            PropertyDefinitionNode* p_node = widget_def->properties;
            while(p_node) {
                if (p_node->prop && p_node->prop->name && strcmp(p_node->prop->name, prop_name) == 0) {
                    return p_node->prop; // Found in properties
                }
                p_node = p_node->next;
            }

            // 1.2 Check "methods" for prop_name
            FunctionMapNode* m_node = widget_def->methods;
            while(m_node) {
                if (m_node->name && strcmp(m_node->name, prop_name) == 0) {
                    // Found as a direct method. Construct PropertyDefinition.
                    memset(&method_prop_def, 0, sizeof(PropertyDefinition));
                    strncpy(method_setter_name, m_node->name, sizeof(method_setter_name) - 1);
                    method_setter_name[sizeof(method_setter_name) - 1] = '\0';

                    method_prop_def.name = (char*)prop_name;
                    method_prop_def.setter = method_setter_name;
                    if (m_node->func_def && m_node->func_def->args_head && m_node->func_def->args_head->next) { // Assuming first arg is obj instance
                        strncpy(method_arg_type, m_node->func_def->args_head->next->type, sizeof(method_arg_type) - 1);
                         method_arg_type[sizeof(method_arg_type) -1] = '\0';
                    } else {
                        strcpy(method_arg_type, "unknown"); // Placeholder
                    }
                    method_prop_def.c_type = method_arg_type;
                    method_prop_def.widget_type_hint = (char*)current_type_to_check;
                    method_prop_def.func_args = m_node->func_def ? m_node->func_def->args_head : NULL; // Set func_args
                    return &method_prop_def;
                }
                m_node = m_node->next;
            }

            // 1.3 Check "methods" for "lv_obj_" + prop_name
            snprintf(constructed_name, sizeof(constructed_name), "lv_obj_%s", prop_name);
            m_node = widget_def->methods;
            while(m_node) {
                if (m_node->name && strcmp(m_node->name, constructed_name) == 0) {
                    // Found as lv_obj_method. Construct PropertyDefinition.
                    memset(&method_prop_def, 0, sizeof(PropertyDefinition));
                    strncpy(method_setter_name, m_node->name, sizeof(method_setter_name) - 1);
                    method_setter_name[sizeof(method_setter_name) -1] = '\0';

                    method_prop_def.name = (char*)prop_name;
                    method_prop_def.setter = method_setter_name;
                    if (m_node->func_def && m_node->func_def->args_head && m_node->func_def->args_head->next) { // Assuming first arg is obj instance
                         strncpy(method_arg_type, m_node->func_def->args_head->next->type, sizeof(method_arg_type) - 1);
                         method_arg_type[sizeof(method_arg_type) -1] = '\0';
                    } else {
                        strcpy(method_arg_type, "unknown");
                    }
                    method_prop_def.c_type = method_arg_type;
                    method_prop_def.widget_type_hint = (char*)current_type_to_check;
                    method_prop_def.func_args = m_node->func_def ? m_node->func_def->args_head : NULL; // Set func_args
                    return &method_prop_def;
                }
                m_node = m_node->next;
            }
            current_type_to_check = widget_def->inherits; // Move to parent
        } else {
            break;
        }
    }

    // --- STEP 2: Search global functions ---
    FunctionMapNode* func_node = NULL;

    // 2.1 Check global functions for prop_name verbatim
    if (spec->functions) {
       func_node = spec->functions;
       while (func_node) {
           if (func_node->name && strcmp(func_node->name, prop_name) == 0) {
               memset(&global_func_prop_def, 0, sizeof(PropertyDefinition));
               strncpy(global_func_setter_name, func_node->name, sizeof(global_func_setter_name) - 1);
               global_func_setter_name[sizeof(global_func_setter_name) -1] = '\0';

               global_func_prop_def.name = (char*)prop_name;
               global_func_prop_def.setter = global_func_setter_name;
               if (func_node->func_def && func_node->func_def->args_head) {
                   bool first_arg_is_obj = (func_node->func_def->args_head->type && (strcmp(func_node->func_def->args_head->type, "lv_obj_t*") == 0 || strcmp(func_node->func_def->args_head->type, "lv_obj_t *") == 0));
                   FunctionArg* relevant_arg = first_arg_is_obj ? func_node->func_def->args_head->next : func_node->func_def->args_head;
                   if (relevant_arg && relevant_arg->type) {
                       strncpy(global_func_arg_type, relevant_arg->type, sizeof(global_func_arg_type) - 1);
                       global_func_arg_type[sizeof(global_func_arg_type)-1] = '\0';
                   } else {
                       strcpy(global_func_arg_type, "unknown");
                   }
               } else {
                   strcpy(global_func_arg_type, "unknown");
               }
               global_func_prop_def.c_type = global_func_arg_type;
               global_func_prop_def.widget_type_hint = (char*)type_name;
               global_func_prop_def.func_args = func_node->func_def ? func_node->func_def->args_head : NULL; // Set func_args
               return &global_func_prop_def;
           }
           func_node = func_node->next;
       }
    }

    // 2.2 Check global functions for "lv_obj_" + prop_name
    snprintf(constructed_name, sizeof(constructed_name), "lv_obj_%s", prop_name);
    if (spec->functions) {
       func_node = spec->functions;
       while (func_node) {
           if (func_node->name && strcmp(func_node->name, constructed_name) == 0) {
               memset(&global_func_prop_def, 0, sizeof(PropertyDefinition));
               strncpy(global_func_setter_name, func_node->name, sizeof(global_func_setter_name) - 1);
               global_func_setter_name[sizeof(global_func_setter_name)-1] = '\0';

               global_func_prop_def.name = (char*)prop_name;
               global_func_prop_def.setter = global_func_setter_name;
                if (func_node->func_def && func_node->func_def->args_head) {
                   bool first_arg_is_obj = (func_node->func_def->args_head->type && (strcmp(func_node->func_def->args_head->type, "lv_obj_t*") == 0 || strcmp(func_node->func_def->args_head->type, "lv_obj_t *") == 0));
                   FunctionArg* relevant_arg = first_arg_is_obj ? func_node->func_def->args_head->next : func_node->func_def->args_head;
                   if (relevant_arg && relevant_arg->type) {
                       strncpy(global_func_arg_type, relevant_arg->type, sizeof(global_func_arg_type) - 1);
                       global_func_arg_type[sizeof(global_func_arg_type)-1] = '\0';
                   } else {
                       strcpy(global_func_arg_type, "unknown");
                   }
               } else {
                   strcpy(global_func_arg_type, "unknown");
               }
               global_func_prop_def.c_type = global_func_arg_type;
               global_func_prop_def.widget_type_hint = (char*)type_name;
               global_func_prop_def.func_args = func_node->func_def ? func_node->func_def->args_head : NULL; // Set func_args
               return &global_func_prop_def;
           }
           func_node = func_node->next;
       }
    }

    return NULL; // Not found
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

// Basic implementation of api_spec_get_function_return_type
// For now, it returns "lv_obj_t*" for all function names.
// A more robust version would look up the function in spec->functions.
const char* api_spec_get_function_return_type(const ApiSpec* spec, const char* func_name) {
    if (!spec || !func_name) {
        return "lv_obj_t*"; // Default or error
    }

    // Iterate through the global functions first
    FunctionMapNode* current_func_node = spec->functions;
    while (current_func_node) {
        if (current_func_node->name && strcmp(current_func_node->name, func_name) == 0) {
            if (current_func_node->func_def && current_func_node->func_def->return_type) {
                return current_func_node->func_def->return_type;
            }
            break; // Found the function, but no return type info
        }
        current_func_node = current_func_node->next;
    }

    // If not found in global functions, iterate through widget methods (if applicable, though less common for this specific use case)
    // This part might be an over-optimization for now, as 'with obj = func()' usually implies global factory/constructor functions.
    // However, for completeness:
    WidgetMapNode* current_widget_node = spec->widgets_list_head;
    while (current_widget_node) {
        if (current_widget_node->widget && current_widget_node->widget->methods) {
            FunctionMapNode* current_method_node = current_widget_node->widget->methods;
            while (current_method_node) {
                if (current_method_node->name && strcmp(current_method_node->name, func_name) == 0) {
                     if (current_method_node->func_def && current_method_node->func_def->return_type) {
                        return current_method_node->func_def->return_type;
                    }
                    // Found the method, but no return type.
                    // Depending on policy, could return default or break.
                    return "lv_obj_t*"; // Default if found but no specific type
                }
                current_method_node = current_method_node->next;
            }
        }
        current_widget_node = current_widget_node->next;
    }


    // Default if not found or no return type specified
    //fprintf(stderr, "Warning: Function '%s' not found in API spec or has no return type, defaulting to lv_obj_t*.\n", func_name);
    return "lv_obj_t*";
}

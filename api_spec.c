#include "api_spec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Helper to safely strdup, returning NULL if input is NULL
static char* safe_strdup(const char* s) {
    return s ? strdup(s) : NULL;
}

// Static buffers for property definitions returned by api_spec_find_property
// This is not thread-safe and assumes properties are not kept long-term by the caller.
// Consider a more robust memory management strategy for a real application.
static PropertyDefinition global_func_prop_def;
static char global_func_setter_name[128];
static char global_func_arg_type[64];

// These seem unused based on current api_spec_find_property, but kept for now if they were intended for something.
// static PropertyDefinition global_prop_def_from_root;
// static char global_prop_name_buf[128];
// static char global_prop_c_type_buf[64];
// static char global_prop_setter_buf[128];
// static char global_prop_obj_setter_prefix_buf[128];
// static char global_prop_widget_type_hint_buf[64];
// static char global_prop_part_default_buf[64];
// static char global_prop_state_default_buf[64];

static PropertyDefinition method_prop_def;
static char method_setter_name[128];
static char method_arg_type[64];


static void free_property_definition_list(PropertyDefinitionNode* head);
static void free_function_arg_list(FunctionArg* head);
static void free_function_definition_list(FunctionMapNode* head);
static WidgetDefinition* parse_widget_def(const char* def_name, const cJSON* def_json_node, const ApiSpec* spec_for_context); // Added spec_for_context

static void free_function_arg_list(FunctionArg* head) {
    FunctionArg* current = head;
    while (current) {
        FunctionArg* next = current->next;
        // Assuming name, type, and enum_type_name are strdup'd const char* effectively
        free((void*)current->name);
        free((void*)current->type);
        free((void*)current->enum_type_name);
        free(current);
        current = next;
    }
}

static void free_function_definition_list(FunctionMapNode* head) {
    FunctionMapNode* current = head;
    while (current) {
        FunctionMapNode* next = current->next;
        free((void*)current->name); // Name of the node (key)
        if (current->func_def) {
            free((void*)current->func_def->name);
            free((void*)current->func_def->return_type);
            free_function_arg_list(current->func_def->args);
            free(current->func_def);
        }
        free(current);
        current = next;
    }
}
static PropertyDefinition* parse_property_def_from_json(const char* prop_name_str, const cJSON* prop_detail_json, const char* widget_def_name, const ApiSpec* spec_for_context) {
    PropertyDefinition* pd = (PropertyDefinition*)calloc(1, sizeof(PropertyDefinition));
    if (!pd) return NULL;

    pd->name = safe_strdup(prop_name_str);
    pd->setter = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(prop_detail_json, "setter")));
    pd->c_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(prop_detail_json, "type")));
    pd->enum_type_name = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(prop_detail_json, "enum_type_name")));
    pd->widget_type_hint = safe_strdup(widget_def_name); // Widget this prop is directly defined under
    pd->obj_setter_prefix = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(prop_detail_json, "obj_setter_prefix")));


    cJSON* style_args_item = cJSON_GetObjectItem(prop_detail_json, "style_args");
    if (!style_args_item) style_args_item = cJSON_GetObjectItem(prop_detail_json, "num_style_args"); // backward compat
    if (cJSON_IsNumber(style_args_item)) pd->num_style_args = style_args_item->valueint;
    else pd->num_style_args = 0;

    pd->style_part_default = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(prop_detail_json, "style_part_default")));
    pd->style_state_default = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(prop_detail_json, "style_state_default")));
    pd->is_style_prop = cJSON_IsTrue(cJSON_GetObjectItem(prop_detail_json, "is_style_prop"));

    // Parse func_args if present
    cJSON* func_args_json = cJSON_GetObjectItemCaseSensitive(prop_detail_json, "func_args");
    if (cJSON_IsArray(func_args_json)) {
        FunctionArg** current_arg_ptr = &pd->func_args;
        cJSON* arg_json = NULL;
        cJSON_ArrayForEach(arg_json, func_args_json) {
            if (cJSON_IsObject(arg_json)) {
                FunctionArg* fa = (FunctionArg*)calloc(1, sizeof(FunctionArg));
                if (!fa) break;
                fa->name = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json, "name")));
                fa->type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json, "type")));
                fa->enum_type_name = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json, "enum_type_name")));
                *current_arg_ptr = fa;
                current_arg_ptr = &fa->next;
            }
        }
    }
    return pd;
}


static WidgetDefinition* parse_widget_def(const char* def_name, const cJSON* def_json_node, const ApiSpec* spec_for_context) {
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
    def->create = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def_json_node, "create")));
    def->c_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def_json_node, "c_type")));
    def->init_func = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def_json_node, "init")));
    def->json_type_override = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(def_json_node, "json_type_override")));


    cJSON* properties_obj = cJSON_GetObjectItemCaseSensitive(def_json_node, "properties");
    if (cJSON_IsObject(properties_obj)) {
        cJSON* prop_detail_json;
        PropertyDefinitionNode** current_prop_list_node = &def->properties;
        cJSON_ArrayForEach(prop_detail_json, properties_obj) {
            const char* prop_name_str = prop_detail_json->string;
            PropertyDefinition* pd = parse_property_def_from_json(prop_name_str, prop_detail_json, def_name, spec_for_context);
            if (pd) {
                *current_prop_list_node = (PropertyDefinitionNode*)calloc(1, sizeof(PropertyDefinitionNode));
                 if (!*current_prop_list_node) { free_property_definition_list(def->properties); def->properties = NULL; /* free pd too */ free((void*)pd->name); free((void*)pd->c_type); free((void*)pd->enum_type_name); free((void*)pd->setter); free((void*)pd->widget_type_hint); free((void*)pd->obj_setter_prefix); free((void*)pd->style_part_default); free((void*)pd->style_state_default); free_function_arg_list(pd->func_args); free(pd); break; } // out of mem
                (*current_prop_list_node)->prop = pd;
                (*current_prop_list_node)->next = NULL;
                current_prop_list_node = &(*current_prop_list_node)->next;
            }
        }
    }

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
            if(!fd->return_type) fd->return_type = safe_strdup("void"); // Default return type
            fd->num_args = 0;
            fd->min_args = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(method_json_item, "min_args"));


            cJSON* args_array_json = cJSON_GetObjectItemCaseSensitive(method_json_item, "args");
            FunctionArg** current_arg_ptr = &fd->args;
            if (cJSON_IsArray(args_array_json)) {
                cJSON* arg_json_item = NULL;
                cJSON_ArrayForEach(arg_json_item, args_array_json) {
                    if (cJSON_IsObject(arg_json_item)) { // Args are objects with name, type, enum_type_name
                        FunctionArg* fa = (FunctionArg*)calloc(1, sizeof(FunctionArg));
                        if (!fa) break;
                        fa->name = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "name")));
                        fa->type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "type")));
                        fa->enum_type_name = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "enum_type_name")));
                        *current_arg_ptr = fa;
                        current_arg_ptr = &fa->next;
                        fd->num_args++;
                    }
                }
            }
            FunctionMapNode* new_mnode = (FunctionMapNode*)calloc(1, sizeof(FunctionMapNode));
            if (!new_mnode) {
                free((void*)fd->name); free((void*)fd->return_type); free_function_arg_list(fd->args); free(fd);
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
    spec->functions_list_head = NULL;

    cJSON* widgets_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "widgets");
    if (widgets_json_obj && cJSON_IsObject(widgets_json_obj)) {
        cJSON* widget_json_item = NULL;
        cJSON_ArrayForEach(widget_json_item, widgets_json_obj) {
            const char* widget_type_name = widget_json_item->string;
            WidgetDefinition* wd = parse_widget_def(widget_type_name, widget_json_item, spec);
            if (wd) {
                WidgetMapNode* new_wnode = (WidgetMapNode*)calloc(1, sizeof(WidgetMapNode));
                if (new_wnode) {
                    new_wnode->name = safe_strdup(widget_type_name);
                    new_wnode->widget = wd;
                    new_wnode->next = spec->widgets_list_head;
                    spec->widgets_list_head = new_wnode;
                } else { // Failed to alloc node for list
                    free((void*)wd->name); free((void*)wd->inherits); free((void*)wd->create); free((void*)wd->c_type); free((void*)wd->init_func); free((void*)wd->json_type_override);
                    free_property_definition_list(wd->properties); free_function_definition_list(wd->methods); free(wd);
                }
            }
        }
    }

    cJSON* objects_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "objects");
     if (objects_json_obj && cJSON_IsObject(objects_json_obj)) {
        cJSON* object_json_item = NULL;
        cJSON_ArrayForEach(object_json_item, objects_json_obj) {
            const char* object_type_name = object_json_item->string;
            WidgetDefinition* od = parse_widget_def(object_type_name, object_json_item, spec); // Use same parser
            if (od) {
                WidgetMapNode* new_onode = (WidgetMapNode*)calloc(1, sizeof(WidgetMapNode));
                if (new_onode) {
                    new_onode->name = safe_strdup(object_type_name);
                    new_onode->widget = od; // Store in same list as widgets
                    new_onode->next = spec->widgets_list_head;
                    spec->widgets_list_head = new_onode;
                } else {
                    free((void*)od->name); free((void*)od->inherits); free((void*)od->create); free((void*)od->c_type); free((void*)od->init_func); free((void*)od->json_type_override);
                    free_property_definition_list(od->properties); free_function_definition_list(od->methods); free(od);
                }
            }
        }
    }
    spec->constants_json_node = cJSON_GetObjectItemCaseSensitive(root_json, "constants");
    spec->enums_json_node = cJSON_GetObjectItemCaseSensitive(root_json, "enums");
    spec->global_properties_json_node = cJSON_GetObjectItemCaseSensitive(root_json, "properties"); // For global properties if any

    cJSON* functions_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "functions");
    if (functions_json_obj && cJSON_IsObject(functions_json_obj)) {
        cJSON* func_json_item = NULL;
        FunctionMapNode** current_func_node_ptr = &spec->functions_list_head;
        cJSON_ArrayForEach(func_json_item, functions_json_obj) {
            const char* func_name_str = func_json_item->string;
            if (!cJSON_IsObject(func_json_item)) continue;
            FunctionDefinition* fd = (FunctionDefinition*)calloc(1, sizeof(FunctionDefinition));
            if (!fd) continue;
            fd->name = safe_strdup(func_name_str);
            fd->return_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(func_json_item, "return_type")));
            if(!fd->return_type) fd->return_type = safe_strdup("void");
            fd->num_args = 0;
            fd->min_args = cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(func_json_item, "min_args"));


            cJSON* args_array_json = cJSON_GetObjectItemCaseSensitive(func_json_item, "args");
            FunctionArg** current_arg_ptr = &fd->args;
            if (cJSON_IsArray(args_array_json)) {
                cJSON* arg_json_item = NULL;
                cJSON_ArrayForEach(arg_json_item, args_array_json) {
                     if (cJSON_IsObject(arg_json_item)) { // Args are objects
                        FunctionArg* fa = (FunctionArg*)calloc(1, sizeof(FunctionArg));
                        if (!fa) break;
                        fa->name = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "name")));
                        fa->type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "type")));
                        fa->enum_type_name = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "enum_type_name")));
                        *current_arg_ptr = fa;
                        current_arg_ptr = &fa->next;
                        fd->num_args++;
                    }
                }
            }
            FunctionMapNode* new_fnode = (FunctionMapNode*)calloc(1, sizeof(FunctionMapNode));
            if (!new_fnode) {
                free((void*)fd->name); free((void*)fd->return_type); free_function_arg_list(fd->args); free(fd);
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
            PropertyDefinition* p = current->prop;
            free((void*)p->name);
            free((void*)p->c_type);
            free((void*)p->enum_type_name);
            free((void*)p->setter);
            free((void*)p->widget_type_hint);
            free((void*)p->obj_setter_prefix);
            free((void*)p->style_part_default);
            free((void*)p->style_state_default);
            free_function_arg_list(p->func_args);
            free(p);
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
        free((void*)current_widget_node->name);
        if (current_widget_node->widget) {
            WidgetDefinition* wd = current_widget_node->widget;
            free((void*)wd->name); free((void*)wd->inherits); free((void*)wd->create);
            free((void*)wd->c_type); free((void*)wd->init_func); free((void*)wd->json_type_override);
            free_property_definition_list(wd->properties);
            free_function_definition_list(wd->methods);
            free(wd);
        }
        free(current_widget_node);
        current_widget_node = next_widget_node;
    }
    spec->widgets_list_head = NULL; // Not strictly necessary as spec is freed next
    free_function_definition_list(spec->functions_list_head);
    spec->functions_list_head = NULL;
    // Note: spec->constants, spec->enums, spec->global_properties_json_node are NOT freed here
    // as they are owned by the cJSON object that was passed to api_spec_parse.
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
            // Check methods if not in direct properties (using static buffers for return)
            FunctionMapNode* m_node = widget_def->methods;
            while(m_node) {
                if (m_node->name && strcmp(m_node->name, prop_name) == 0) {
                    memset(&method_prop_def, 0, sizeof(PropertyDefinition));
                    strncpy(method_setter_name, m_node->name, sizeof(method_setter_name) - 1);
                    method_setter_name[sizeof(method_setter_name) - 1] = '\0';
                    method_prop_def.name = (char*)prop_name; // This is safe as prop_name is input arg
                    method_prop_def.setter = method_setter_name;
                    if (m_node->func_def && m_node->func_def->args && m_node->func_def->args->next) { // 1st arg is obj, 2nd is value
                        strncpy(method_arg_type, m_node->func_def->args->next->type, sizeof(method_arg_type) - 1);
                        method_arg_type[sizeof(method_arg_type) -1] = '\0';
                        method_prop_def.enum_type_name = m_node->func_def->args->next->enum_type_name;
                    } else {
                        strcpy(method_arg_type, "unknown");
                    }
                    method_prop_def.c_type = method_arg_type;
                    method_prop_def.widget_type_hint = (char*)current_type_to_check;
                    method_prop_def.func_args = m_node->func_def ? m_node->func_def->args : NULL;
                    return &method_prop_def;
                }
                m_node = m_node->next;
            }
            current_type_to_check = widget_def->inherits;
        } else {
            break;
        }
    }

    // Fallback to global functions for properties not found in widget/object defs
    // This uses a static buffer for the PropertyDefinition, not thread-safe.
    const FunctionDefinition* global_func_def = api_spec_get_function(spec, prop_name);
    if (global_func_def) {
        memset(&global_func_prop_def, 0, sizeof(PropertyDefinition));
        strncpy(global_func_setter_name, global_func_def->name, sizeof(global_func_setter_name) - 1);
        global_func_setter_name[sizeof(global_func_setter_name)-1] = '\0';
        global_func_prop_def.name = (char*)prop_name; // Safe, input arg
        global_func_prop_def.setter = global_func_setter_name;

        if (global_func_def->args) { // Assuming first arg is target, second is value if applicable
            FunctionArg* val_arg = global_func_def->args->next ? global_func_def->args->next : global_func_def->args;
             if (val_arg && strcmp(val_arg->type, "lv_obj_t*") != 0) { // If it's not the object itself
                global_func_prop_def.c_type = val_arg->type;
                global_func_prop_def.enum_type_name = val_arg->enum_type_name;
             } else if (global_func_def->args->type && strcmp(global_func_def->args->type, "lv_obj_t*") != 0) {
                global_func_prop_def.c_type = global_func_def->args->type;
                global_func_prop_def.enum_type_name = global_func_def->args->enum_type_name;
             } else {
                global_func_prop_def.c_type = "unknown";
             }
        } else {
            global_func_prop_def.c_type = "unknown";
        }
        global_func_prop_def.widget_type_hint = (char*)type_name;
        global_func_prop_def.func_args = global_func_def->args;
        return &global_func_prop_def;
    }
    return NULL;
}

const cJSON* api_spec_get_constants(const ApiSpec* spec) {
    return spec ? spec->constants_json_node : NULL;
}

const cJSON* api_spec_get_enums(const ApiSpec* spec) {
    return spec ? spec->enums_json_node : NULL;
}

const char* widget_get_create_func(const WidgetDefinition* widget) {
    return widget ? widget->create : NULL;
}

const char* api_spec_get_function_return_type(const ApiSpec* spec, const char* func_name) {
    const FunctionDefinition* func_def = api_spec_get_function(spec, func_name);
    if (func_def && func_def->return_type) {
        return func_def->return_type;
    }
    // Default or error if not found, or if found but no return type specified
    //fprintf(stderr, "Warning: Function '%s' not found or no return type in API spec, defaulting.\n", func_name);
    return "lv_obj_t*"; // A common default, but might not be accurate.
}

// --- New function implementations ---

const FunctionDefinition* api_spec_get_function(const ApiSpec* spec, const char* func_name) {
    if (!spec || !func_name) return NULL;
    FunctionMapNode* current_fnode = spec->functions_list_head;
    while (current_fnode) {
        if (current_fnode->name && strcmp(current_fnode->name, func_name) == 0) {
            return current_fnode->func_def;
        }
        current_fnode = current_fnode->next;
    }
    // If not in global functions, check widget methods (though less common for direct lookup like this)
    // This part might be overly broad if func_name isn't widget-qualified.
    // For simplicity, this example only checks global functions.
    // A more complex system might check methods of a 'current_widget_context'.
    return NULL;
}

bool api_spec_is_enum_value(const ApiSpec* spec, const char* value_str) {
    if (!spec || !value_str || !spec->enums_json_node) return false;
    const cJSON* enum_type_json = NULL;
    cJSON_ArrayForEach(enum_type_json, spec->enums_json_node) { // Iterate over each enum type object
        // const char* enum_name = cJSON_GetObjectItem(enum_type_json, "name")->valuestring; // Not needed for this check
        const cJSON* members = cJSON_GetObjectItem(enum_type_json, "members");
        if (cJSON_IsArray(members)) {
            const cJSON* member_json = NULL;
            cJSON_ArrayForEach(member_json, members) {
                if (cJSON_IsString(member_json) && strcmp(value_str, member_json->valuestring) == 0) {
                    return true;
                }
            }
        } else if (cJSON_IsObject(members)) { // Handle enums where members are key-value (name:value)
             cJSON* member_item = NULL;
             cJSON_ArrayForEach(member_item, members) { // members is an object here
                 if (strcmp(value_str, member_item->string) == 0) { // value_str matches member name
                     return true;
                 }
             }
        }
    }
    return false;
}

const cJSON* api_spec_get_enum(const ApiSpec* spec, const char* enum_name) {
    if (!spec || !enum_name || !spec->enums_json_node) return NULL;
    const cJSON* enum_type_json = NULL;
    cJSON_ArrayForEach(enum_type_json, spec->enums_json_node) {
        const cJSON* name_item = cJSON_GetObjectItem(enum_type_json, "name");
        if (name_item && cJSON_IsString(name_item) && strcmp(enum_name, name_item->valuestring) == 0) {
            return cJSON_GetObjectItem(enum_type_json, "members"); // Return the members array/object
        }
    }
    return NULL;
}

bool api_spec_is_constant(const ApiSpec* spec, const char* key) {
    if (!spec || !key || !spec->constants_json_node) return false;
    if (cJSON_GetObjectItem(spec->constants_json_node, key)) {
        return true;
    }
    return false;
}

bool api_spec_is_enum_type(const ApiSpec* spec, const char* type_name) {
    if (!spec || !type_name || !spec->enums_json_node) return false;
    const cJSON* enum_type_json = NULL;
    cJSON_ArrayForEach(enum_type_json, spec->enums_json_node) {
        const cJSON* name_item = cJSON_GetObjectItem(enum_type_json, "name");
        if (name_item && cJSON_IsString(name_item) && strcmp(type_name, name_item->valuestring) == 0) {
            return true;
        }
    }
    return false;
}

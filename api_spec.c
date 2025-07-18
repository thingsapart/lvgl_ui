#include "api_spec.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include "utils.h"

// Helper to safely strdup, returning NULL if input is NULL
static char* safe_strdup(const char* s) {
    return s ? strdup(s) : NULL;
}

// Helper to create a heap-allocated PropertyDefinition from a function definition.
// This is used to synthesize properties from methods or global functions.
// The caller is responsible for freeing the returned pointer.
static PropertyDefinition* _create_prop_from_func(const char* prop_name, const FunctionDefinition* func_def, const char* widget_type_hint) {
    if (!prop_name || !func_def) return NULL;

    PropertyDefinition* pd = (PropertyDefinition*)calloc(1, sizeof(PropertyDefinition));
    if (!pd) render_abort("Failed to allocate synthesized PropertyDefinition");

    pd->is_heap_allocated = true; // Mark for freeing by caller
    pd->name = safe_strdup(prop_name);
    pd->setter = safe_strdup(func_def->name);
    pd->widget_type_hint = safe_strdup(widget_type_hint);
    pd->func_args = func_def->args_head; // This is a reference, not a copy

    // Determine the C type and expected enum type of the property's value.
    // This is usually the second argument of the function (after the target object).
    const FunctionArg* value_arg = NULL;
    if (func_def->args_head) {
        bool first_arg_is_target = (func_def->args_head->type &&
                                   (strstr(func_def->args_head->type, "lv_obj_t*") ||
                                    strstr(func_def->args_head->type, "lv_style_t*")));
        if (first_arg_is_target) {
            value_arg = func_def->args_head->next;
        } else {
            // This handles functions that don't take a target obj, like lv_color_hex
            value_arg = func_def->args_head;
        }
    }

    if (value_arg) {
        pd->c_type = safe_strdup(value_arg->type);
        pd->expected_enum_type = safe_strdup(value_arg->expected_enum_type);
    } else {
        pd->c_type = safe_strdup("unknown");
        pd->expected_enum_type = NULL;
    }

    return pd;
}


static void free_property_definition_list(PropertyDefinitionNode* head);
static void free_function_arg_list(FunctionArg* head);
static void free_function_definition_list(FunctionMapNode* head);
// Pass ApiSpec to allow enum lookup for function arguments
static WidgetDefinition* parse_widget_def(const char* def_name, const cJSON* def_json_node, const ApiSpec* spec_for_enum_lookup);
static char* strip_comments_and_trim(const char* input);


// Helper to clean up a constant value string from the JSON spec.
// It removes C-style comments and trims leading/trailing whitespace.
// The caller is responsible for freeing the returned string.
static char* strip_comments_and_trim(const char* input) {
    if (!input) return NULL;
    char* buffer = strdup(input);
    if (!buffer) return NULL;

    // Find and terminate at comments
    char* comment_start = strstr(buffer, "/*");
    if (comment_start) {
        *comment_start = '\0';
    }
    comment_start = strstr(buffer, "//");
    if (comment_start) {
        *comment_start = '\0';
    }

    // Trim trailing whitespace
    char* end = buffer + strlen(buffer) - 1;
    while (end >= buffer && isspace((unsigned char)*end)) {
        *end-- = '\0';
    }

    // Trim leading whitespace
    char* start = buffer;
    while (*start && isspace((unsigned char)*start)) {
        start++;
    }

    char* result = strdup(start);
    free(buffer);
    return result;
}


static void free_function_arg_list(FunctionArg* head) {
    FunctionArg* current = head;
    while (current) {
        FunctionArg* next = current->next;
        free(current->type);
        free(current->name); // Free the name if allocated
        free(current->expected_enum_type); // Free the expected_enum_type if allocated
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

static WidgetDefinition* parse_widget_def(const char* def_name, const cJSON* def_json_node, const ApiSpec* spec_for_enum_lookup) {
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
            pd->is_style_prop = cJSON_IsTrue(cJSON_GetObjectItem(prop_detail_json, "is_style_prop"));
            pd->func_args = NULL; // Initialize new field
            pd->expected_enum_type = NULL; // Initialize new field
            pd->is_heap_allocated = false; // Part of the main spec, not heap allocated per-call

            cJSON* expected_enum_type_json = cJSON_GetObjectItem(prop_detail_json, "expected_enum_type");
            if (cJSON_IsString(expected_enum_type_json) && expected_enum_type_json->valuestring != NULL) {
                pd->expected_enum_type = safe_strdup(expected_enum_type_json->valuestring);
            } else {
                pd->expected_enum_type = NULL;
            }

            *current_prop_list_node = (struct PropertyDefinitionNode*)calloc(1, sizeof(struct PropertyDefinitionNode));
            if (!*current_prop_list_node) {
                free(pd->name); free(pd->setter); free(pd->c_type); free(pd->widget_type_hint);
                free(pd->expected_enum_type);
                // func_args not owned by pd here
                free(pd);
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
                cJSON* arg_json_item = NULL;
                cJSON_ArrayForEach(arg_json_item, args_array_json) {
                    FunctionArg* fa = (FunctionArg*)calloc(1, sizeof(FunctionArg));
                    if (!fa) break;
                    fa->name = NULL; // Initialize
                    fa->expected_enum_type = NULL; // Initialize

                    if (cJSON_IsString(arg_json_item)) {
                        fa->type = safe_strdup(arg_json_item->valuestring);
                    } else if (cJSON_IsObject(arg_json_item)) {
                        fa->type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "type")));
                        fa->name = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "name")));
                        fa->expected_enum_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "expected_enum_type")));
                    }

                    if (!fa->expected_enum_type && fa->type && spec_for_enum_lookup && spec_for_enum_lookup->enums) {
                         if (cJSON_GetObjectItem(spec_for_enum_lookup->enums, fa->type)) {
                            fa->expected_enum_type = safe_strdup(fa->type);
                        }
                    }

                    *current_arg_ptr = fa;
                    current_arg_ptr = &fa->next;
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
            WidgetDefinition* wd = parse_widget_def(widget_type_name, widget_json_item, spec); // Pass spec here
            if (wd) {
                WidgetMapNode* new_wnode = (WidgetMapNode*)calloc(1, sizeof(WidgetMapNode));
                if (new_wnode) {
                    new_wnode->name = safe_strdup(widget_type_name);
                    new_wnode->widget = wd;
                    new_wnode->next = spec->widgets_list_head;
                    spec->widgets_list_head = new_wnode;
                } else {
                    if (wd->name) { free(wd->name); }
                    if (wd->inherits) { free(wd->inherits); }
                    if (wd->create) { free(wd->create); }
                    free_property_definition_list(wd->properties);
                    free(wd);
                }
            }
        }
    }

    cJSON* objects_json_obj = cJSON_GetObjectItemCaseSensitive(root_json, "objects");
    if (objects_json_obj && cJSON_IsObject(objects_json_obj)) {
        cJSON* object_json_item = NULL;
        cJSON_ArrayForEach(object_json_item, objects_json_obj) {
            const char* object_type_name = object_json_item->string;
            WidgetDefinition* od = parse_widget_def(object_type_name, object_json_item, spec); // Pass spec here
            if (od) {
                WidgetMapNode* new_onode = (WidgetMapNode*)calloc(1, sizeof(WidgetMapNode));
                if (new_onode) {
                    new_onode->name = safe_strdup(object_type_name);
                    new_onode->widget = od;
                    new_onode->next = spec->widgets_list_head;
                    spec->widgets_list_head = new_onode;
                } else {
                     if (od->name) { free(od->name); }
                     if (od->inherits) { free(od->inherits); }
                     if (od->create) { free(od->create); }
                    free_property_definition_list(od->properties);
                    free(od);
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
                cJSON* arg_json_item = NULL;
                cJSON_ArrayForEach(arg_json_item, args_array_json) {
                    FunctionArg* fa = (FunctionArg*)calloc(1, sizeof(FunctionArg));
                    if (!fa) break;
                    fa->name = NULL;
                    fa->expected_enum_type = NULL;

                    if (cJSON_IsString(arg_json_item)) {
                        fa->type = safe_strdup(arg_json_item->valuestring);
                    } else if (cJSON_IsObject(arg_json_item)) {
                        fa->type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "type")));
                        fa->name = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "name")));
                        fa->expected_enum_type = safe_strdup(cJSON_GetStringValue(cJSON_GetObjectItem(arg_json_item, "expected_enum_type")));
                    }

                    if (!fa->expected_enum_type && fa->type && spec->enums) { // Use spec->enums here
                         if (cJSON_GetObjectItem(spec->enums, fa->type)) {
                            fa->expected_enum_type = safe_strdup(fa->type);
                        }
                    }

                    *current_arg_ptr = fa;
                    current_arg_ptr = &fa->next;
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
            free(current->prop->expected_enum_type);
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
    char constructed_name[128];

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
                    // Found a method matching the property name. Synthesize a PropertyDefinition.
                    return _create_prop_from_func(prop_name, m_node->func_def, current_type_to_check);
                }
                m_node = m_node->next;
            }

            // 1.3 Check "methods" for "lv_obj_" + prop_name
            snprintf(constructed_name, sizeof(constructed_name), "lv_obj_%s", prop_name);
            m_node = widget_def->methods;
            while(m_node) {
                if (m_node->name && strcmp(m_node->name, constructed_name) == 0) {
                    return _create_prop_from_func(prop_name, m_node->func_def, current_type_to_check);
                }
                m_node = m_node->next;
            }
            current_type_to_check = widget_def->inherits; // Move to parent
        } else {
            break;
        }
    }

    // --- STEP 2: Search global functions ---
    const FunctionDefinition* func_def = NULL;

    // 2.1 Check global functions for prop_name verbatim
    func_def = api_spec_find_function(spec, prop_name);
    if (func_def) {
        return _create_prop_from_func(prop_name, func_def, type_name);
    }

    // 2.2 Check global functions for "lv_obj_" + prop_name
    snprintf(constructed_name, sizeof(constructed_name), "lv_obj_%s", prop_name);
    func_def = api_spec_find_function(spec, constructed_name);
    if (func_def) {
        return _create_prop_from_func(prop_name, func_def, type_name);
    }

    return NULL; // Not found
}

void api_spec_free_property(const PropertyDefinition* prop) {
    if (prop && prop->is_heap_allocated) {
        free(prop->name);
        free(prop->c_type);
        free(prop->setter);
        free(prop->widget_type_hint);
        free(prop->obj_setter_prefix);
        free(prop->expected_enum_type);
        // Do not free prop->func_args, as it's a reference to a definition in the main spec.
        free((void*)prop);
    }
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

    WidgetMapNode* current_widget_node = spec->widgets_list_head;
    while (current_widget_node) {
        if (current_widget_node->widget && current_widget_node->widget->methods) {
            FunctionMapNode* current_method_node = current_widget_node->widget->methods;
            while (current_method_node) {
                if (current_method_node->name && strcmp(current_method_node->name, func_name) == 0) {
                     if (current_method_node->func_def && current_method_node->func_def->return_type) {
                        return current_method_node->func_def->return_type;
                    }
                    return "lv_obj_t*";
                }
                current_method_node = current_method_node->next;
            }
        }
        current_widget_node = current_widget_node->next;
    }

    return "lv_obj_t*";
}

const FunctionArg* api_spec_get_function_args_by_name(const ApiSpec* spec, const char* func_name) {
    if (!spec || !func_name) {
        return NULL;
    }

    const FunctionDefinition* func_def = api_spec_find_function(spec, func_name);
    if(func_def) {
        return func_def->args_head;
    }

    return NULL;
}

bool api_spec_is_valid_enum_int_value(const ApiSpec* spec, const char* enum_type_name, int int_value) {
    if (!spec || !spec->enums || !enum_type_name) {
        return false;
    }

    const cJSON* enum_type_json = cJSON_GetObjectItem(spec->enums, enum_type_name);
    if (!enum_type_json || !cJSON_IsObject(enum_type_json)) {
        return false;
    }

    cJSON* enum_member = NULL;
    cJSON_ArrayForEach(enum_member, enum_type_json) {
        if (cJSON_IsString(enum_member) && enum_member->valuestring) {
            char* endptr;
            long val = strtol(enum_member->valuestring, &endptr, 0);
            if (*endptr == '\0') {
                if (val == int_value) {
                    return true;
                }
            }
        } else if (cJSON_IsNumber(enum_member)) {
             if (enum_member->valueint == int_value) {
                 return true;
             }
        }
    }
    return false;
}

bool api_spec_is_enum_member(const ApiSpec* spec, const char* enum_name, const char* member_name) {
    if (!spec || !spec->enums || !enum_name || !member_name) {
        return false;
    }
    const cJSON* enum_type_json = cJSON_GetObjectItem(spec->enums, enum_name);
    if (!enum_type_json || !cJSON_IsObject(enum_type_json)) {
        return false;
    }
    return cJSON_GetObjectItem(enum_type_json, member_name) != NULL;
}

const char* api_spec_find_global_enum_type(const ApiSpec* spec, const char* member_name) {
    if (!spec || !spec->enums || !member_name) {
        return NULL;
    }
    cJSON* enum_type_json = NULL;
    cJSON_ArrayForEach(enum_type_json, spec->enums) {
        if (cJSON_IsObject(enum_type_json)) {
            if (cJSON_GetObjectItem(enum_type_json, member_name) != NULL) {
                return enum_type_json->string; // The key of the enum object is its type name
            }
        }
    }
    return NULL;
}

bool api_spec_is_global_enum_member(const ApiSpec* spec, const char* member_name) {
    return api_spec_find_global_enum_type(spec, member_name) != NULL;
}

bool api_spec_is_constant(const ApiSpec* spec, const char* const_name) {
    if (!spec || !spec->constants || !const_name) {
        return false;
    }
    return cJSON_GetObjectItem(spec->constants, const_name) != NULL;
}

bool api_spec_find_constant_value(const ApiSpec* spec, const char* const_name, long* out_value) {
    if (!spec || !spec->constants || !const_name || !out_value) {
        return false;
    }

    const cJSON* const_json = cJSON_GetObjectItemCaseSensitive(spec->constants, const_name);
    if (!const_json) {
        return false;
    }

    if (cJSON_IsNumber(const_json)) {
        *out_value = (long)const_json->valuedouble;
        return true;
    }

    if (cJSON_IsString(const_json) && const_json->valuestring) {
        // Use the helper to get a clean string without comments.
        char* clean_val = strip_comments_and_trim(const_json->valuestring);
        if (!clean_val) return false;

        // A numeric constant should NOT be in quotes. If it is, it's a string constant.
        if (clean_val[0] == '"') {
            free(clean_val);
            return false;
        }

        char* endptr;
        long val = strtol(clean_val, &endptr, 0); // Use base 0 for auto-detection (e.g., 0x)

        // The parse is successful only if the entire string was a valid number.
        bool success = (*endptr == '\0' && endptr != clean_val);
        if (success) {
            *out_value = val;
        }

        free(clean_val);
        return success;
    }

    return false;
}

char* api_spec_find_constant_string(const ApiSpec* spec, const char* const_name) {
    if (!spec || !spec->constants || !const_name) {
        return NULL;
    }
    const cJSON* const_json = cJSON_GetObjectItemCaseSensitive(spec->constants, const_name);
    if (!cJSON_IsString(const_json) || !const_json->valuestring) {
        return NULL;
    }

    // Use the helper to get a clean string without comments.
    char* clean_val = strip_comments_and_trim(const_json->valuestring);
    if (!clean_val) return NULL;

    size_t len = strlen(clean_val);
    // A string constant MUST be enclosed in quotes.
    if (len >= 2 && clean_val[0] == '"' && clean_val[len - 1] == '"') {
        // It's a string literal. Remove the outer quotes and return the content.
        clean_val[len - 1] = '\0'; // Nuke the last quote
        char* result = strdup(clean_val + 1);
        free(clean_val);
        return result;
    }

    // This is not a string literal, so we fail the lookup.
    free(clean_val);
    return NULL;
}

bool api_spec_has_function(const ApiSpec* spec, const char* func_name) {
    return api_spec_find_function(spec, func_name) != NULL;
}

bool api_spec_find_enum_value(const ApiSpec* spec, const char* enum_type_name, const char* member_name, long* out_value) {
    if (!spec || !spec->enums || !enum_type_name || !member_name || !out_value) {
        return false;
    }

    const cJSON* enum_type_obj = cJSON_GetObjectItemCaseSensitive(spec->enums, enum_type_name);
    if (!cJSON_IsObject(enum_type_obj)) {
        return false;
    }

    const cJSON* enum_member_json = cJSON_GetObjectItemCaseSensitive(enum_type_obj, member_name);
    if (!enum_member_json) {
        return false;
    }

    if (cJSON_IsString(enum_member_json) && enum_member_json->valuestring != NULL) {
        char* endptr;
        *out_value = strtol(enum_member_json->valuestring, &endptr, 0);
        if (*endptr == '\0' && endptr != enum_member_json->valuestring) {
            return true;
        } else {
            return false;
        }
    } else if (cJSON_IsNumber(enum_member_json)) {
        *out_value = (long)enum_member_json->valuedouble;
        return true;
    }

    return false;
}

const FunctionDefinition* api_spec_find_function(const ApiSpec* spec, const char* func_name) {
    if (!spec || !func_name) return NULL;

    if(spec->functions) {
        FunctionMapNode* current_fnode = spec->functions;
        while (current_fnode) {
            if (current_fnode->name && strcmp(current_fnode->name, func_name) == 0) {
                return current_fnode->func_def; // Function found
            }
            current_fnode = current_fnode->next;
        }
    }

    WidgetMapNode* current_wnode = spec->widgets_list_head;
    while (current_wnode) {
        if(current_wnode->widget && current_wnode->widget->methods) {
             FunctionMapNode* current_mnode = current_wnode->widget->methods;
             while(current_mnode) {
                if(current_mnode->name && strcmp(current_mnode->name, func_name) == 0) {
                    return current_mnode->func_def;
                }
                current_mnode = current_mnode->next;
             }
        }
        current_wnode = current_wnode->next;
    }

    return NULL; // Function not found
}

const char* api_spec_find_enum_symbol_by_value(const ApiSpec* spec, const char* enum_type_name, long value) {
    if (!spec || !spec->enums || !enum_type_name) {
        return NULL;
    }
    const cJSON* enum_type_obj = cJSON_GetObjectItemCaseSensitive(spec->enums, enum_type_name);
    if (!cJSON_IsObject(enum_type_obj)) {
        return NULL;
    }

    cJSON* member = NULL;
    cJSON_ArrayForEach(member, enum_type_obj) {
        long member_val = -1;
        bool parsed = false;
        if (cJSON_IsString(member) && member->valuestring) {
            char* endptr;
            member_val = strtol(member->valuestring, &endptr, 0);
            if (*endptr == '\0' && endptr != member->valuestring) {
                parsed = true;
            }
        } else if (cJSON_IsNumber(member)) {
            member_val = (long)member->valuedouble;
            parsed = true;
        }
        if (parsed && member_val == value) {
            return member->string; // The key of the member is its symbolic name
        }
    }

    return NULL; // Not found
}

const char* api_spec_suggest_property(const ApiSpec* spec, const char* type_name, const char* misspelled_prop) {
    static char best_match[128] = {0}; // Static buffer to return pointer to
    int min_dist = INT_MAX;
    best_match[0] = '\0';

    const char* current_type_to_check = type_name;
    while (current_type_to_check != NULL && current_type_to_check[0] != '\0') {
        const WidgetDefinition* widget_def = api_spec_find_widget(spec, current_type_to_check);
        if (widget_def) {
            // Check properties
            PropertyDefinitionNode* p_node = widget_def->properties;
            while(p_node) {
                if (p_node->prop && p_node->prop->name) {
                    int dist = levenshtein_distance(misspelled_prop, p_node->prop->name);
                    if (dist < min_dist) {
                        min_dist = dist;
                        strncpy(best_match, p_node->prop->name, sizeof(best_match) - 1);
                        best_match[sizeof(best_match) - 1] = '\0';
                    }
                }
                p_node = p_node->next;
            }
            // Check methods
            FunctionMapNode* m_node = widget_def->methods;
            while(m_node) {
                if (m_node->name) {
                    int dist = levenshtein_distance(misspelled_prop, m_node->name);
                    if (dist < min_dist) {
                        min_dist = dist;
                        strncpy(best_match, m_node->name, sizeof(best_match) - 1);
                        best_match[sizeof(best_match) - 1] = '\0';
                    }
                }
                m_node = m_node->next;
            }
            current_type_to_check = widget_def->inherits;
        } else {
            break;
        }
    }
    
    // Suggest only if the match is reasonably close (e.g., distance < 4)
    if (min_dist < 4) {
        return best_match;
    }

    return NULL;
}

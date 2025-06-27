#include "generator.h"
#include "ir.h"
#include "api_spec.h"
#include "registry.h"
#include "debug_log.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>

// --- Generation Context ---
typedef struct {
    const ApiSpec* api_spec;
    Registry* registry;
    int var_counter;
} GenContext;

// --- Forward Declarations ---
static IRObject* parse_object(GenContext* ctx, cJSON* obj_json, const char* parent_c_name, const cJSON* ui_context);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* obj_type, const char* prop_name);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static const char* find_widget_name_by_c_type(const ApiSpec* spec, const char* c_type);
static char* sanitize_c_identifier(const char* input_name);


// --- Main Entry Point ---

IRRoot* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
    if (!ui_spec_root || !cJSON_IsArray(ui_spec_root)) {
        DEBUG_LOG(LOG_MODULE_GENERATOR, "Error: UI spec root is not a valid JSON array.");
        return NULL;
    }
    if (!api_spec) {
        DEBUG_LOG(LOG_MODULE_GENERATOR, "Error: API spec is NULL.");
        return NULL;
    }

    IRRoot* ir_root = ir_new_root();
    if (!ir_root) {
        render_abort("Failed to create IR Root.");
        return NULL;
    }

    GenContext ctx = {
        .api_spec = api_spec,
        .registry = registry_create(),
        .var_counter = 0
    };
    if (!ctx.registry) {
        ir_free((IRNode*)ir_root);
        render_abort("Failed to create registry.");
        return NULL;
    }

    // The implicit parent for all top-level objects in the UI-SPEC.
    // The codegen/renderer will know what to do with this name.
    const char* root_parent_name = "parent";

    cJSON* obj_json = NULL;
    cJSON_ArrayForEach(obj_json, ui_spec_root) {
        if (cJSON_IsObject(obj_json)) {
            IRObject* new_obj = parse_object(&ctx, obj_json, root_parent_name, NULL);
            if (new_obj) {
                ir_object_list_add(&ir_root->root_objects, new_obj);
            }
        } else {
            DEBUG_LOG(LOG_MODULE_GENERATOR, "Warning: Non-object item found in UI spec root array. Skipping.");
        }
    }

    registry_free(ctx.registry);
    return ir_root;
}


// --- Core Object Parser ---

static IRObject* parse_object(GenContext* ctx, cJSON* obj_json, const char* parent_c_name, const cJSON* ui_context) {
    if (!cJSON_IsObject(obj_json)) return NULL;

    const char* json_type_str = NULL;
    const char* registered_id = NULL;
    IRExpr* constructor_call = NULL;

    cJSON* type_item = cJSON_GetObjectItem(obj_json, "type");
    cJSON* init_item = cJSON_GetObjectItem(obj_json, "init");
    cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
    if (!id_item) id_item = cJSON_GetObjectItem(obj_json, "name"); // "name" as alias for "id"


    // 1. Determine the object's type and constructor call
    if (type_item && cJSON_IsString(type_item)) {
        json_type_str = type_item->valuestring;
        const WidgetDefinition* widget_def = api_spec_find_widget(ctx->api_spec, json_type_str);

        if (widget_def) {
            if (widget_def->create) { // It's a widget, created with a function like lv_btn_create(parent)
                IRExprNode* args = NULL;
                ir_expr_list_add(&args, ir_new_expr_registry_ref(parent_c_name));
                constructor_call = ir_new_expr_func_call(widget_def->create, args);
            } else if (widget_def->init_func) { // It's an object, created with malloc + init
                 if (!id_item) {
                    DEBUG_LOG(LOG_MODULE_GENERATOR, "Error: Object of type '%s' requires an 'id' for registry storage.", json_type_str);
                    return NULL;
                 }
                // The IR for this is tricky. We'll model it as a call to the init function.
                // The codegen will have to see this and know to declare a variable, not a pointer.
                // We pass the *address of* the object to init. This is a codegen detail.
                // For IR, we just reference the object itself.
                IRExprNode* args = NULL;
                ir_expr_list_add(&args, ir_new_expr_registry_ref(id_item->valuestring)); // Refers to itself
                constructor_call = ir_new_expr_func_call(widget_def->init_func, args);
            } else {
                 DEBUG_LOG(LOG_MODULE_GENERATOR, "Error: Type '%s' has no 'create' or 'init' function in API spec.", json_type_str);
                 return NULL;
            }
        } else {
            DEBUG_LOG(LOG_MODULE_GENERATOR, "Error: Type '%s' not found in API spec.", json_type_str);
            return NULL;
        }

    } else if (init_item) {
        // Construct the object using a direct function call
        constructor_call = unmarshal_value(ctx, init_item, ui_context, NULL, "_init");
        if (constructor_call && constructor_call->type == IR_EXPR_FUNCTION_CALL) {
            IRExprFunctionCall* call = (IRExprFunctionCall*)constructor_call;
            const char* ret_c_type = api_spec_get_function_return_type(ctx->api_spec, call->func_name);
            json_type_str = find_widget_name_by_c_type(ctx->api_spec, ret_c_type);
            if (!json_type_str) {
                DEBUG_LOG(LOG_MODULE_GENERATOR, "Warning: Could not determine object type from return C-type '%s' for func '%s'. Defaulting to 'obj'.", ret_c_type, call->func_name);
                json_type_str = "obj"; // Default fallback
            }
        } else {
             DEBUG_LOG(LOG_MODULE_GENERATOR, "Error: 'init' key must contain a valid function call object.");
             ir_free((IRNode*)constructor_call);
             return NULL;
        }
    } else {
        // It's a "typeless" object, essentially just a container for properties on the parent.
        // We'll treat its type as the parent's type for property lookups. This is an edge case for things like `{"size": [100, 100]}` at the top level.
        // For simplicity, we'll assign it a base "obj" type.
        json_type_str = "obj";
    }

    // 2. Create the IRObject
    if (id_item && cJSON_IsString(id_item)) {
        registered_id = id_item->valuestring;
    }
    char* c_name = generate_unique_var_name(ctx, registered_id ? registered_id : json_type_str);
    IRObject* ir_obj = ir_new_object(c_name, json_type_str, registered_id);
    if (registered_id) {
        registry_add_generated_var(ctx->registry, registered_id, c_name);
    }
    free(c_name);

    // 3. Add constructor as the first "property" if it exists
    if (constructor_call) {
        ir_property_list_add(&ir_obj->properties, ir_new_property("_constructor", constructor_call));
    }


    // 4. Process all other properties
    cJSON* prop_item = NULL;
    cJSON_ArrayForEach(prop_item, obj_json) {
        const char* key = prop_item->string;
        // Skip reserved keys we've already handled
        if (strcmp(key, "type") == 0 || strcmp(key, "init") == 0 || strcmp(key, "id") == 0 || strcmp(key, "name") == 0 || strcmp(key, "children") == 0) {
            continue;
        }

        const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, ir_obj->json_type, key);
        const char* func_name_to_call = NULL;

        if (prop_def && prop_def->setter) {
            func_name_to_call = prop_def->setter;
        } else if (api_spec_has_function(ctx->api_spec, key)) { // Check if key is a global function
            func_name_to_call = key;
        } else {
            DEBUG_LOG(LOG_MODULE_GENERATOR, "Warning: Could not resolve property/method '%s' for type '%s'. Skipping.", key, ir_obj->json_type);
            continue;
        }

        IRExpr* value_expr = unmarshal_value(ctx, prop_item, ui_context, ir_obj->json_type, key);
        if (!value_expr) {
            DEBUG_LOG(LOG_MODULE_GENERATOR, "Warning: Failed to unmarshal value for property '%s'. Skipping.", key);
            continue;
        }

        // The property in IR is the function to call, not the JSON key.
        ir_property_list_add(&ir_obj->properties, ir_new_property(func_name_to_call, value_expr));
    }


    // 5. Process children recursively
    cJSON* children_item = cJSON_GetObjectItem(obj_json, "children");
    if (children_item && cJSON_IsArray(children_item)) {
        cJSON* child_json = NULL;
        cJSON_ArrayForEach(child_json, children_item) {
            IRObject* child_ir_obj = parse_object(ctx, child_json, ir_obj->c_name, ui_context);
            if (child_ir_obj) {
                ir_object_list_add(&ir_obj->children, child_ir_obj);
            }
        }
    }

    return ir_obj;
}


// --- Value Unmarshaler ---

static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* obj_type, const char* prop_name) {
    if (!value) return ir_new_expr_literal("NULL");

    if (cJSON_IsString(value)) {
        const char* s = value->valuestring;
        size_t len = strlen(s);

        if (s[0] == '$' && s[1] != '\0') {
            cJSON* ctx_val = ui_context ? cJSON_GetObjectItem(ui_context, s + 1) : NULL;
            if (ctx_val) {
                return unmarshal_value(ctx, ctx_val, ui_context, obj_type, "context_resolved_value");
            }
            return ir_new_expr_context_var(s + 1);
        }
        if (s[0] == '@') return ir_new_expr_registry_ref(s);
        if (s[0] == '!') return ir_new_expr_static_string(s + 1);

        if (s[0] == '#') { // Hex color
            long hex_val = strtol(s + 1, NULL, 16);
            char hex_str_arg[32];
            snprintf(hex_str_arg, sizeof(hex_str_arg), "0x%06lX", hex_val);
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_literal(hex_str_arg));
            return ir_new_expr_func_call("lv_color_hex", args);
        }
        if (len > 0 && s[len - 1] == '%') { // Percentage
            char* temp_s = strdup(s);
            if (!temp_s) return NULL;
            temp_s[len - 1] = '\0';
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_literal(temp_s));
            free(temp_s);
            return ir_new_expr_func_call("lv_pct", args);
        }

        // Try to interpret as enum or constant
        const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, obj_type, prop_name);

        if (prop_def && prop_def->expected_enum_type) {
            if (api_spec_is_enum_member(ctx->api_spec, prop_def->expected_enum_type, s)) {
                return ir_new_expr_enum(s, 0);
            } else {
                DEBUG_LOG(LOG_MODULE_GENERATOR, "Warning: Invalid enum member '%s' for property '%s' (expected '%s'). Defaulting to 0.", s, prop_name, prop_def->expected_enum_type);
                return ir_new_expr_literal("0");
            }
        }
        if (api_spec_is_global_enum_member(ctx->api_spec, s) || api_spec_is_constant(ctx->api_spec, s)) {
            return ir_new_expr_enum(s, 0);
        }

        return ir_new_expr_literal_string(s);
    }
    if (cJSON_IsNumber(value)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%g", value->valuedouble);
        return ir_new_expr_literal(buf);
    }
    if (cJSON_IsBool(value)) {
        return ir_new_expr_literal(cJSON_IsTrue(value) ? "true" : "false");
    }
    if (cJSON_IsNull(value)) {
        return ir_new_expr_literal("NULL");
    }
    if (cJSON_IsArray(value)) {
        IRExprNode* elements = NULL;
        cJSON* elem_json;
        cJSON_ArrayForEach(elem_json, value) {
            ir_expr_list_add(&elements, unmarshal_value(ctx, elem_json, ui_context, obj_type, prop_name));
        }
        return ir_new_expr_array(elements);
    }
    if (cJSON_IsObject(value)) {
        // This is a JSON-funccall: { "func_name": [args] or arg }
        cJSON* func_item = value->child;
        if (func_item && func_item->string) {
            const char* func_name = func_item->string;
            IRExprNode* args_list = NULL;
            cJSON* args_json = func_item; // The value associated with the key

            if (cJSON_IsArray(args_json)) {
                cJSON* arg_json;
                cJSON_ArrayForEach(arg_json, args_json) {
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, arg_json, ui_context, NULL, "func_arg"));
                }
            } else { // Single argument
                ir_expr_list_add(&args_list, unmarshal_value(ctx, args_json, ui_context, NULL, "func_arg"));
            }
            return ir_new_expr_func_call(func_name, args_list);
        }
    }

    DEBUG_LOG(LOG_MODULE_GENERATOR, "Warning: Unhandled JSON type in unmarshal_value for property '%s'.", prop_name);
    return ir_new_expr_literal("NULL");
}


// --- Helper Functions ---

static char* sanitize_c_identifier(const char* input_name) {
    if (!input_name || *input_name == '\0') return strdup("unnamed_var");

    size_t len = strlen(input_name);
    char* sanitized = malloc(len + 2); // +1 for possible leading '_' and +1 for '\0'
    if (!sanitized) return strdup("oom_var");

    char* s_ptr = sanitized;
    const char* i_ptr = input_name;

    // '@' is a special prefix for registry refs, remove it for C var names
    if (*i_ptr == '@') {
        i_ptr++;
    }

    if (!isalpha((unsigned char)*i_ptr) && *i_ptr != '_') {
        *s_ptr++ = '_';
    }

    while (*i_ptr) {
        if (isalnum((unsigned char)*i_ptr) || *i_ptr == '_') {
            *s_ptr++ = *i_ptr;
        } else {
            *s_ptr++ = '_';
        }
        i_ptr++;
    }
    *s_ptr = '\0';

    return sanitized;
}

static char* generate_unique_var_name(GenContext* ctx, const char* base_name) {
    char sanitized_base[256];
    char* temp_sanitized = sanitize_c_identifier(base_name);
    strncpy(sanitized_base, temp_sanitized, sizeof(sanitized_base) - 1);
    sanitized_base[sizeof(sanitized_base) - 1] = '\0';
    free(temp_sanitized);

    char* final_name = malloc(strlen(sanitized_base) + 16);
    if (!final_name) {
        render_abort("Failed to allocate memory for variable name.");
        return NULL;
    }
    snprintf(final_name, strlen(sanitized_base) + 16, "%s_%d", sanitized_base, ctx->var_counter++);
    return final_name;
}

static const char* find_widget_name_by_c_type(const ApiSpec* spec, const char* c_type) {
    if (!spec || !c_type) return NULL;

    // Create a cleaned-up C type to match against (e.g., "lv_obj_t *" -> "lv_obj_t*")
    char cleaned_c_type[128];
    int j = 0;
    for (int i = 0; c_type[i] && j < sizeof(cleaned_c_type) - 1; i++) {
        if (!isspace(c_type[i])) {
            cleaned_c_type[j++] = c_type[i];
        }
    }
    cleaned_c_type[j] = '\0';


    WidgetMapNode* current_wnode = spec->widgets_list_head;
    while (current_wnode) {
        if (current_wnode->widget && current_wnode->widget->c_type) {
            // Also clean the spec's c_type for comparison
            char cleaned_spec_type[128];
            int k = 0;
            for (int i = 0; current_wnode->widget->c_type[i] && k < sizeof(cleaned_spec_type) - 1; i++) {
                if (!isspace(current_wnode->widget->c_type[i])) {
                     cleaned_spec_type[k++] = current_wnode->widget->c_type[i];
                }
            }
            cleaned_spec_type[k] = '\0';

            if (strcmp(cleaned_c_type, cleaned_spec_type) == 0) {
                return current_wnode->widget->name;
            }
        }
        // Special case for widgets which are all lv_obj_t*
        if (current_wnode->widget && current_wnode->widget->create) {
             if (strcmp(cleaned_c_type, "lv_obj_t*") == 0) {
                 // This isn't perfect, as many create functions return lv_obj_t*
                 // but returning "obj" is a safe bet for property lookups.
                 return "obj";
             }
        }
        current_wnode = current_wnode->next;
    }
    return NULL; // Not found
}

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include "api_spec.h"
#include "generator.h"
#include "ir.h"
#include "registry.h"
#include "utils.h"
#include "debug_log.h"

// --- Context for Generation ---
typedef struct {
    Registry* registry;
    const ApiSpec* api_spec;
    int var_counter;
} GenContext;

// --- Forward Declarations ---
static IRObject* process_ui_node(GenContext* ctx, cJSON* node_json, const char* default_obj_type, cJSON* ui_context);
static IRProperty* process_properties(GenContext* ctx, cJSON* props_json, cJSON* ui_context, const char* obj_type_for_api_lookup);
static IRWithBlock* process_with_blocks(GenContext* ctx, cJSON* with_json, cJSON* ui_context); // Reverted: obj_type_for_with_target_prop_lookup not needed
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context, const char* obj_type, const char* prop_name); // Added obj_type
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);
static bool parse_enum_value_from_json(const cJSON* enum_member_json, intptr_t* out_value);


static bool parse_enum_value_from_json(const cJSON* enum_member_json, intptr_t* out_value) {
    if (!enum_member_json || !out_value) return false;

    if (cJSON_IsNumber(enum_member_json)) {
        *out_value = (intptr_t)enum_member_json->valuedouble;
        return true;
    }

    if (cJSON_IsString(enum_member_json)) {
        const char* s = enum_member_json->valuestring;
        if (!s) return false;

        int base, shift;
        if (sscanf(s, " ( %d << %d )", &base, &shift) == 2) {
             *out_value = (intptr_t)(base << shift);
             return true;
        }

        char* endptr;
        long val = strtol(s, &endptr, 0);
        while(isspace((unsigned char)*endptr)) endptr++;
        if (*endptr == '\0' && s != endptr) {
            *out_value = (intptr_t)val;
            return true;
        }
    }

    return false;
}


// --- Main Generation Logic ---

IRRoot* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
    return generate_ir_from_ui_spec_with_registry(ui_spec_root, api_spec, NULL);
}

IRRoot* generate_ir_from_ui_spec_with_registry(
    const cJSON* ui_spec_root,
    const ApiSpec* api_spec,
    Registry* registry_for_gencontext) {

    if (!ui_spec_root || !api_spec || !cJSON_IsArray(ui_spec_root)) {
        fprintf(stderr, "Error: Invalid arguments or UI Spec root is not an array.\n");
        return NULL;
    }

    GenContext ctx;
    ctx.api_spec = api_spec;
    ctx.var_counter = 0;

    bool own_registry = false;
    if (registry_for_gencontext) {
        ctx.registry = registry_for_gencontext;
    } else {
        ctx.registry = registry_create();
        if (!ctx.registry) {
            fprintf(stderr, "Error: Failed to create registry.\n");
            return NULL;
        }
        own_registry = true;
    }

    IRRoot* ir_root = ir_new_root();
    if (!ir_root) {
        fprintf(stderr, "Error: Failed to create root IR node.\n");
        if (own_registry) registry_free(ctx.registry);
        return NULL;
    }

    // First pass: Register all components by creating their IR and attaching to root
    cJSON* item_json;
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        cJSON* type_node = cJSON_GetObjectItemCaseSensitive(item_json, "type");
        if (type_node && cJSON_IsString(type_node) && strcmp(type_node->valuestring, "component") == 0) {
            cJSON* id_json = cJSON_GetObjectItemCaseSensitive(item_json, "id");
            cJSON* root_json = cJSON_GetObjectItemCaseSensitive(item_json, "root");

            if (cJSON_IsString(id_json) && id_json->valuestring && id_json->valuestring[0] == '@' && root_json) {
                const char* comp_type = cJSON_GetStringValue(cJSON_GetObjectItem(root_json, "type"));
                IRObject* comp_root_obj = process_ui_node(&ctx, root_json, comp_type ? comp_type : "obj", NULL);
                if (comp_root_obj) {
                    IRComponent* comp_def = ir_new_component_def(id_json->valuestring, comp_root_obj);
                    ir_component_def_list_add(&ir_root->components, comp_def);
                }
            } else {
                fprintf(stderr, "Warning: Invalid component definition. Skipping.\n");
            }
        }
    }

    // Second pass: Process all top-level objects (widgets, styles, etc.)
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        cJSON* type_node = cJSON_GetObjectItemCaseSensitive(item_json, "type");
        const char* type_str = cJSON_GetStringValue(type_node);
        if (type_str && strcmp(type_str, "component") == 0) {
            continue; // Skip component definitions
        }

        IRObject* obj = process_ui_node(&ctx, item_json, type_str ? type_str : "obj", NULL);
        if (obj) {
            ir_object_list_add(&ir_root->root_objects, obj);
        }
    }

    if (own_registry) {
        registry_free(ctx.registry);
    }
    return ir_root;
}

static IRObject* process_ui_node(GenContext* ctx, cJSON* node_json, const char* default_obj_type, cJSON* ui_context) {
    if (!cJSON_IsObject(node_json)) return NULL;

    const char* json_type = cJSON_GetStringValue(cJSON_GetObjectItem(node_json, "type"));
    if (!json_type) json_type = default_obj_type;

    const char* name_source = cJSON_GetStringValue(cJSON_GetObjectItem(node_json, "named"));
    char* c_var_name = name_source ? sanitize_c_identifier(name_source) : generate_unique_var_name(ctx, json_type);

    const char* id_str = cJSON_GetStringValue(cJSON_GetObjectItem(node_json, "named"));
    if (!id_str) id_str = cJSON_GetStringValue(cJSON_GetObjectItem(node_json, "id"));

    IRObject* ir_obj = ir_new_object(c_var_name, json_type, id_str);
    free(c_var_name);

    // Handle context merging for child nodes
    cJSON* node_specific_context = cJSON_GetObjectItem(node_json, "context");
    cJSON* effective_context = NULL;
    bool own_effective_context = false;
    if (ui_context && node_specific_context) {
        effective_context = cJSON_Duplicate(ui_context, true);
        cJSON* item;
        for (item = node_specific_context->child; item != NULL; item = item->next) {
            if (cJSON_GetObjectItem(effective_context, item->string)) {
                cJSON_ReplaceItemInObject(effective_context, item->string, cJSON_Duplicate(item, true));
            } else {
                cJSON_AddItemToObject(effective_context, item->string, cJSON_Duplicate(item, true));
            }
        }
        own_effective_context = true;
    } else {
        effective_context = node_specific_context ? node_specific_context : ui_context;
    }

    // Special handling for 'use-view'
    if (strcmp(json_type, "use-view") == 0) {
        const char* comp_id = cJSON_GetStringValue(cJSON_GetObjectItem(node_json, "id"));
        if (comp_id) {
            ir_obj->use_view_component_id = strdup(comp_id);
            cJSON* context_json = cJSON_GetObjectItem(node_json, "context");
            if (context_json) {
                ir_obj->use_view_context = process_properties(ctx, context_json, NULL, "context");
            }
        }
    }

    // Process all other properties
    ir_obj->properties = process_properties(ctx, node_json, effective_context, json_type);

    // Process 'with' blocks
    // Pass json_type (type of the current object obj) to process_with_blocks,
    // so it can pass it to unmarshal_value if context vars are resolved there.
    // However, for the direct target of 'with obj:', that obj_type is not json_type.
    ir_obj->with_blocks = process_with_blocks(ctx, cJSON_GetObjectItem(node_json, "with"), effective_context);

    // Process children
    cJSON* children_json = cJSON_GetObjectItem(node_json, "children");
    if (cJSON_IsArray(children_json)) {
        cJSON* child_json;
        cJSON_ArrayForEach(child_json, children_json) {
            IRObject* child_obj = process_ui_node(ctx, child_json, "obj", effective_context);
            if (child_obj) {
                ir_object_list_add(&ir_obj->children, child_obj);
            }
        }
    }

    if (own_effective_context) {
        cJSON_Delete(effective_context);
    }

    return ir_obj;
}

static IRProperty* process_properties(GenContext* ctx, cJSON* props_json, cJSON* ui_context, const char* obj_type_for_api_lookup) {
    if (!cJSON_IsObject(props_json)) return NULL;

    IRProperty* head = NULL;
    cJSON* prop_json = props_json->child;
    while(prop_json) {
        const char* prop_name = prop_json->string;
        // Skip reserved keywords that are handled elsewhere
        if (strcmp(prop_name, "type") == 0 || strcmp(prop_name, "id") == 0 ||
            strcmp(prop_name, "named") == 0 || strcmp(prop_name, "children") == 0 ||
            strcmp(prop_name, "with") == 0 || strcmp(prop_name, "context") == 0 ||
            strcmp(prop_name, "root") == 0 || strcmp(prop_name, "//") == 0) { // Added "//"
            prop_json = prop_json->next;
            continue;
        }

        IRExpr* value_expr = unmarshal_value(ctx, prop_json, ui_context, obj_type_for_api_lookup, prop_name); // Pass obj_type_for_api_lookup
        if (value_expr) {
            IRProperty* new_prop = ir_new_property(prop_name, value_expr);
            ir_property_list_add(&head, new_prop);
        }
        prop_json = prop_json->next;
    }
    return head;
}

static IRWithBlock* process_with_blocks(GenContext* ctx, cJSON* with_json, cJSON* ui_context) { // Signature reverted
    if (!with_json) return NULL;

    IRWithBlock* head = NULL;
    cJSON* with_item_iterator = cJSON_IsArray(with_json) ? with_json->child : with_json;

    while (with_item_iterator) {
        cJSON* current_with_object = with_item_iterator;
        if (cJSON_IsObject(current_with_object)) {
            cJSON* obj_expr_json = cJSON_GetObjectItem(current_with_object, "obj");
            cJSON* do_block_json = cJSON_GetObjectItem(current_with_object, "do");

            if (obj_expr_json && do_block_json) {
                // For "with.obj", the obj_type is not the containing widget's type. Use "obj" as generic.
                IRExpr* target_expr = unmarshal_value(ctx, obj_expr_json, ui_context, "obj", "with.obj");
                IRProperty* props = process_properties(ctx, do_block_json, ui_context, "obj"); // Properties in "do" apply to an "obj"

                IRObject* with_children_root = NULL;
                cJSON* children_json = cJSON_GetObjectItem(do_block_json, "children");
                if (cJSON_IsArray(children_json)) {
                    with_children_root = ir_new_object(NULL, "container", NULL);
                    cJSON* child_json;
                    cJSON_ArrayForEach(child_json, children_json) {
                        IRObject* child_obj = process_ui_node(ctx, child_json, "obj", ui_context);
                        if (child_obj) {
                           ir_object_list_add(&with_children_root->children, child_obj);
                        }
                    }
                }

                IRWithBlock* wb = ir_new_with_block(target_expr, props, with_children_root);
                ir_with_block_list_add(&head, wb);
            }
        }

        if (cJSON_IsArray(with_json)) {
            with_item_iterator = with_item_iterator->next;
        } else {
            break; // Not an array, process only one and exit loop
        }
    }
    return head;
}

// Added obj_type parameter
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context, const char* obj_type, const char* prop_name) {
    if (!value) return ir_new_expr_literal("NULL");

    if (cJSON_IsString(value)) {
        const char* s = value->valuestring;
        size_t len = strlen(s);

        // Handle special prefixes first, as they override type checking for the property
        if (s[0] == '$' && s[1] != '\0') {
            cJSON* ctx_val = ui_context ? cJSON_GetObjectItem(ui_context, s + 1) : NULL;
            if (ctx_val) {
                // When resolving a context variable, the obj_type and prop_name context might change.
                // However, for now, we pass the original obj_type and a generic "context_resolved_value" as prop_name.
                // This is a simplification; a more robust system might need to track the type of the context var.
                return unmarshal_value(ctx, ctx_val, ui_context, obj_type, "context_resolved_value");
            }
            return ir_new_expr_context_var(s + 1);
        }
        if (s[0] == '@') return ir_new_expr_registry_ref(s); // Keep the '@'
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
            char num_str_arg[32];
            snprintf(num_str_arg, sizeof(num_str_arg), "%d", atoi(temp_s));
            free(temp_s);
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_literal(num_str_arg));
            return ir_new_expr_func_call("lv_pct", args);
        }

        // If no prefix matched, attempt to interpret as enum or constant
        const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, obj_type, prop_name);
        if (!prop_def && obj_type && strcmp(obj_type, "obj") != 0) {
            // If prop_def not found for the specific obj_type (e.g., "base_widget"),
            // try finding it for the generic "obj" type. This helps resolve common properties.
            prop_def = api_spec_find_property(ctx->api_spec, "obj", prop_name);
        }

        if (prop_def && prop_def->expected_enum_type) {
            // Property expects a specific enum type
            if (api_spec_is_enum_member(ctx->api_spec, prop_def->expected_enum_type, s)) {
                return ir_new_expr_enum(s, 0); // Value 0 is placeholder, symbol 's' is used by codegen
            } else {
                // Invalid member for the expected enum type. Default to 0.
                fprintf(stderr, "Warning: Invalid enum member '%s' for property '%s' (expected type '%s'). Defaulting to 0 for object type '%s'.\n", s, prop_name, prop_def->expected_enum_type, obj_type);
                return ir_new_expr_literal("0");
            }
        } else if (api_spec_is_global_enum_member(ctx->api_spec, s) || api_spec_is_constant(ctx->api_spec, s)) {
            // It's a known global enum/constant, not specifically expected by this property, or prop_def is still NULL.
            return ir_new_expr_enum(s, 0);
        }

        // Default: treat as a literal string if no other interpretation fits
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
            // For array elements, the relevant obj_type is the one owning the array property.
            // The prop_name also remains the same (name of the array property).
            ir_expr_list_add(&elements, unmarshal_value(ctx, elem_json, ui_context, obj_type, prop_name));
        }
        return ir_new_expr_array(elements);
    }
    if (cJSON_IsObject(value)) {
        cJSON* call_item = cJSON_GetObjectItem(value, "call");
        if (cJSON_IsString(call_item)) {
            IRExprNode* args_list = NULL;
            cJSON* args_item = cJSON_GetObjectItem(value, "args");
            if (args_item) {
                if(cJSON_IsArray(args_item)) {
                    cJSON* arg_json;
                    cJSON_ArrayForEach(arg_json, args_item) {
                        // For function arguments, obj_type is not directly applicable for prop_def lookup.
                        // Pass NULL for obj_type, and "call.args[]" as prop_name.
                        ir_expr_list_add(&args_list, unmarshal_value(ctx, arg_json, ui_context, NULL, "call.args[]"));
                    }
                } else {
                     // Pass NULL for obj_type, and "call.args" as prop_name.
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, args_item, ui_context, NULL, "call.args"));
                }
            }
            return ir_new_expr_func_call(call_item->valuestring, args_list);
        }
    }

    fprintf(stderr, "Warning: Unhandled JSON type in unmarshal_value for property '%s'. Returning NULL literal.\n", prop_name);
    return ir_new_expr_literal("NULL");
}

// --- Utility Functions ---

static char* sanitize_c_identifier(const char* input_name) {
    if (!input_name || *input_name == '\0') return strdup("unnamed_var");

    size_t len = strlen(input_name);
    char* sanitized = malloc(len + 2);
    if (!sanitized) return strdup("oom_var");

    char* s_ptr = sanitized;
    const char* i_ptr = input_name;

    if (isdigit((unsigned char)*i_ptr)) {
        *s_ptr++ = '_';
    }

    while(*i_ptr) {
        if (isalnum((unsigned char)*i_ptr)) {
            *s_ptr++ = *i_ptr;
        } else if (s_ptr > sanitized && *(s_ptr-1) != '_') {
            *s_ptr++ = '_';
        }
        i_ptr++;
    }
    *s_ptr = '\0';
    return sanitized;
}

static char* generate_unique_var_name(GenContext* ctx, const char* base_type) {
    char buf[128];
    char* sanitized_base = sanitize_c_identifier(base_type ? base_type : "obj");
    snprintf(buf, sizeof(buf), "%s_%d", sanitized_base, ctx->var_counter++);
    free(sanitized_base);
    return strdup(buf);
}

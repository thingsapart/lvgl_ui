#include "generator.h"
#include "ir.h"
#include "registry.h"
#include "api_spec.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h> // For bool type if used

// --- Context for Generation ---
typedef struct {
    Registry* registry;
    ApiSpec* api_spec;
    int var_counter; // For generating unique variable names
    IRStmtBlock* current_global_block; // For global statements like style declarations
} GenContext;

// Forward declarations
static void process_node(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context);
static void process_properties(GenContext* ctx, cJSON* props_json, const char* target_c_var_name, IRStmtBlock* current_block, const char* obj_type_for_api_lookup, cJSON* ui_context);
static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);


// --- Utility Functions ---

// Generates a unique C variable name (e.g., "button_0", "style_1")
static char* generate_unique_var_name(GenContext* ctx, const char* base_type) {
    char buf[128];
    // A more robust approach might involve checking for collisions if base_type could be complex
    snprintf(buf, sizeof(buf), "%s_%d", base_type, ctx->var_counter++);
    return strdup(buf);
}

// --- Core Processing Functions ---

// Unmarshals a JSON value into an IR expression
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, cJSON* ui_context) {
    if (!value) return ir_new_literal("NULL"); // Handle case where context lookup might yield NULL

    if (cJSON_IsString(value)) {
        const char* s_orig = value->valuestring;
        char* s_duped = strdup(s_orig); // Work with a mutable copy for unescaping/parsing
        char* s = s_duped;

        // 1. Check for unescaped values first
        size_t len = strlen(s);
        if (len >= 2 && s[0] == s[len - 1]) {
            char boundary_char = s[0];
            if (boundary_char == '$' || boundary_char == '!' || boundary_char == '@' || boundary_char == '#' || boundary_char == '%') {
                // Check for "%%" specifically for percentage unescaping
                if (boundary_char == '%' && len > 1 && s[len-2] == '%') { // e.g. "100%%"
                    s[len - 1] = '\0'; // Remove last '%'
                    // s now holds "100%" - this will be handled by lv_pct later if it's the N% case
                    // or be a literal string "100%" if not.
                } else if (boundary_char != '%') { // For $, !, @, #
                    s[len - 1] = '\0'; // Remove trailing boundary char
                    memmove(s, s + 1, len - 1); // Shift string left, len includes new null term
                                                // s now holds the unescaped inner value
                    IRExpr* lit = ir_new_literal_string(s);
                    free(s_duped);
                    return lit;
                }
                // If it was "%%", s was modified, fall through to normal processing with the modified s.
            }
        }

        // 2. Handle special prefixes/values
        if (s[0] == '$') { // Context variable
            if (ui_context) { // Ensure ui_context is not NULL
                cJSON* ctx_val = cJSON_GetObjectItem(ui_context, s + 1);
                if (ctx_val) {
                    free(s_duped); // Free before recursive call that will alloc its own
                    return unmarshal_value(ctx, ctx_val, ui_context); // Recurse
                } else {
                    fprintf(stderr, "Warning: Context variable '%s' not found.\n", s + 1);
                    free(s_duped);
                    return ir_new_literal("NULL"); // Or some other representation of "not found"
                }
            } else {
                 fprintf(stderr, "Warning: Attempted to access context variable '%s' with NULL context.\n", s + 1);
                 free(s_duped);
                 return ir_new_literal("NULL");
            }
        }
        if (s[0] == '@') { // Registry reference
            const char* registered_c_var = registry_get_generated_var(ctx->registry, s + 1);
            if (registered_c_var) {
                // Heuristic: if it's a style, return its address
                // This could be made more robust if styles had a specific type in registry or API spec
                if (strstr(registered_c_var, "style") != NULL) {
                    IRExpr* var_expr = ir_new_variable(registered_c_var);
                    IRExpr* addr_expr = ir_new_address_of(var_expr);
                    free(s_duped);
                    return addr_expr;
                }
                IRExpr* var_expr = ir_new_variable(registered_c_var);
                free(s_duped);
                return var_expr;
            }
            // Not found in generated_vars, assume it's a C-registered pointer/variable
            // or a named object that will be resolvable in C.
            IRExpr* var_expr = ir_new_variable(s + 1); // Use name without '@'
            free(s_duped);
            return var_expr;
        }
        if (s[0] == '#') { // Color e.g. #FF00AA
            long hex_val = strtol(s + 1, NULL, 16);
            char hex_str_arg[32];
            // LVGL's lv_color_hex expects a single integer 0xRRGGBB
            snprintf(hex_str_arg, sizeof(hex_str_arg), "0x%06lX", hex_val);

            IRExprNode* args = ir_new_expr_node(ir_new_literal(hex_str_arg));
            IRExpr* call_expr = ir_new_func_call_expr("lv_color_hex", args);
            free(s_duped);
            return call_expr;
        }
        if (s[0] == '!') { // Static string "!Hello World"
            // Treat as a regular string literal after removing '!'
            // The "static" nature refers to its lifetime for LVGL,
            // a C string literal already has static lifetime.
            IRExpr* lit_expr = ir_new_literal_string(s + 1);
            free(s_duped);
            return lit_expr;
        }

        // Check for "N%" percentage, carefully, could be "100%%" which was unescaped to "100%"
        len = strlen(s); // re-evaluate len as s might have changed
        if (len > 0 && s[len - 1] == '%') {
            char* temp_s = strdup(s); // work on another copy for modification
            temp_s[len - 1] = '\0'; // Remove '%'
            char* endptr;
            long num_val = strtol(temp_s, &endptr, 10);
            if (*endptr == '\0') { // Successfully parsed as a number
                char num_str_arg[32];
                snprintf(num_str_arg, sizeof(num_str_arg), "%ld", num_val);
                IRExprNode* args = ir_new_expr_node(ir_new_literal(num_str_arg));
                IRExpr* call_expr = ir_new_func_call_expr("lv_pct", args);
                free(temp_s);
                free(s_duped);
                return call_expr;
            }
            free(temp_s);
            // If not a valid number before %, it's just a string ending with %, treat as literal string below
        }

        // 3. Check for known constants/enums from API spec
        const cJSON* constants = api_spec_get_constants(ctx->api_spec);
        if (constants && cJSON_GetObjectItem(constants, s)) {
            IRExpr* lit_expr = ir_new_literal(s); // e.g. LV_ALIGN_CENTER
            free(s_duped);
            return lit_expr;
        }
        const cJSON* enums = api_spec_get_enums(ctx->api_spec);
        if (enums && cJSON_GetObjectItem(enums, s)) {
            IRExpr* lit_expr = ir_new_literal(s); // e.g. LV_FLEX_FLOW_ROW
            free(s_duped);
            return lit_expr;
        }

        // 4. Default: It's a regular string literal
        IRExpr* lit_expr = ir_new_literal_string(s);
        free(s_duped);
        return lit_expr;

    } else if (cJSON_IsNumber(value)) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%d", (int)value->valuedouble); // Consider if double/float is needed
        return ir_new_literal(buf);
    } else if (cJSON_IsBool(value)) { // LVGL uses macros like LV_STATE_CHECKED (int) or bools in C
        return ir_new_literal(cJSON_IsTrue(value) ? "true" : "false"); // Or "1" / "0" if preferred for C
    } else if (cJSON_IsArray(value)) {
        IRExprNode* elements = NULL;
        cJSON* elem_json;
        cJSON_ArrayForEach(elem_json, value) {
            ir_expr_list_add(&elements, unmarshal_value(ctx, elem_json, ui_context));
        }
        return ir_new_array(elements);
    } else if (cJSON_IsObject(value)) { // Function call { "call": "name", "args": [] }
        cJSON* call_item = cJSON_GetObjectItem(value, "call");
        cJSON* args_item = cJSON_GetObjectItem(value, "args");

        if (cJSON_IsString(call_item)) {
            const char* call_name = call_item->valuestring;
            IRExprNode* args_list = NULL;
            if (cJSON_IsArray(args_item)) {
                 cJSON* arg_json;
                 cJSON_ArrayForEach(arg_json, args_item) {
                    ir_expr_list_add(&args_list, unmarshal_value(ctx, arg_json, ui_context));
                }
            } else if (args_item != NULL) { // Single argument not in an array
                 ir_expr_list_add(&args_list, unmarshal_value(ctx, args_item, ui_context));
            }
            return ir_new_func_call_expr(call_name, args_list);
        }
    } else if (cJSON_IsNull(value)) {
        return ir_new_literal("NULL");
    }

    fprintf(stderr, "Warning: Unhandled JSON type in unmarshal_value or invalid structure. Returning NULL literal.\n");
    return ir_new_literal("NULL"); // Fallback for unhandled types
}

// Processes a "properties" JSON object and adds corresponding IR statements (function calls)
// The 'obj_type_for_api_lookup' is crucial for finding the correct property info in api_spec
// (e.g., "obj", "label", "button", "style").
static void process_properties(GenContext* ctx, cJSON* props_json, const char* target_c_var_name, IRStmtBlock* current_block, const char* obj_type_for_api_lookup, cJSON* ui_context) {
    if (!props_json) return;

    cJSON* prop = NULL;
    for (prop = props_json->child; prop != NULL; prop = prop->next) {
        const char* prop_name = prop->string;
        // Use obj_type_for_api_lookup to get the correct property definition
        const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, obj_type_for_api_lookup, prop_name);

        if (!prop_def) {
            // Fallback: try generic "obj" type if specific type lookup failed
            if (strcmp(obj_type_for_api_lookup, "obj") != 0) {
                prop_def = api_spec_find_property(ctx->api_spec, "obj", prop_name);
            }
            if (!prop_def) {
                 if (strncmp(prop_name, "style_", 6) == 0 && strcmp(obj_type_for_api_lookup, "style") != 0) {
                    fprintf(stderr, "Info: Property '%s' on obj_type '%s' looks like a style property. Ensure it's applied to a style object or handled by add_style.\n", prop_name, obj_type_for_api_lookup);
                 } else if (strcmp(obj_type_for_api_lookup, "style") == 0 && api_spec_find_property(ctx->api_spec, "obj", prop_name)) {
                    // This is a style object, but the property is an obj property (e.g. width on a style) - this is usually not what's intended.
                    fprintf(stderr, "Warning: Property '%s' applied to a style object '%s' is an object property, not a style property. This might be incorrect.\n", prop_name, target_c_var_name);
                 } else {
                    fprintf(stderr, "Warning: Property '%s' for object type '%s' (C var '%s') not found in API spec. Skipping.\n", prop_name, obj_type_for_api_lookup, target_c_var_name);
                 }
                continue;
            }
        }

        // Determine the actual setter function name
        // Assuming PropertyDefinition has a 'setter' field for the function name.
        const char* actual_setter_name_const = prop_def->setter;
        char* actual_setter_name_allocated = NULL;

        if (!actual_setter_name_const) {
            // Construct if not directly provided
            char constructed_setter[128];
            if (strcmp(obj_type_for_api_lookup, "style") == 0) {
                 snprintf(constructed_setter, sizeof(constructed_setter), "lv_style_set_%s", prop_name);
            } else {
                 // Assuming PropertyDefinition has a 'widget_type_hint' field like PropertyInfo did.
                 snprintf(constructed_setter, sizeof(constructed_setter), "lv_%s_set_%s",
                         prop_def->widget_type_hint ? prop_def->widget_type_hint : obj_type_for_api_lookup,
                         prop_name);
            }
            actual_setter_name_allocated = strdup(constructed_setter);
            actual_setter_name_const = actual_setter_name_allocated;
            fprintf(stderr, "Warning: Setter for '%s' on type '%s' constructed as '%s'. API spec should provide this.\n", prop_name, obj_type_for_api_lookup, actual_setter_name_const);
        }

        IRExprNode* args_list = ir_new_expr_node(ir_new_variable(target_c_var_name));

        bool is_complex_style_prop = false;
        // Assuming PropertyDefinition has 'num_style_args' field
        if (prop_def->num_style_args > 0) {
            is_complex_style_prop = true;
        }

        cJSON* value_to_unmarshal = prop;
        // Assuming PropertyDefinition has 'style_part_default' and 'style_state_default' fields
        const char* part_str = prop_def->style_part_default;
        const char* state_str = prop_def->style_state_default;

        if (cJSON_IsObject(prop) && cJSON_HasObjectItem(prop, "value")) {
            cJSON* part_json = cJSON_GetObjectItem(prop, "part");
            cJSON* state_json = cJSON_GetObjectItem(prop, "state");
            cJSON* value_json = cJSON_GetObjectItem(prop, "value");

            if (cJSON_IsString(part_json)) part_str = part_json->valuestring;
            if (cJSON_IsString(state_json)) state_str = state_json->valuestring;
            value_to_unmarshal = value_json; // Actual value is nested
        }

        if (is_complex_style_prop) {
            if (strcmp(obj_type_for_api_lookup, "style") != 0) { // e.g. lv_obj_set_local_...
                 ir_expr_list_add(&args_list, ir_new_literal((char*)part_str)); // part arg
            }
            // Both obj and style setters with style_args take state
            ir_expr_list_add(&args_list, ir_new_literal((char*)state_str)); // state arg
        }

        IRExpr* val_expr = unmarshal_value(ctx, value_to_unmarshal, ui_context);
        ir_expr_list_add(&args_list, val_expr);

        IRStmt* call_stmt = ir_new_func_call_stmt(actual_setter_name_const, args_list);
        ir_block_add_stmt(current_block, call_stmt);

        if (actual_setter_name_allocated) { // If we strdup'd for constructed name
            free(actual_setter_name_allocated);
        }
    }
}

// Processes a single UI node (widget or component instance)
// New signature:
static void process_node(GenContext* ctx, cJSON* node_json, IRStmtBlock* parent_block, const char* parent_c_var, const char* default_obj_type, cJSON* ui_context) {
    if (!cJSON_IsObject(node_json)) return;

    // TODO: Use default_obj_type if type_item is missing, though current logic requires "type".
    // For now, default_obj_type is unused if "type" is mandatory.
    cJSON* type_item = cJSON_GetObjectItem(node_json, "type");
    if (!cJSON_IsString(type_item)) {
        fprintf(stderr, "Error: Node missing 'type' or type is not a string. Skipping node.\n");
        return;
    }

    // Determine current widget variable name
    char* current_widget_var = NULL;
    cJSON* id_item = cJSON_GetObjectItem(node_json, "id");
    if (cJSON_IsString(id_item)) {
        current_widget_var = strdup(id_item->valuestring);
        // Register this ID so if it's referenced by '@id', we use the C var name
        registry_add_generated_var(ctx->registry, id_item->valuestring, current_widget_var);
    } else {
        current_widget_var = generate_unique_var_name(ctx, cJSON_GetStringValue(type_item));
    }

    // --- Context Handling ---
    // Parameter 'ui_context' is the inherited context.
    cJSON* node_specific_context = cJSON_GetObjectItem(node_json, "context");
    cJSON* effective_context = NULL;
    bool own_effective_context = false;

    if (ui_context && node_specific_context) { // inherited_context is now ui_context
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
    } else if (node_specific_context) {
        effective_context = node_specific_context;
    } else if (ui_context) { // inherited_context is now ui_context
        effective_context = ui_context;
    }
    // If neither, effective_context remains NULL.

    // --- Component Expansion and Property Merging ---
    cJSON* effective_properties = NULL;
    bool own_effective_properties = false; // If true, effective_properties needs cJSON_Delete

    const char* widget_type_str = cJSON_GetStringValue(type_item);
    const char* final_widget_type_for_create = widget_type_str; // Actual LVGL type, e.g. "button", "label"

    cJSON* component_definition = NULL;
    if (widget_type_str[0] == '@') { // It's a component reference
        component_definition = (cJSON*)registry_get_component(ctx->registry, widget_type_str + 1);
        if (!component_definition) {
            fprintf(stderr, "Error: Component '%s' not found in registry. Skipping node.\n", widget_type_str);
            if (own_effective_context) cJSON_Delete(effective_context);
            free(current_widget_var);
            return;
        }
        // The actual LVGL type is inside the component's definition
        cJSON* comp_type_item = cJSON_GetObjectItem(component_definition, "type");
        if (!cJSON_IsString(comp_type_item)) {
             fprintf(stderr, "Error: Component definition '%s' is missing 'type'. Skipping node.\n", widget_type_str);
             if (own_effective_context) cJSON_Delete(effective_context);
             free(current_widget_var);
             return;
        }
        final_widget_type_for_create = cJSON_GetStringValue(comp_type_item);

        // Properties: component defaults < node instance properties
        cJSON* component_props = cJSON_GetObjectItem(component_definition, "properties");
        cJSON* instance_props = cJSON_GetObjectItem(node_json, "properties");

        if (component_props) {
            effective_properties = cJSON_Duplicate(component_props, true);
            own_effective_properties = true;
            if (instance_props) { // Override with instance properties
                cJSON* prop_iter;
                for (prop_iter = instance_props->child; prop_iter != NULL; prop_iter = prop_iter->next) {
                    if (cJSON_GetObjectItem(effective_properties, prop_iter->string)) {
                        cJSON_ReplaceItemInObject(effective_properties, prop_iter->string, cJSON_Duplicate(prop_iter, true));
                    } else {
                        cJSON_AddItemToObject(effective_properties, prop_iter->string, cJSON_Duplicate(prop_iter, true));
                    }
                }
            }
        } else if (instance_props) {
            effective_properties = instance_props; // Use directly, no free by this block
            own_effective_properties = false;
        }
    } else { // It's a direct widget type
        effective_properties = cJSON_GetObjectItem(node_json, "properties"); // Use directly
        own_effective_properties = false;
    }


    // --- Create IR for the widget object itself ---
    char parent_c_var_for_create[128];
    if (parent_c_var) { // Use new parameter parent_c_var
        snprintf(parent_c_var_for_create, sizeof(parent_c_var_for_create), "%s", parent_c_var);
    } else {
        snprintf(parent_c_var_for_create, sizeof(parent_c_var_for_create), "lv_scr_act()");
    }

    char create_func_name[128];
    snprintf(create_func_name, sizeof(create_func_name), "lv_%s_create", final_widget_type_for_create);

    IRExprNode* create_args = ir_new_expr_node(ir_new_variable(parent_c_var_for_create));

    IRStmt* create_stmt = ir_new_var_decl(
        "lv_obj_t*",
        current_widget_var,
        ir_new_func_call_expr(create_func_name, create_args)
    );
    ir_block_add_stmt(parent_block, create_stmt);

    // Process properties (setters) using effective_properties and effective_context
    // The obj_type_for_api_lookup for a newly created widget is its final_widget_type_for_create (e.g. "button", "label")
    if (effective_properties) {
        process_properties(ctx, effective_properties, current_widget_var, parent_block, final_widget_type_for_create, effective_context);
    }

    // Process children, passing current_widget_var as their parent and effective_context
    cJSON* children_json = cJSON_GetObjectItem(component_definition ? component_definition : node_json, "children");
    // If it's a component, its defined children are processed. Node's "children" are ignored if it's a component instance.
    // This behavior might need refinement: should component instance be able to add more children?
    // For now, component's structure is fixed.

    if (cJSON_IsArray(children_json)) {
        cJSON* child_node_json;
        cJSON_ArrayForEach(child_node_json, children_json) {
            // Recursive call needs to match the new 6-argument signature.
            // parent_block is the same.
            // parent_c_var for child is current_widget_var.
            // default_obj_type for child: ideally child's actual type, or NULL if it must have a "type" field.
            // ui_context for child is effective_context.
            cJSON* child_type_item = cJSON_GetObjectItem(child_node_json, "type");
            const char* child_default_type = child_type_item ? cJSON_GetStringValue(child_type_item) : "obj"; // Fallback if type missing, though type is usually mandatory
            if(child_type_item && child_type_item->valuestring[0] == '@'){ // If child is a component, its type for creation is inside component def.
                 const cJSON* child_comp_def = registry_get_component(ctx->registry, child_type_item->valuestring + 1);
                 if(child_comp_def) {
                    cJSON* child_comp_type_item = cJSON_GetObjectItem(child_comp_def, "type");
                    if(child_comp_type_item) child_default_type = cJSON_GetStringValue(child_comp_type_item);
                 }
            }
            process_node(ctx, child_node_json, parent_block, current_widget_var, child_default_type, effective_context);
        }
    }

    // Cleanup
    if (own_effective_properties) {
        cJSON_Delete(effective_properties);
    }
    if (own_effective_context) {
        cJSON_Delete(effective_context);
    }
    // --- Process "with" blocks (after main properties and children of original node) ---
    // "with" is on the instance node_json (which could be a component instance or direct widget)
    cJSON* with_prop = cJSON_GetObjectItem(node_json, "with");
    if (with_prop) {
        cJSON* current_with_item = NULL;
        if (cJSON_IsArray(with_prop)) {
            cJSON_ArrayForEach(current_with_item, with_prop) {
                // parent_block is the block where the current widget (node_json) was created.
                process_single_with_block(ctx, current_with_item, parent_block, effective_context);
            }
        } else if (cJSON_IsObject(with_prop)) {
            process_single_with_block(ctx, with_prop, parent_block, effective_context);
        }
    }

    if (own_effective_properties) { // Cleanup effective_properties if it was duplicated
        cJSON_Delete(effective_properties);
    }
    if (own_effective_context) { // Cleanup effective_context if it was duplicated/merged
        cJSON_Delete(effective_context);
    }
    free(current_widget_var);
}

// New helper function for "with" blocks:
static void process_single_with_block(GenContext* ctx, cJSON* with_node, IRStmtBlock* parent_ir_block, cJSON* ui_context) {
    if (!cJSON_IsObject(with_node)) {
        fprintf(stderr, "Error: 'with' block item must be an object.\n");
        return;
    }

    cJSON* obj_json = cJSON_GetObjectItem(with_node, "obj");
    cJSON* do_json = cJSON_GetObjectItem(with_node, "do");

    if (!obj_json) {
        fprintf(stderr, "Error: 'with' block missing 'obj' key.\n");
        return;
    }
    if (!cJSON_IsObject(do_json)) {
        fprintf(stderr, "Error: 'with' block missing 'do' object or 'do' is not an object.\n");
        return;
    }

    IRExpr* obj_expr = unmarshal_value(ctx, obj_json, ui_context);
    if (!obj_expr) {
        fprintf(stderr, "Error: Failed to unmarshal 'obj' in 'with' block.\n");
        return;
    }

    const char* target_c_var_name = NULL;
    char* generated_var_name_to_free = NULL; // Stores any name that was strdup'd and needs freeing locally

    const char* obj_type_for_props = "obj"; // Default type for properties lookup, e.g. "obj", "style"
    char* temp_var_c_type = "lv_obj_t*"; // Default C type for temporary variable, e.g. "lv_obj_t*", "lv_style_t*"


    if (obj_expr->type == IR_EXPR_VARIABLE) {
        // If obj is already a variable, use its name.
        // Duplicate the name because obj_expr will be freed.
        target_c_var_name = strdup(((IRExprVariable*)obj_expr)->name);
        generated_var_name_to_free = (char*)target_c_var_name; // Mark for freeing

        // Determine type for properties based on variable name heuristic
        if (strstr(target_c_var_name, "style") != NULL) {
            obj_type_for_props = "style";
        }
        ir_free((IRNode*)obj_expr); // Free the temporary IRExprVariable, we only needed its name.
    } else if (obj_expr->type == IR_EXPR_FUNC_CALL || obj_expr->type == IR_EXPR_ADDRESS_OF || obj_expr->type == IR_EXPR_LITERAL) {
        // If obj is a function call or other expression, create a temporary C variable.
        generated_var_name_to_free = generate_unique_var_name(ctx, "with_target"); // This is strdup'd
        target_c_var_name = generated_var_name_to_free;

        // Try to determine the C type and property type more accurately
        if (obj_expr->type == IR_EXPR_ADDRESS_OF) {
            IRExprAddressOf* addr_of = (IRExprAddressOf*)obj_expr;
            if (addr_of->expr && addr_of->expr->type == IR_EXPR_VARIABLE) {
                if (strstr(((IRExprVariable*)addr_of->expr)->name, "style") != NULL) {
                    temp_var_c_type = "lv_style_t*";
                    obj_type_for_props = "style";
                }
            }
        } else if (obj_expr->type == IR_EXPR_FUNC_CALL) {
            // TODO: Infer C type and property type from function return type in API spec
            // For now, default to lv_obj_t* and "obj"
        }

        IRStmt* var_decl_stmt = ir_new_var_decl(temp_var_c_type, target_c_var_name, obj_expr);
        // obj_expr is now owned by var_decl_stmt and will be freed with the IR tree.
        ir_block_add_stmt(parent_ir_block, var_decl_stmt);
    } else {
        fprintf(stderr, "Error: 'obj' expression in 'with' block yielded an unexpected IR type: %d.\n", obj_expr->type);
        ir_free((IRNode*)obj_expr); // Free if not used
        return;
    }

    // Process the "do" block, applying properties to target_c_var_name
    // The existing process_properties needs to be called here.
    // It currently takes: GenContext* ctx, cJSON* props_json, const char* widget_var_name, IRStmtBlock* current_block, cJSON* ui_context
    // We need to adapt it or make a new one that takes obj_type_for_props.
    // Let's assume we'll modify process_properties to accept obj_type_for_props.
    process_properties(ctx, do_json, target_c_var_name, parent_ir_block, obj_type_for_props, ui_context);
    process_properties(ctx, do_json, target_c_var_name, parent_ir_block, obj_type_for_props, ui_context);


    if (generated_var_name_to_free) {
        // This was either from strdup(var->name) or generate_unique_var_name.
        // The name string itself is copied by ir_new_variable or ir_new_var_decl.
        // So, this local copy can be freed.
        free(generated_var_name_to_free);
    }
}


// Process "styles" block from the UI spec
static void process_styles(GenContext* ctx, cJSON* styles_json, IRStmtBlock* global_block) {
    if (!cJSON_IsObject(styles_json)) return;

    cJSON* style_item = NULL;
    for (style_item = styles_json->child; style_item != NULL; style_item = style_item->next) {
        const char* style_name_json = style_item->string; // e.g., "style_main_button"
        if (!cJSON_IsObject(style_item)) {
            fprintf(stderr, "Warning: Style '%s' is not a JSON object. Skipping.\n", style_name_json);
            continue;
        }

        char* style_c_var = generate_unique_var_name(ctx, "style");
        registry_add_generated_var(ctx->registry, style_name_json, style_c_var); // Register "style_main_button" -> "style_0"

        // Create style variable: lv_style_t style_0;
        ir_block_add_stmt(global_block, ir_new_var_decl("lv_style_t", style_c_var, NULL));

        // Initialize style: lv_style_init(&style_0);
        IRExprNode* init_args = ir_new_expr_node(ir_new_address_of(ir_new_variable(style_c_var)));
        ir_block_add_stmt(global_block, ir_new_func_call_stmt("lv_style_init", init_args));

        // Process style properties
        cJSON* prop = NULL;
        for (prop = style_item->child; prop != NULL; prop = prop->next) {
            const char* prop_name = prop->string; // e.g., "bg_color"
            // For style definitions, we look up the property as a "style" type property
            const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, "style", prop_name);

            if (!prop_def || !prop_def->is_style_prop) { // Ensure it's marked as a style property
                 fprintf(stderr, "Warning: Property '%s' in style definition '%s' is not a recognized style property. Skipping.\n", prop_name, style_name_json);
                 continue;
            }

            // Construct setter: lv_style_set_PROP(&style_0, LV_STATE_DEFAULT, value)
            // Or for specific states: lv_style_set_bg_color(&style_0, LV_STATE_PRESSED, value)
            // The JSON for styles needs to support states. For now, assume LV_STATE_DEFAULT.
            // Example: "bg_color": "#FF0000" -> lv_style_set_bg_color(&style_0, LV_STATE_DEFAULT, lv_color_hex(0xFF0000))
            // If value is an object: "text_font": { "state": "PRESSED", "value": "@font_large" }

            char setter_name[128];
            // Most style setters are lv_style_set_property_name
            // The api_spec should ideally confirm this or provide the exact setter.
            // Example: prop_name "radius" -> "lv_style_set_radius"
            // prop_name "bg_color" -> "lv_style_set_bg_color"
            snprintf(setter_name, sizeof(setter_name), "lv_style_set_%s", prop_name);


            IRExprNode* style_setter_args = ir_new_expr_node(ir_new_address_of(ir_new_variable(style_c_var)));

            IRExpr* value_expr;
            const char* state_str = "LV_STATE_DEFAULT"; // Default state

            if (cJSON_IsObject(prop)) { // Check for explicit state: "prop_name": { "state": "...", "value": "..." }
                cJSON* state_json = cJSON_GetObjectItem(prop, "state");
                cJSON* value_json = cJSON_GetObjectItem(prop, "value");
                if (cJSON_IsString(state_json) && value_json) {
                    // TODO: map state string (e.g., "PRESSED", "FOCUSED") to LVGL enum/macro string
                    // For now, assume the string itself is the macro e.g. "LV_STATE_PRESSED"
                    state_str = state_json->valuestring;
                    value_expr = unmarshal_value(ctx, value_json, NULL); // No specific ui_context for global styles generally
                } else {
                    fprintf(stderr, "Warning: Style property '%s' for style '%s' is an object but not in expected format {state:..., value:...}. Skipping.\n", prop_name, style_name_json);
                    free(value_expr); // if partially assigned
                    continue;
                }
            } else { // Simple value, use default state
                value_expr = unmarshal_value(ctx, prop, NULL);
            }

            ir_expr_list_add(&style_setter_args, ir_new_literal((char*)state_str)); // State argument
            ir_expr_list_add(&style_setter_args, value_expr);    // Value argument

            ir_block_add_stmt(global_block, ir_new_func_call_stmt(setter_name, style_setter_args));
        }
        free(style_c_var);
    }
}


// Main function to generate IR from UI specification
IRStmtBlock* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
    if (!ui_spec_root) {
        fprintf(stderr, "Error: UI Spec root is NULL in generate_ir_from_ui_spec.\n");
        return NULL;
    }
    if (!api_spec) {
        fprintf(stderr, "Error: API Spec is NULL in generate_ir_from_ui_spec.\n");
        return NULL;
    }
    // The problem description's example ui_spec_root was an array of nodes,
    // but current generator process_node expects "ui" section as object or array.
    // For now, let's stick to the established structure where ui_spec_root has "components", "styles", "ui" sections.
    // The example in the prompt for generate_ir_from_ui_spec directly iterates ui_spec_root as if it's the "ui" array.
    // This needs clarification. Assuming ui_spec_root is the top-level JSON object from the file.

    GenContext ctx; // Intentionally not initializing api_spec here yet
    ctx.registry = registry_create();
    if (!ctx.registry) {
        fprintf(stderr, "Error: Failed to create registry in generate_ir_from_ui_spec.\n");
        return NULL;
    }
    // Now assign api_spec. It's const, and GenContext.api_spec is also const ApiSpec*.
    ctx.api_spec = api_spec;
    ctx.var_counter = 0;

    IRStmtBlock* root_ir_block = ir_new_block();
    if (!root_ir_block) {
        fprintf(stderr, "Error: Failed to create root IR block.\n");
        registry_free(ctx.registry);
        return NULL;
    }
    ctx.current_global_block = root_ir_block;

    // 1. Register all components first
    cJSON* components_json = cJSON_GetObjectItem(ui_spec_root, "components");
    if (cJSON_IsObject(components_json)) {
        cJSON* comp_item = NULL;
        for (comp_item = components_json->child; comp_item != NULL; comp_item = comp_item->next) {
            // The key of the component is its name, e.g. "@my_component"
            // The value (comp_item) is the component definition JSON object.
            // registry_add_component expects name without '@'.
            const char* comp_name_with_at = comp_item->string;
            if (comp_name_with_at && comp_name_with_at[0] == '@') {
                 registry_add_component(ctx.registry, comp_name_with_at + 1, comp_item);
            } else {
                fprintf(stderr, "Warning: Component name '%s' does not start with '@'. Skipping component registration.\n", comp_name_with_at ? comp_name_with_at : "NULL_NAME");
            }
        }
    }

    // 2. Process global styles
    cJSON* styles_json = cJSON_GetObjectItem(ui_spec_root, "styles");
    if (styles_json) {
        process_styles(&ctx, styles_json, root_ir_block);
    }

    // 3. Process UI nodes (screens/widgets)
    cJSON* ui_section = cJSON_GetObjectItem(ui_spec_root, "ui");
    if (cJSON_IsObject(ui_section)) {
        cJSON* screen_node_json = NULL;
        for (screen_node_json = ui_section->child; screen_node_json != NULL; screen_node_json = screen_node_json->next) {
            char comment_text[128];
            snprintf(comment_text, sizeof(comment_text), "Generating UI for: %s", screen_node_json->string);
            ir_block_add_stmt(root_ir_block, ir_new_comment(comment_text));
            // Calls to process_node need to be updated to the 6-argument version.
            // parent_block = root_ir_block
            // parent_c_var = NULL (for top-level, will default to lv_scr_act() or similar)
            // default_obj_type = screen_node_json->string (if "ui" is object) or actual type from node.
            // ui_context = NULL (for top-level)
            const char* node_type_str = screen_node_json->string; // If UI section is an object, key is often a screen type/name
                                                              // If node_json itself has a "type" field, that's more reliable.
            cJSON* type_field = cJSON_GetObjectItem(screen_node_json, "type");
            if(type_field) node_type_str = cJSON_GetStringValue(type_field);
            else if (ui_section->type == cJSON_Object) node_type_str = screen_node_json->string; // fallback to key name
            else node_type_str = "obj"; // generic fallback for array items without explicit type

            process_node(&ctx, screen_node_json, root_ir_block, NULL, node_type_str, NULL);
        }
    } else if (cJSON_IsArray(ui_section)) {
         cJSON* screen_node_json = NULL;
         int screen_idx = 0;
         cJSON_ArrayForEach(screen_node_json, ui_section) {
            char comment_text[128];
            snprintf(comment_text, sizeof(comment_text), "Generating UI for screen index: %d", screen_idx++);
            ir_block_add_stmt(root_ir_block, ir_new_comment(comment_text));
            cJSON* type_field = cJSON_GetObjectItem(screen_node_json, "type");
            const char* node_type_str = type_field ? cJSON_GetStringValue(type_field) : "obj";
            process_node(&ctx, screen_node_json, root_ir_block, NULL, node_type_str, NULL);
         }
    } else {
        fprintf(stderr, "Warning: 'ui' section in UI spec is not an object or array. No UI will be generated.\n");
    }

    registry_free(ctx.registry); // Free the registry created by this function
    return root_ir_block;
}

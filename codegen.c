#include "codegen.h"
#include "api_spec.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdlib.h> // For malloc, free

// --- Forward Declarations ---
static void codegen_object(IRObject* obj, const ApiSpec* api_spec, IRRoot* ir_root, int indent, const char* parent_c_name);
// Added expected_c_type for casting registry lookups and other type-aware codegen
static void codegen_expr(IRExpr* expr, const ApiSpec* api_spec, IRRoot* ir_root, IRObject* use_view_context_owner, const char* expected_c_type);
static void print_indent(int level);
static IRObject* clone_ir_object(IRObject* original); // Helper for use-view
static char* sanitize_c_identifier(const char* input_name);

// --- Main Entry Point ---
void codegen_generate_c(IRRoot* root, const ApiSpec* api_spec) {
    if (!root) {
        printf("// No IR root provided.\n");
        return;
    }

    // Process all top-level objects, assuming they are parented to `parent`
    // which should be the main screen/container passed to the UI creation function.
    IRObject* current_obj = root->root_objects;
    while (current_obj) {
        codegen_object(current_obj, api_spec, root, 1, "parent");
        current_obj = current_obj->next;
    }
}

// --- Core Codegen Logic ---

static void codegen_object(IRObject* obj, const ApiSpec* api_spec, IRRoot* ir_root, int indent, const char* parent_c_name) {
    if (!obj) return;

    // Handle 'use-view' by finding and inlining the component
    if (obj->json_type && strcmp(obj->json_type, "use-view") == 0) {
        if (!obj->use_view_component_id) {
            fprintf(stderr, "Codegen Error: use-view object '%s' has no component ID.\n", obj->c_name);
            return;
        }

        // Find component definition in the IR
        IRComponent* comp_def = ir_root->components;
        while(comp_def) {
            if (strcmp(comp_def->id, obj->use_view_component_id) == 0) break;
            comp_def = comp_def->next;
        }

        if (!comp_def || !comp_def->root_widget) {
            fprintf(stderr, "Codegen Error: Component '%s' not found or is empty for use-view '%s'.\n", obj->use_view_component_id, obj->c_name);
            return;
        }

        // Clone the component's root widget to avoid mutating the original IR.
        IRObject* component_root_instance = clone_ir_object(comp_def->root_widget);
        if (!component_root_instance) {
            fprintf(stderr, "Codegen Error: Failed to clone component for use-view '%s'.\n", obj->c_name);
            return;
        }

        // The instance takes the C variable name of the use-view object.
        free(component_root_instance->c_name);
        component_root_instance->c_name = strdup(obj->c_name); // obj->c_name is from use-view's "named" or generated

        // If the use-view node itself was named, register ITS c_name with ITS registered_id.
        // The component_root_instance now effectively IS the use-view instance.
        if (obj->registered_id) {
            print_indent(indent);
            // Use component_root_instance->c_name because that's the actual C variable for the component's root obj.
            printf("obj_registry_add(\"%s\", (void*)%s);\n", obj->registered_id, component_root_instance->c_name);
        }

        // Apply properties from the use-view node as overrides.
        IRProperty* override_prop = obj->properties;
        while(override_prop) {
            // This is a simple append; a real system might replace existing properties.
            ir_property_list_add(&component_root_instance->properties, ir_new_property(override_prop->name, override_prop->value));
            override_prop = override_prop->next;
        }

        codegen_object(component_root_instance, api_spec, ir_root, indent, parent_c_name);

        ir_free((IRNode*)component_root_instance); // Clean up the cloned instance
        return;
    }


    // --- Object Creation ---
    const WidgetDefinition* widget_def = api_spec_find_widget(api_spec, obj->json_type);
    bool is_widget_allocation = false; // Flag to indicate if this is a heap-allocated lv_obj_t* like widget
    print_indent(indent);
    if (widget_def && widget_def->create) { // It's a standard widget
        printf("lv_obj_t* %s = %s(%s);\n", obj->c_name, widget_def->create, parent_c_name ? parent_c_name : "lv_screen_active()");
        is_widget_allocation = true;
    } else if (widget_def && widget_def->c_type && widget_def->init_func) { // It's an object like a style (usually stack allocated or global)
        printf("%s %s;\n", widget_def->c_type, obj->c_name);
        print_indent(indent);
        printf("%s(&%s);\n", widget_def->init_func, obj->c_name);
        // Styles initialized this way are not heap allocated pointers that can be NULL from creation.
    } else { // Fallback to generic object
        printf("lv_obj_t* %s = lv_obj_create(%s);\n", obj->c_name, parent_c_name ? parent_c_name : "lv_screen_active()");
        is_widget_allocation = true;
    }

    if (is_widget_allocation) {
        print_indent(indent);
        printf("if (%s == NULL) render_abort(\"Error: Failed to create widget '%s' (type: %s).\");\n", obj->c_name, obj->c_name, obj->json_type);
    }

    // If the object has a registered ID, add it to the C-side registry
    if (obj->registered_id) {
        print_indent(indent);
        // Cast to void* to be compatible with the generic registry function
        printf("obj_registry_add(\"%s\", (void*)%s);\n", obj->registered_id, obj->c_name);
    }

    // --- Properties ---
    IRProperty* prop = obj->properties;
    int temp_arg_counter = 0; // Counter for unique temporary variable names

    while (prop) {
        const PropertyDefinition* prop_def = api_spec_find_property(api_spec, obj->json_type, prop->name);

        char setter_func[128];
        char temp_setter_name[128];
        bool setter_found = false;

        if (prop_def && prop_def->setter) {
            strncpy(setter_func, prop_def->setter, sizeof(setter_func) - 1);
            setter_func[sizeof(setter_func) - 1] = '\0';
            setter_found = true;
        } else {
            snprintf(temp_setter_name, sizeof(temp_setter_name), "lv_%s_set_%s", obj->json_type, prop->name);
            if (api_spec_has_function(api_spec, temp_setter_name)) {
                strncpy(setter_func, temp_setter_name, sizeof(setter_func) -1);
                setter_func[sizeof(setter_func) -1] = '\0';
                setter_found = true;
            } else {
                if (strcmp(obj->json_type, "obj") != 0) {
                    snprintf(temp_setter_name, sizeof(temp_setter_name), "lv_obj_set_%s", prop->name);
                    if (api_spec_has_function(api_spec, temp_setter_name)) {
                        strncpy(setter_func, temp_setter_name, sizeof(setter_func) - 1);
                        setter_func[sizeof(setter_func) - 1] = '\0';
                        setter_found = true;
                    }
                }
            }
            if (!setter_found) {
                snprintf(setter_func, sizeof(setter_func), "lv_%s_set_%s", obj->json_type, prop->name);
            }
        }

        char obj_ref_for_setter[128];
        if (widget_def && widget_def->init_func) { // e.g. for lv_style_set_...(&style, ...)
            snprintf(obj_ref_for_setter, sizeof(obj_ref_for_setter), "&%s", obj->c_name);
        } else {
            snprintf(obj_ref_for_setter, sizeof(obj_ref_for_setter), "%s", obj->c_name);
        }

        const FunctionArg* first_value_arg_details = NULL;
        if (prop_def && prop_def->func_args && prop_def->func_args->next) {
            first_value_arg_details = prop_def->func_args->next;
        }

        bool is_true_no_arg_setter_scenario = false;
        if (prop_def && !first_value_arg_details) {
            if (prop->value->type == IR_EXPR_LITERAL) {
                IRExprLiteral* lit = (IRExprLiteral*)prop->value;
                if (!lit->is_string && (strcmp(lit->value, "true") == 0 || strcmp(lit->value, "false") == 0)) {
                    is_true_no_arg_setter_scenario = true;
                }
            }
        }

        // Prepare arguments and generate pre-call checks
        char args_c_code[1024] = ""; // Buffer to store C code for arguments
        // char temp_var_declarations[1024] = ""; // Buffer for temporary variable declarations and checks - Unused after refactor

        bool has_args_to_print = false;

        if (first_value_arg_details) {
            strcat(args_c_code, ", ");
            has_args_to_print = true;
            if (prop->value->type == IR_EXPR_ARRAY) {
                IRExprArray* arr = (IRExprArray*)prop->value;
                IRExprNode* elem = arr->elements;
                const FunctionArg* current_arg_details_iter = first_value_arg_details;
                bool first_array_elem = true;
                while(elem) {
                    if (!first_array_elem) strcat(args_c_code, ", ");

                    const char* arg_c_type = current_arg_details_iter ? current_arg_details_iter->type : NULL;
                    bool needs_null_check = arg_c_type && (strcmp(arg_c_type, "lv_obj_t*") == 0 || strcmp(arg_c_type, "lv_style_t*") == 0);

                    char temp_arg_var_name[64];
                    snprintf(temp_arg_var_name, sizeof(temp_arg_var_name), "_arg_val_%d", temp_arg_counter++);

                    if (needs_null_check) {
                        // char decl_buf[256]; // Unused after refactor
                        // Declare temp var
                        print_indent(indent); // Indent for declaration
                        printf("%s %s = ", arg_c_type, temp_arg_var_name);
                        codegen_expr(elem->expr, api_spec, ir_root, obj, arg_c_type);
                        printf(";\n");

                        // Add NULL check for this temp var
                        print_indent(indent); // Indent for check
                        printf("if (%s == NULL) render_abort(\"Error: Argument for '%s' in '%s(%s, ...)' is NULL (property: %s, object: %s).\");\n",
                               temp_arg_var_name, current_arg_details_iter->name, setter_func, obj_ref_for_setter, prop->name, obj->c_name);
                        strcat(args_c_code, temp_arg_var_name);
                    } else {
                        // Directly generate expression for non-checked types
                        // This part is tricky as codegen_expr prints. We need to capture its output.
                        // For simplicity, assume codegen_expr can be called if not needing a check.
                        // This will require codegen_expr to not print for this path, or a new helper.
                        // Let's make codegen_expr print directly into args_c_code for now if no check.
                        // THIS IS A HACK. A better way is a sprintf-like variant of codegen_expr.
                        // For now, I'll print directly and the main printf will just print args_c_code.
                        // This means print_indent(indent) for the main call needs to be careful.

                        // Re-simplification: If no null check, print expression directly in the final call.
                        // The args_c_code will build up the string of arguments.
                        // This part will be tricky. Let's assume for now that if it's not null_checked,
                        // it's generated directly into the final printf.
                        // So, for array elements, if not null_checked, we append to args_c_code by calling codegen_expr
                        // which will print. This means args_c_code doesn't store the C code but is a placeholder.
                        // This entire argument generation section needs a rethink for cleanliness.

                        // Let's stick to the temporary variable approach for *all* arguments for consistency in generation.
                        // This simplifies the final printf call.
                        print_indent(indent);
                        printf("%s %s = ", arg_c_type ? arg_c_type : "void*", temp_arg_var_name); // Use void* if type unknown
                        codegen_expr(elem->expr, api_spec, ir_root, obj, arg_c_type);
                        printf(";\n");
                        strcat(args_c_code, temp_arg_var_name);
                    }

                    first_array_elem = false;
                    if (current_arg_details_iter) current_arg_details_iter = current_arg_details_iter->next;
                    elem = elem->next;
                }
            } else { // Property value is a single expression
                const char* arg_c_type = first_value_arg_details->type;
                bool needs_null_check = arg_c_type && (strcmp(arg_c_type, "lv_obj_t*") == 0 || strcmp(arg_c_type, "lv_style_t*") == 0);

                char temp_arg_var_name[64];
                snprintf(temp_arg_var_name, sizeof(temp_arg_var_name), "_arg_val_%d", temp_arg_counter++);

                print_indent(indent);
                printf("%s %s = ", arg_c_type, temp_arg_var_name);
                codegen_expr(prop->value, api_spec, ir_root, obj, arg_c_type);
                printf(";\n");

                if (needs_null_check) {
                    print_indent(indent);
                    printf("if (%s == NULL) render_abort(\"Error: Argument '%s' for '%s(%s, ...)' is NULL (property: %s, object: %s).\");\n",
                           temp_arg_var_name, first_value_arg_details->name, setter_func, obj_ref_for_setter, prop->name, obj->c_name);
                }
                strcat(args_c_code, temp_arg_var_name);
            }
        } else if (prop_def && !is_true_no_arg_setter_scenario) { // Inferred single argument
            if (!(prop->value->type == IR_EXPR_LITERAL && strcmp(((IRExprLiteral*)prop->value)->value, "NULL") == 0 && !((IRExprLiteral*)prop->value)->is_string)) {
                strcat(args_c_code, ", ");
                has_args_to_print = true;
                // For inferred args, we don't know the type, so no NULL check unless we make assumptions.
                // Let's assume it's not a pointer type needing a check for now.
                char temp_arg_var_name[64];
                snprintf(temp_arg_var_name, sizeof(temp_arg_var_name), "_arg_val_%d", temp_arg_counter++);
                print_indent(indent);
                printf("void* %s = ", temp_arg_var_name); // Use void* as type is unknown
                codegen_expr(prop->value, api_spec, ir_root, obj, NULL);
                printf(";\n");
                strcat(args_c_code, temp_arg_var_name);
            }
        }
        // If is_true_no_arg_setter_scenario, args_c_code remains empty, has_args_to_print is false.

        // Perform the call
        if (is_true_no_arg_setter_scenario) {
            bool call_the_setter_for_toggle = false;
            if (prop->value->type == IR_EXPR_LITERAL) {
                IRExprLiteral* lit = (IRExprLiteral*)prop->value;
                if (strcmp(lit->value, "true") == 0) {
                    call_the_setter_for_toggle = true;
                }
            }
            if (call_the_setter_for_toggle) {
                print_indent(indent);
                printf("%s(%s);\n", setter_func, obj_ref_for_setter);
            } else {
                print_indent(indent);
                printf("/* Property '%s' (value: ", prop->name);
                codegen_expr(prop->value, api_spec, ir_root, obj, NULL); // For comment
                printf(") not called for %s (setter expects no value args, and value was not 'true') */\n", obj->c_name);
            }
        } else {
            print_indent(indent);
            if (has_args_to_print) {
                printf("%s(%s%s);\n", setter_func, obj_ref_for_setter, args_c_code);
            } else {
                 // This case should ideally not be hit if !is_true_no_arg_setter_scenario,
                 // as it implies there should be args or it's an error in logic.
                 // However, if setter_found was true due to a guess and no args were processed.
                printf("%s(%s);\n", setter_func, obj_ref_for_setter);
            }
        }
        prop = prop->next;
    }

    // --- 'with' blocks ---
    IRWithBlock* wb = obj->with_blocks;
    while(wb) {
        char temp_var_name[64];
        snprintf(temp_var_name, sizeof(temp_var_name), "with_target_%s", obj->c_name);

        print_indent(indent);
        printf("{\n");
        print_indent(indent + 1);
        printf("void* %s = ", temp_var_name);
        codegen_expr(wb->target_expr, api_spec, ir_root, obj, NULL); // Pass NULL for expected_c_type for the target expr
        printf(";\n");

        IRProperty* with_prop = wb->properties;
        while(with_prop) {
            // Determine setter function for the 'with' block property
            // For 'with' blocks, we generally assume the target is an 'lv_obj_t*' for properties.
            // A more sophisticated approach might try to infer type from 'wb->target_expr' if it's a function call.
            const char* target_obj_type_for_with = "obj"; // Default assumption for 'with' target
            const PropertyDefinition* with_prop_def = api_spec_find_property(api_spec, target_obj_type_for_with, with_prop->name);

            char setter_func[128];
            if (with_prop_def && with_prop_def->setter) {
                strncpy(setter_func, with_prop_def->setter, sizeof(setter_func) - 1);
                setter_func[sizeof(setter_func) - 1] = '\0';
            } else {
                // Fallback setter name construction (common for lv_obj_set_...)
                snprintf(setter_func, sizeof(setter_func), "lv_%s_set_%s", target_obj_type_for_with, with_prop->name);
            }

            // Argument handling (similar to main properties loop)
            const FunctionArg* with_first_value_arg_details = NULL;
            if (with_prop_def && with_prop_def->func_args && with_prop_def->func_args->next) {
                with_first_value_arg_details = with_prop_def->func_args->next;
            }

            char with_args_c_code[512] = "";
            bool with_has_args_to_print = false;

            if (with_first_value_arg_details) {
                strcat(with_args_c_code, ", ");
                with_has_args_to_print = true;
                // Simplified: assuming single argument for 'with' properties for now. Array values not fully handled here.
                // A full implementation would mirror the complexity of the main property loop.
                const char* with_arg_c_type = with_first_value_arg_details->type;
                bool with_needs_null_check = with_arg_c_type && (strcmp(with_arg_c_type, "lv_obj_t*") == 0 || strcmp(with_arg_c_type, "lv_style_t*") == 0);

                char temp_with_arg_var_name[64];
                snprintf(temp_with_arg_var_name, sizeof(temp_with_arg_var_name), "_with_arg_val_%d", temp_arg_counter++);

                print_indent(indent + 1);
                printf("%s %s = ", with_arg_c_type, temp_with_arg_var_name);
                codegen_expr(with_prop->value, api_spec, ir_root, obj, with_arg_c_type); // obj is context for $vars
                printf(";\n");

                if (with_needs_null_check) {
                    print_indent(indent + 1);
                    printf("if (%s == NULL) render_abort(\"Error: Argument for '%s' in 'with' block call '%s(%s, ...)' is NULL (property: %s, object: %s).\");\n",
                           temp_with_arg_var_name, with_first_value_arg_details->name, setter_func, temp_var_name, with_prop->name, obj->c_name);
                }
                strcat(with_args_c_code, temp_with_arg_var_name);
            } else {
                 // No explicit args from prop_def, but value might be an implicit arg (e.g. color for bg_color)
                 // This path is simplified; a full solution would check for boolean toggles etc.
                if (!(with_prop->value->type == IR_EXPR_LITERAL && strcmp(((IRExprLiteral*)with_prop->value)->value, "NULL") == 0 && !((IRExprLiteral*)with_prop->value)->is_string)) {
                    strcat(with_args_c_code, ", ");
                    with_has_args_to_print = true;
                    char temp_with_arg_var_name[64];
                    snprintf(temp_with_arg_var_name, sizeof(temp_with_arg_var_name), "_with_arg_val_%d", temp_arg_counter++);
                    print_indent(indent+1);
                    printf("void* %s = ", temp_with_arg_var_name); // Use void* as type is unknown
                    codegen_expr(with_prop->value, api_spec, ir_root, obj, NULL);
                    printf(";\n");
                    strcat(with_args_c_code, temp_with_arg_var_name);
                }
            }

            print_indent(indent + 1);
            if (with_has_args_to_print) {
                printf("%s(%s%s);\n", setter_func, temp_var_name, with_args_c_code);
            } else {
                // Assumed to be a no-arg setter (e.g. a flag) or a boolean toggle that resolved to false.
                // For simplicity, if no args processed, assume direct call or already handled toggle.
                // This part might need more refinement for boolean toggles in 'with' blocks.
                printf("%s(%s);\n", setter_func, temp_var_name);
            }
            with_prop = with_prop->next;
        }

        if (wb->children_root) {
            IRObject* child = wb->children_root->children;
            while(child) {
                codegen_object(child, api_spec, ir_root, indent + 1, temp_var_name);
                child = child->next;
            }
        }

        print_indent(indent);
        printf("}\n");
        wb = wb->next;
    }

    // --- Children ---
    if (obj->children) {
        print_indent(indent);
        printf("\n"); // Spacer
        IRObject* child = obj->children;
        while (child) {
            codegen_object(child, api_spec, ir_root, indent, obj->c_name);
            child = child->next;
        }
    }
}

static void codegen_expr(IRExpr* expr, const ApiSpec* api_spec, IRRoot* ir_root, IRObject* use_view_context_owner, const char* expected_c_type) {
    if (!expr) {
        printf("NULL");
        return;
    }
    switch(expr->type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)expr;
            if (lit->is_string) printf("\"%s\"", lit->value);
            else printf("%s", lit->value);
            break;
        }
        case IR_EXPR_STATIC_STRING:
            printf("\"%s\"", ((IRExprStaticString*)expr)->value);
            break;
        case IR_EXPR_ENUM:
            printf("%s", ((IRExprEnum*)expr)->symbol);
            break;
        case IR_EXPR_REGISTRY_REF:
        {
            IRExprRegistryRef* reg_ref_expr = (IRExprRegistryRef*)expr;
            const char* ref_name = reg_ref_expr->name;
            if (ref_name && ref_name[0] == '@') {
                const char* id_to_lookup = ref_name + 1;
                // Check if the expected type is lv_obj_t* or lv_style_t* for NULL checking
                bool needs_null_check = false;
                if (expected_c_type &&
                    (strcmp(expected_c_type, "lv_obj_t*") == 0 || strcmp(expected_c_type, "lv_style_t*") == 0)) {
                    needs_null_check = true;
                }

                if (needs_null_check) {
                    // Generate:
                    // type* temp_val = (type*)obj_registry_get("id");
                    // if (temp_val == NULL) render_abort("...");
                    // temp_val /* this will be used by the caller */
                    char temp_var_name[128];
                    // Create a unique enough temp var name, less critical in C as it's scoped if done right.
                    // For simplicity, using a fixed name here, assuming it's used immediately and doesn't clash.
                    // A real robust solution might involve a counter or passing down a unique ID generator.
                    snprintf(temp_var_name, sizeof(temp_var_name), "_tmp_reg_%s", sanitize_c_identifier(id_to_lookup));

                    // This output is tricky because codegen_expr is supposed to print an expression.
                    // So, we print the assignment and check here IF it's part of a larger statement generated by codegen_object.
                    // This direct print within codegen_expr for assignments and checks is a simplification.
                    // Ideally, codegen_expr would return a structure that codegen_object uses.
                    // For now, we assume this is directly usable in the function call argument list.
                    // The caller (codegen_object) will need to handle emitting the temp var declaration and check *before* the call.
                    // This means codegen_expr will just output the temp_var_name.
                    // The actual declaration and check needs to be hoisted.
                    // THIS IS A MAJOR REFACTORING POINT if we want it clean.
                    // Given current structure, the easiest (but less clean) is to have codegen_expr print the var name,
                    // and have codegen_object handle the pre-call setup.
                    // Let's try to make codegen_expr itself output the lookup and then the variable.
                    // This means codegen_expr can't just be used for simple values anymore if it has side effects.

                    // Revised approach: codegen_expr will output the expression to get the value,
                    // and the NULL check will be handled by codegen_object's property handling.
                    // So, just print the cast and the registry get.
                    if (expected_c_type && strchr(expected_c_type, '*')) {
                         printf("(%s)", expected_c_type);
                    }
                    printf("obj_registry_get(\"%s\")", id_to_lookup);

                } else {
                    // No NULL check needed, or type is not obj/style pointer
                    if (expected_c_type && strchr(expected_c_type, '*')) {
                        printf("(%s)", expected_c_type); // Print cast, e.g. (lv_style_t*)
                    }
                    printf("obj_registry_get(\"%s\")", id_to_lookup); // Skip the '@'
                }
            } else {
                fprintf(stderr, "Warning: Registry reference '%s' does not start with '@'. Generating as raw name.\n", ref_name ? ref_name : "NULL");
                printf("%s", ref_name ? ref_name : "NULL"); // Might lead to compile error if var undefined
            }
            break;
        }
        case IR_EXPR_CONTEXT_VAR: {
            const char* var_name = ((IRExprContextVar*)expr)->name;
            if (use_view_context_owner && use_view_context_owner->use_view_context) {
                IRProperty* ctx_prop = use_view_context_owner->use_view_context;
                while(ctx_prop) {
                    if (strcmp(ctx_prop->name, var_name) == 0) {
                        // When resolving context var, we don't know its expected type in *this* specific usage,
                        // but the original 'expected_c_type' for the outer property still applies somewhat.
                        codegen_expr(ctx_prop->value, api_spec, ir_root, use_view_context_owner, expected_c_type);
                        return;
                    }
                    ctx_prop = ctx_prop->next;
                }
            }
            printf("/* Context var '%s' not found */ NULL", var_name);
            break;
        }
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;
            printf("%s(", call->func_name);
            IRExprNode* arg = call->args;
            const FunctionArg* first_arg_for_call = api_spec_get_function_args_by_name(api_spec, call->func_name);
            const FunctionArg* current_func_arg_type = first_arg_for_call;
            while(arg) {
                codegen_expr(arg->expr, api_spec, ir_root, use_view_context_owner, current_func_arg_type ? current_func_arg_type->type : NULL);
                if (arg->next) printf(", ");
                if (current_func_arg_type) current_func_arg_type = current_func_arg_type->next;
                arg = arg->next;
            }
            printf(")");
            break;
        }
        case IR_EXPR_ARRAY: {
            // This case is now primarily for array initializers like { val1, val2 }
            // If an array is used for multiple function arguments, codegen_object's property loop handles it.
            IRExprArray* arr = (IRExprArray*)expr;
            IRExprNode* elem = arr->elements;
            printf("{ ");
            bool first = true;
            while(elem) {
                if (!first) printf(", ");
                // For elements of an array initializer, expected_c_type of the outer context doesn't apply to individual elements.
                // Pass NULL for expected_c_type for elements.
                codegen_expr(elem->expr, api_spec, ir_root, use_view_context_owner, NULL);
                first = false;
                elem = elem->next;
            }
            printf(" }");
            break;
        }
        default:
            printf("/* unhandled expr type %d */", expr->type);
    }
}

// --- Helpers ---
static void print_indent(int level) {
    for (int i = 0; i < level; ++i) {
        printf("    "); // 4 spaces
    }
}

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

static IRExpr* clone_ir_expr(IRExpr* original_expr);
static IRProperty* clone_ir_property_list(IRProperty* original_props);
static IRObject* clone_ir_object_list(IRObject* original_objs);
static IRWithBlock* clone_ir_with_block_list(IRWithBlock* original_blocks);

static IRExprNode* clone_ir_expr_list(IRExprNode* original_expr_list) {
    if (!original_expr_list) return NULL;
    IRExprNode* head = NULL;
    IRExprNode* current_new = NULL;
    IRExprNode* current_original = original_expr_list;
    while (current_original) {
        IRExpr* cloned_expr = clone_ir_expr(current_original->expr);
        IRExprNode* new_node = calloc(1, sizeof(IRExprNode));
        new_node->expr = cloned_expr;
        new_node->next = NULL;
        if (!head) {
            head = new_node;
            current_new = head;
        } else {
            current_new->next = new_node;
            current_new = new_node;
        }
        current_original = current_original->next;
    }
    return head;
}

static IRExpr* clone_ir_expr(IRExpr* original_expr) {
    if (!original_expr) return NULL;
    switch (original_expr->type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* orig_lit = (IRExprLiteral*)original_expr;
            IRExprLiteral* new_lit = (IRExprLiteral*)ir_new_expr_literal(orig_lit->value);
            new_lit->is_string = orig_lit->is_string; // ir_new_expr_literal sets is_string to false by default
            if (orig_lit->is_string) { // If original was string literal, use the right constructor
                free(new_lit); // free the one from ir_new_expr_literal
                new_lit = (IRExprLiteral*)ir_new_expr_literal_string(orig_lit->value);
            }
            return (IRExpr*)new_lit;
        }
        case IR_EXPR_STATIC_STRING:
            return ir_new_expr_static_string(((IRExprStaticString*)original_expr)->value);
        case IR_EXPR_ENUM:
            return ir_new_expr_enum(((IRExprEnum*)original_expr)->symbol, ((IRExprEnum*)original_expr)->value);
        case IR_EXPR_REGISTRY_REF:
            return ir_new_expr_registry_ref(((IRExprRegistryRef*)original_expr)->name);
        case IR_EXPR_CONTEXT_VAR:
            return ir_new_expr_context_var(((IRExprContextVar*)original_expr)->name);
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* orig_call = (IRExprFunctionCall*)original_expr;
            IRExprNode* cloned_args = clone_ir_expr_list(orig_call->args);
            return ir_new_expr_func_call(orig_call->func_name, cloned_args);
        }
        case IR_EXPR_ARRAY: {
            IRExprArray* orig_arr = (IRExprArray*)original_expr;
            IRExprNode* cloned_elements = clone_ir_expr_list(orig_arr->elements);
            return ir_new_expr_array(cloned_elements);
        }
        default:
            fprintf(stderr, "clone_ir_expr: Unknown expression type %d\n", original_expr->type);
            return NULL;
    }
}

static IRProperty* clone_ir_property_list(IRProperty* original_props) {
    if (!original_props) return NULL;
    IRProperty* head = NULL;
    IRProperty* current_new = NULL;
    IRProperty* current_original = original_props;
    while (current_original) {
        IRExpr* cloned_value = clone_ir_expr(current_original->value);
        IRProperty* new_prop = ir_new_property(current_original->name, cloned_value);
        if (!head) {
            head = new_prop;
            current_new = head;
        } else {
            current_new->next = new_prop;
            current_new = new_prop;
        }
        current_original = current_original->next;
    }
    return head;
}


static IRObject* clone_ir_object(IRObject* original); // Forward declare for mutual recursion if needed by with_blocks or children

static IRWithBlock* clone_ir_with_block_list(IRWithBlock* original_blocks) {
    if (!original_blocks) return NULL;
    IRWithBlock* head = NULL;
    IRWithBlock* current_new = NULL;
    IRWithBlock* current_original = original_blocks;
    while (current_original) {
        IRExpr* cloned_target_expr = clone_ir_expr(current_original->target_expr);
        IRProperty* cloned_properties = clone_ir_property_list(current_original->properties);
        IRObject* cloned_children_root = clone_ir_object(current_original->children_root); // Recursive call
        IRWithBlock* new_block = ir_new_with_block(cloned_target_expr, cloned_properties, cloned_children_root);

        if (!head) {
            head = new_block;
            current_new = head;
        } else {
            current_new->next = new_block;
            current_new = new_block;
        }
        current_original = current_original->next;
    }
    return head;
}

static IRObject* clone_ir_object_list(IRObject* original_objs) {
    if (!original_objs) return NULL;
    IRObject* head = NULL;
    IRObject* current_new = NULL;
    IRObject* current_original = original_objs;
    while (current_original) {
        IRObject* new_obj = clone_ir_object(current_original); // Recursive call
        if (!head) {
            head = new_obj;
            current_new = head;
        } else {
            current_new->next = new_obj;
            current_new = new_obj;
        }
        current_original = current_original->next;
    }
    return head;
}


// Deep clone for IRObject
static IRObject* clone_ir_object(IRObject* original) {
    if (!original) return NULL;
    IRObject* clone = ir_new_object(original->c_name, original->json_type, original->registered_id);

    if (original->use_view_component_id) {
        clone->use_view_component_id = strdup(original->use_view_component_id);
    }
    clone->use_view_context = clone_ir_property_list(original->use_view_context);
    clone->properties = clone_ir_property_list(original->properties);
    clone->children = clone_ir_object_list(original->children);
    clone->with_blocks = clone_ir_with_block_list(original->with_blocks);

    return clone;
}

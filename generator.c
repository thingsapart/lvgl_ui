#include "generator.h"
#include "ir.h"
#include "api_spec.h"
#include "registry.h"
#include "debug_log.h"
#include "lvgl_ui_utils.h"
#include "yaml_parser.h"
#include "ui_sim.h" // ADDED: For UI-Sim processing
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdarg.h>

// --- Global Configuration (from main.c) ---
extern bool g_strict_mode;
extern bool g_strict_registry_mode;

// --- Generation Context ---
typedef struct {
    const ApiSpec* api_spec;
    Registry* registry;
    int var_counter;
    bool error_occurred; // Flag to stop processing on error
} GenContext;

// --- Forward Declarations ---
static IRObject* parse_object(GenContext* ctx, cJSON* obj_json, const char* parent_c_name, const cJSON* ui_context, const char* current_base_path);
static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* expected_c_type, const char* parent_c_name, const char* target_c_name, IRObject* ir_obj_for_warnings);
static char* generate_unique_var_name(GenContext* ctx, const char* base_type);
static char* sanitize_c_identifier(const char* input_name);
static void process_and_validate_call(GenContext* ctx, const char* func_name, IRExprNode** args_list_ptr, IRObject* ir_obj_for_warnings);
static void merge_json_objects(cJSON* dest, const cJSON* source);
static int count_cjson_array(cJSON* array_json);
static int count_function_args(const FunctionArg* head);
static bool types_compatible(const char* expected, const char* actual);
static cJSON* process_context_keys_recursive(const cJSON* source_json, const cJSON* context);
IRRoot* generate_ir_from_string_with_base_path(const char* ui_spec_string, const char* base_path, const ApiSpec* api_spec);
static void process_ui_spec_array(GenContext* ctx, cJSON* array_json, const char* current_base_path, IRObject** object_list_head, IROperationNode** operation_list_head, const char* parent_c_name, const cJSON* ui_context);


// --- Refactored Path Interpolation Helper ---
/**
 * @brief Checks if a path string starts with "S:~/" and expands it to "S:<HOME_DIR>/...".
 * @param path The input path string.
 * @return A new, heap-allocated string with the expanded path, or NULL if no expansion was needed.
 *         The caller is responsible for freeing the returned string.
 */
static char* interpolate_home_path(const char* path) {
    if (!path) return NULL;
    if (strncmp(path, "S:~/", 4) == 0) {
        const char* home_dir = NULL;
#ifdef _WIN32
        home_dir = getenv("USERPROFILE");
#else
        home_dir = getenv("HOME");
#endif

        if (home_dir) {
            const char* rest_of_path = path + 3; // The part starting with "/" after "~"
            size_t home_len = strlen(home_dir);
            size_t rest_len = strlen(rest_of_path);

            // Allocate buffer for "S:" + home_dir + rest_of_path + '\0'
            size_t new_path_len = 2 + home_len + rest_len;
            char* new_path = malloc(new_path_len + 1);
            if (!new_path) return NULL; // OOM

            snprintf(new_path, new_path_len + 1, "S:%s%s", home_dir, rest_of_path);

            // Normalize path separators to forward slashes for consistency
            for (char* p = new_path; *p; p++) {
                if (*p == '\\') {
                    *p = '/';
                }
            }
            return new_path;
        }
    }
    return NULL;
}


/* Forward declaration needed because expand_includes_in_array calls this
 * function which is defined later in the file. */
static void prefix_ids_recursive(cJSON* node, const char* prefix);

/**
 * Expand top-level include directives in an array by replacing the include
 * entry with the array contents of the included file. Returns true on
 * success, false on fatal error (render_abort called).
 */
static bool expand_includes_in_array(cJSON* array, const char* base_path) {
    if (!array || !cJSON_IsArray(array)) return true;

    int i = 0;
    while (i < cJSON_GetArraySize(array)) {
        cJSON* item = cJSON_GetArrayItem(array, i);
        if (!item) { i++; continue; }

        cJSON* include_item = cJSON_GetObjectItem(item, "include");
        if (!include_item) { i++; continue; }

        // Ensure include is standalone
        int other_keys = 0;
        cJSON* tmp = NULL;
        cJSON_ArrayForEach(tmp, item) {
            if (!tmp->string) continue;
            if (strncmp(tmp->string, "//", 2) == 0) continue;
            if (strcmp(tmp->string, "include") == 0) continue;
            other_keys++;
        }
        if (other_keys > 0) {
            render_abort("Include directive must be a standalone item with no other keys.");
            return false;
        }

        const char* include_file = NULL;
        const cJSON* include_context = NULL;
        const char* as_prefix = NULL;

        if (cJSON_IsString(include_item)) {
            include_file = include_item->valuestring;
        } else if (cJSON_IsObject(include_item)) {
            cJSON* f = cJSON_GetObjectItemCaseSensitive((cJSON*)include_item, "file");
            if (f && cJSON_IsString(f)) include_file = f->valuestring;
            cJSON* ctx_item = cJSON_GetObjectItemCaseSensitive((cJSON*)include_item, "context");
            if (ctx_item && cJSON_IsObject(ctx_item)) include_context = ctx_item;
            cJSON* as_item = cJSON_GetObjectItemCaseSensitive((cJSON*)include_item, "as");
            if (as_item && cJSON_IsString(as_item)) as_prefix = as_item->valuestring;
        } else {
            render_abort("Unsupported include directive type; expected string or mapping.");
            return false;
        }

        if (!include_file) {
            render_abort("Include directive missing 'file' entry.");
            return false;
        }

        // Resolve path relative to base_path and expand S:~/ style
        char* expanded_include = interpolate_home_path(include_file);
        char* include_copy = expanded_include ? expanded_include : strdup(include_file);
        char* full_path = join_path(base_path, include_copy);
        if (expanded_include) free(expanded_include);
        free(include_copy);

        char* included_content = read_file(full_path);
        if (!included_content) {
            char err_buf[512];
            snprintf(err_buf, sizeof(err_buf), "Could not read include file: %s", full_path);
            render_abort(err_buf);
            free(full_path);
            return false;
        }

        char* error_msg = NULL;
        cJSON* included_json = yaml_to_cjson(included_content, &error_msg);
        free(included_content);
        if (error_msg) {
            char err_buf[1024];
            snprintf(err_buf, sizeof(err_buf), "Error in included file '%s': %s", full_path, error_msg);
            render_abort(err_buf);
            free(error_msg);
            free(full_path);
            if (included_json) cJSON_Delete(included_json);
            return false;
        }

        if (!included_json || !cJSON_IsArray(included_json)) {
            char err_buf[512];
            snprintf(err_buf, sizeof(err_buf), "Included file '%s' does not contain a top-level YAML/JSON list.", full_path);
            render_abort(err_buf);
            if (included_json) cJSON_Delete(included_json);
            free(full_path);
            return false;
        }

        // Recursively expand nested includes inside the included file
        char* new_base = get_dirname(full_path);
        if (!expand_includes_in_array(included_json, new_base)) {
            free(new_base);
            cJSON_Delete(included_json);
            free(full_path);
            return false;
        }

        // Optionally prefix ids inside included content
        if (as_prefix) prefix_ids_recursive(included_json, as_prefix);

        // Duplicate included items and (optionally) attach include_context
        int included_count = cJSON_GetArraySize(included_json);
        cJSON** dup_items = calloc(included_count, sizeof(cJSON*));
        for (int j = 0; j < included_count; j++) {
            cJSON* src = cJSON_GetArrayItem(included_json, j);
            dup_items[j] = cJSON_Duplicate(src, true);
            if (include_context && cJSON_IsObject(include_context) && dup_items[j] && cJSON_IsObject(dup_items[j])) {
                cJSON_AddItemToObject(dup_items[j], "__include_context", cJSON_Duplicate((cJSON*)include_context, true));
            }
        }

        // Remove the include entry and splice duplicated items in its place
        cJSON_DeleteItemFromArray(array, i);
        for (int j = 0; j < included_count; j++) {
            cJSON_InsertItemInArray(array, i + j, dup_items[j]);
        }

        free(dup_items);
        cJSON_Delete(included_json);
        free(new_base);
        free(full_path);

        // Advance index past inserted items
        i += included_count;
    }
    return true;
}


// --- Main Entry Point ---

IRRoot* generate_ir_from_ui_spec(const cJSON* ui_spec_root, const ApiSpec* api_spec) {
    if (!ui_spec_root || !cJSON_IsArray(ui_spec_root)) {
        render_abort("UI spec root must be a valid JSON array.");
        return NULL;
    }
    if (!api_spec) {
        render_abort("API spec is NULL.");
        return NULL;
    }

    IRRoot* ir_root = ir_new_root();
    if (!ir_root) {
        render_abort("Failed to create IR Root.");
        return NULL;
    }

    GenContext ctx = { .api_spec = api_spec, .registry = registry_create(), .var_counter = 0, .error_occurred = false };
    if (!ctx.registry) {
        ir_free((IRNode*)ir_root);
        render_abort("Failed to create registry.");
        return NULL;
    }

    // Pre-pass to find and register all components
    cJSON* item_json = NULL;
    cJSON_ArrayForEach(item_json, ui_spec_root) {
        if (cJSON_IsObject(item_json)) {
            cJSON* type_item = cJSON_GetObjectItemCaseSensitive(item_json, "type");
            if (type_item && cJSON_IsString(type_item)) {
                if (strcmp(type_item->valuestring, "component") == 0) {
                    cJSON* id_item = cJSON_GetObjectItemCaseSensitive(item_json, "id");
                    cJSON* content_item = cJSON_GetObjectItemCaseSensitive(item_json, "root");
                    if (!content_item) content_item = cJSON_GetObjectItemCaseSensitive(item_json, "content");
                    if (id_item && cJSON_IsString(id_item) && content_item && cJSON_IsObject(content_item)) {
                        registry_add_component(ctx.registry, id_item->valuestring, content_item);
                        DEBUG_LOG(LOG_MODULE_GENERATOR, "Registered component: %s", id_item->valuestring);
                    } else {
                      if (!id_item) {
                        print_warning("Found 'component' with missing 'id'.");
                      } else if (!cJSON_IsString(id_item)) {
                        print_warning("Found 'component' with 'id' that is not a string.");
                      }
                      if (!content_item) {
                        print_warning("Found 'component' with missing 'root'/'content'.");

                      } else if (!cJSON_IsObject(content_item)) {
                        print_warning("Found 'component' with 'root'/'content' that is not an object (got array or scalar instead).");
                      }
                    }
                }
            }
        }
    }


    const char* root_parent_name = "parent";
    registry_add_generated_var(ctx.registry, root_parent_name, root_parent_name, "lv_obj_t*");


    process_ui_spec_array(&ctx, (cJSON*)ui_spec_root, ".", &ir_root->root_objects, NULL, root_parent_name, NULL);


    registry_free(ctx.registry);

    if (ctx.error_occurred) {
        ir_free((IRNode*)ir_root);
        return NULL;
    }

    return ir_root;
}

IRRoot* generate_ir_from_string_with_base_path(const char* ui_spec_string, const char* base_path, const ApiSpec* api_spec) {
    if (!ui_spec_string || strlen(ui_spec_string) == 0) {
        return ir_new_root();
    }

    cJSON* ui_spec_json = NULL;
    char* error_msg = NULL;

    const char* p = ui_spec_string;
    while (*p && isspace((unsigned char)*p)) p++;

    bool tried_json = false;
    if (*p == '{' || *p == '[') {
        tried_json = true;
        ui_spec_json = cJSON_Parse(ui_spec_string);
        if (!ui_spec_json) {
            ui_spec_json = yaml_to_cjson(ui_spec_string, &error_msg);
        }
    } else {
        ui_spec_json = yaml_to_cjson(ui_spec_string, &error_msg);
    }

    if (error_msg) {
        render_abort(error_msg);
        free(error_msg);
        cJSON_Delete(ui_spec_json);
        return NULL;
    }

    if (!ui_spec_json) {
        if (tried_json) render_abort(cJSON_GetErrorPtr());
        else render_abort("Failed to parse UI specification. Content is not valid YAML or JSON.");
        return NULL;
    }

    if (!cJSON_IsArray(ui_spec_json)) {
        render_abort("UI spec root must be a valid JSON/YAML array.");
        cJSON_Delete(ui_spec_json);
        return NULL;
    }

    // Pre-expand any include directives so subsequent pre-passes (component
    // registration etc.) see the included items as if they were inline.
    if (!expand_includes_in_array(ui_spec_json, base_path)) {
        cJSON_Delete(ui_spec_json);
        return NULL;
    }

    IRRoot* ir_root = ir_new_root();
    GenContext ctx = { .api_spec = api_spec, .registry = registry_create(), .var_counter = 0, .error_occurred = false };

    // Pre-pass for components...
    cJSON* item_json = NULL;
    cJSON_ArrayForEach(item_json, ui_spec_json) {
        if (cJSON_IsObject(item_json)) {
            cJSON* type_item = cJSON_GetObjectItemCaseSensitive(item_json, "type");
            if (type_item && cJSON_IsString(type_item) && strcmp(type_item->valuestring, "component") == 0) {
                cJSON* id_item = cJSON_GetObjectItemCaseSensitive(item_json, "id");
                cJSON* content_item = cJSON_GetObjectItemCaseSensitive(item_json, "root");
                if (!content_item) content_item = cJSON_GetObjectItemCaseSensitive(item_json, "content");
                if (id_item && cJSON_IsString(id_item) && content_item && cJSON_IsObject(content_item)) {
                    registry_add_component(ctx.registry, id_item->valuestring, content_item);
                } else {
                    if (!id_item || !cJSON_IsString(id_item))
                        print_warning("Found 'component' with missing or non-string 'id'.");
                    if (!content_item)
                        print_warning("Found 'component' with missing 'root'/'content'.");
                    else if (!cJSON_IsObject(content_item))
                        print_warning("Found 'component' with 'root'/'content' that is not an object (got array or scalar instead).");
                }
            }
        }
    }

    const char* root_parent_name = "parent";
    registry_add_generated_var(ctx.registry, root_parent_name, root_parent_name, "lv_obj_t*");
    process_ui_spec_array(&ctx, ui_spec_json, base_path, &ir_root->root_objects, NULL, root_parent_name, NULL);

    registry_free(ctx.registry);
    cJSON_Delete(ui_spec_json);

    if (ctx.error_occurred) {
        ir_free((IRNode*)ir_root);
        return NULL;
    }

    // Validate IR for dynamic-dispatch requirements: enums/constants must carry numeric values
    if (!ir_validate_for_dynamic_dispatch(ir_root)) {
        ir_free((IRNode*)ir_root);
        render_abort("IR validation for dynamic dispatch failed: missing numeric values for some constants.");
        return NULL;
    }

    return ir_root;
}


IRRoot* generate_ir_from_string(const char* ui_spec_string, const ApiSpec* api_spec) {
    return generate_ir_from_string_with_base_path(ui_spec_string, ".", api_spec);
}

IRRoot* generate_ir_from_file(const char* ui_spec_path, const ApiSpec* api_spec) {
    char* ui_spec_content = read_file(ui_spec_path);
    if (!ui_spec_content) {
        char err_buf[512];
        snprintf(err_buf, sizeof(err_buf), "Error reading UI spec file: %s", ui_spec_path);
        render_abort(err_buf);
        return NULL;
    }

    char* base_path = get_dirname(ui_spec_path);
    IRRoot* ir_root = generate_ir_from_string_with_base_path(ui_spec_content, base_path, api_spec);
    free(base_path);
    free(ui_spec_content);

    if (!ir_root) {
        DEBUG_LOG(LOG_MODULE_GENERATOR, "Failed to generate IR from the UI spec file '%s'.", ui_spec_path);
    }

    return ir_root;
}

// --- NEW ---
/**
 * @brief Recursively traverses a cJSON structure, creating a deep copy.
 * During the copy, it inspects every object key. If a key starts with '$',
 * it attempts to replace it with the corresponding string value from the context.
 *
 * @param source_json The cJSON structure to process.
 * @param context The cJSON object containing context variables.
 * @return A new, fully processed cJSON structure. The caller is responsible for deleting it.
 */
static cJSON* process_context_keys_recursive(const cJSON* source_json, const cJSON* context) {
    if (!source_json) {
        return NULL;
    }

    if (cJSON_IsObject(source_json)) {
        cJSON* new_obj = cJSON_CreateObject();
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, source_json) {
            const char* original_key = item->string;
            const char* final_key = original_key;

            if (original_key && original_key[0] == '$') {
                // Support new syntax `$name/doc-info` where a slash introduces
                // an inline doc string. For lookup we strip the slash and the
                // trailing doc so that `$foo/description` maps to context key
                // `foo`.
                const char* raw = original_key + 1;
                const char* slash = strchr(raw, '/');
                size_t varlen = slash ? (size_t)(slash - raw) : strlen(raw);
                char varbuf[256];
                if (varlen >= sizeof(varbuf)) varlen = sizeof(varbuf) - 1;
                memcpy(varbuf, raw, varlen);
                varbuf[varlen] = '\0';
                cJSON* context_val_item = cJSON_GetObjectItem(context, varbuf);
                if (context_val_item && cJSON_IsString(context_val_item)) {
                    final_key = context_val_item->valuestring;
                }
            }

            cJSON* new_value = process_context_keys_recursive(item, context);
            cJSON_AddItemToObject(new_obj, final_key, new_value);
        }
        return new_obj;
    }

    if (cJSON_IsArray(source_json)) {
        cJSON* new_arr = cJSON_CreateArray();
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, source_json) {
            cJSON* new_item = process_context_keys_recursive(item, context);
            cJSON_AddItemToArray(new_arr, new_item);
        }
        return new_arr;
    }

    // For non-container types (string, number, bool, null), just duplicate.
    return cJSON_Duplicate(source_json, true);
}

// Recursively prefix `id` string values in objects with given prefix.
static void prefix_ids_recursive(cJSON* node, const char* prefix) {
    if (!node || !prefix) return;
    if (cJSON_IsObject(node)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, node) {
            if (item->string && strcmp(item->string, "id") == 0 && cJSON_IsString(item)) {
                const char* orig = item->valuestring;
                // Do not prefix ids that start with '@' (intentional global id)
                if (orig && orig[0] == '@') continue;
                size_t newlen = strlen(prefix) + 1 + strlen(orig) + 1;
                char* buf = malloc(newlen);
                if (!buf) continue;
                snprintf(buf, newlen, "%s_%s", prefix, orig);
                cJSON_ReplaceItemInObject(node, "id", cJSON_CreateString(buf));
                free(buf);
            } else {
                prefix_ids_recursive(item, prefix);
            }
        }
        return;
    }
    if (cJSON_IsArray(node)) {
        cJSON* it = NULL;
        cJSON_ArrayForEach(it, node) {
            prefix_ids_recursive(it, prefix);
        }
    }
}


// --- Core Object Parser ---

static IRObject* parse_object(GenContext* ctx, cJSON* obj_json, const char* parent_c_name, const cJSON* ui_context, const char* current_base_path) {
    if (ctx->error_occurred) return NULL;
    if (!cJSON_IsObject(obj_json)) return NULL;

    const char* original_json_type_str = "obj";
    cJSON* type_item = cJSON_GetObjectItem(obj_json, "type");
    if (type_item && cJSON_IsString(type_item)) {
        original_json_type_str = type_item->valuestring;
    }

    /*
    // --- Handle special asset types ---
    if (strcmp(original_json_type_str, "image") == 0) {
        cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
        cJSON* path_item = cJSON_GetObjectItem(obj_json, "path");
        if (!id_item || !cJSON_IsString(id_item) || !path_item || !cJSON_IsString(path_item)) {
            print_warning("'image' type requires a string 'id' and 'path'.");
            return NULL;
        }
        const char* registered_id_from_json = id_item->valuestring;
        char* c_name = generate_unique_var_name(ctx, registered_id_from_json);
        const char* clean_id = (registered_id_from_json[0] == '@') ? registered_id_from_json + 1 : registered_id_from_json;
        IRObject* ir_obj = ir_new_object(c_name, "image", "const char*", clean_id);
        ir_obj->constructor_expr = ir_new_expr_literal_string(path_item->valuestring, strlen(path_item->valuestring));
        registry_add_generated_var(ctx->registry, registered_id_from_json, ir_obj->c_name, ir_obj->c_type);
        free(c_name);
        return ir_obj;
    }
    */

    if (strcmp(original_json_type_str, "font") == 0) {
        cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
        cJSON* path_item = cJSON_GetObjectItem(obj_json, "path");
        cJSON* size_item = cJSON_GetObjectItem(obj_json, "size");
        if (!id_item || !cJSON_IsString(id_item) || !path_item || !cJSON_IsString(path_item) || !size_item || !cJSON_IsNumber(size_item)) {
            print_warning("'font' type requires a string 'id', a string 'path', and a numeric 'size'.");
            return NULL;
        }
        const char* registered_id_from_json = id_item->valuestring;
        char* c_name = generate_unique_var_name(ctx, registered_id_from_json);
        const char* clean_id = (registered_id_from_json[0] == '@') ? registered_id_from_json + 1 : registered_id_from_json;
        IRObject* ir_obj = ir_new_object(c_name, "font", "const lv_font_t*", clean_id);

        const char* original_path = path_item->valuestring;
        char* interpolated_path = interpolate_home_path(original_path);
        const char* final_path = interpolated_path ? interpolated_path : original_path;

        IRExprNode* args = NULL;
        ir_expr_list_add(&args, ir_new_expr_literal_string(final_path, strlen(final_path)));
        char size_buf[16];
        snprintf(size_buf, sizeof(size_buf), "%d", (int)size_item->valuedouble);
        ir_expr_list_add(&args, ir_new_expr_literal(size_buf, "lv_font_size_t"));

        ir_obj->constructor_expr = ir_new_expr_func_call("lv_tiny_ttf_create_file", args, "lv_font_t*");
        registry_add_generated_var(ctx->registry, registered_id_from_json, ir_obj->c_name, ir_obj->c_type);

        if (interpolated_path) free(interpolated_path);
        free(c_name);
        return ir_obj;
    }

    // --- Handle `use-view` directive first, as it's not a real widget type ---
    if (strcmp(original_json_type_str, "use-view") == 0) {
        cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
        if (!id_item || !cJSON_IsString(id_item)) {
            print_warning("'use-view' requires a string 'id'.");
            return NULL;
        }
        const cJSON* component_content = registry_get_component(ctx->registry, id_item->valuestring);
        if (!component_content) {
            print_warning("Component '%s' not found for 'use-view'.", id_item->valuestring);
            return NULL;
        }

        cJSON* new_context = cJSON_CreateObject();
        if (ui_context) merge_json_objects(new_context, ui_context);
        cJSON* local_context = cJSON_GetObjectItem(obj_json, "context");
        if (local_context) merge_json_objects(new_context, local_context);

        // NEW: Process the component template with the context to substitute keys
        cJSON* processed_template = process_context_keys_recursive(component_content, new_context);

        // The final JSON object we will parse. Start with the processed template.
        cJSON* final_json = processed_template;

        // Now, merge/override properties from the use-view block itself onto the processed template
        cJSON* prop_item = NULL;
        cJSON_ArrayForEach(prop_item, obj_json) {
            const char* key = prop_item->string;
            if (strncmp(key, "//", 2) == 0) continue;

            if (strcmp(key, "children") == 0) {
                if (cJSON_IsArray(prop_item)) {
                    cJSON* final_children = cJSON_GetObjectItem(final_json, "children");
                    if (!final_children) {
                        final_children = cJSON_CreateArray();
                        cJSON_AddItemToObject(final_json, "children", final_children);
                    }
                    if (cJSON_IsArray(final_children)) {
                        cJSON* child_to_add;
                        cJSON_ArrayForEach(child_to_add, prop_item) {
                            cJSON_AddItemToArray(final_children, cJSON_Duplicate(child_to_add, true));
                        }
                    }
                }
                continue;
            }

            if (strcmp(key, "type") == 0 || strcmp(key, "id") == 0 || strcmp(key, "context") == 0) continue;

            if (cJSON_HasObjectItem(final_json, key)) {
                cJSON_ReplaceItemInObject(final_json, key, cJSON_Duplicate(prop_item, true));
            } else {
                cJSON_AddItemToObject(final_json, key, cJSON_Duplicate(prop_item, true));
            }
        }

        IRObject* generated_obj = parse_object(ctx, final_json, parent_c_name, new_context, current_base_path);

        cJSON_Delete(final_json);
        cJSON_Delete(new_context);
        return generated_obj;
    }

    // --- It's a regular object. ---
    const WidgetDefinition* widget_def = api_spec_find_widget(ctx->api_spec, original_json_type_str);
    bool fallback_to_obj = false;
    if (!widget_def && strcmp(original_json_type_str, "obj") != 0) {
        fallback_to_obj = true;
        widget_def = api_spec_find_widget(ctx->api_spec, "obj");
        if (!widget_def) {
            render_abort("API Spec is missing the fundamental 'obj' definition.");
            ctx->error_occurred = true;
            return NULL;
        }
    }

    cJSON* new_scope_context = cJSON_CreateObject();
    if(ui_context) merge_json_objects(new_scope_context, ui_context);
    // If this object was produced from an include, it may carry a
    // synthetic '__include_context' object that should be merged into
    // the active ui context for this item's parsing.
    cJSON* include_ctx_item = cJSON_GetObjectItemCaseSensitive(obj_json, "__include_context");
    if (include_ctx_item && cJSON_IsObject(include_ctx_item)) merge_json_objects(new_scope_context, include_ctx_item);
    cJSON* local_context = cJSON_GetObjectItem(obj_json, "context");
    if (local_context && cJSON_IsObject(local_context)) merge_json_objects(new_scope_context, local_context);

    cJSON* init_item = cJSON_GetObjectItem(obj_json, "init");
    cJSON* id_item = cJSON_GetObjectItem(obj_json, "id");
    if (!id_item) id_item = cJSON_GetObjectItem(obj_json, "name");
    const char* registered_id_from_json = (id_item && cJSON_IsString(id_item)) ? id_item->valuestring : NULL;

    char* c_name = generate_unique_var_name(ctx, registered_id_from_json ? registered_id_from_json : original_json_type_str);
    if (!c_name) { // Error already reported by generate_unique_var_name
        cJSON_Delete(new_scope_context);
        return NULL;
    }

    const char* object_c_type = "lv_obj_t*";
    char derived_c_type[256] = {0};

    if (init_item && cJSON_IsObject(init_item) && init_item->child) {
        object_c_type = api_spec_get_function_return_type(ctx->api_spec, init_item->child->string);
    } else if (widget_def) {
        if (widget_def->create) object_c_type = api_spec_get_function_return_type(ctx->api_spec, widget_def->create);
        else if (widget_def->c_type) {
            bool is_init_object = (widget_def->init_func != NULL && widget_def->create == NULL);
            if (is_init_object && strchr(widget_def->c_type, '*') == NULL) {
                snprintf(derived_c_type, sizeof(derived_c_type), "%s*", widget_def->c_type);
                object_c_type = derived_c_type;
            } else object_c_type = widget_def->c_type;
        }
    }

    const char* clean_id = registered_id_from_json;
    if (clean_id && clean_id[0] == '@') clean_id++;
    IRObject* ir_obj = ir_new_object(c_name, original_json_type_str, object_c_type, clean_id);

#define PARSE_OBJECT_ERROR(msg_format, ...) do { \
    char err_buf[512]; \
    snprintf(err_buf, sizeof(err_buf), msg_format, ##__VA_ARGS__); \
    render_abort(err_buf); \
    ctx->error_occurred = true; \
    free(c_name); \
    cJSON_Delete(new_scope_context); \
    ir_free((IRNode*)ir_obj); \
    return NULL; \
} while (0)

    registry_add_generated_var(ctx->registry, ir_obj->c_name, ir_obj->c_name, ir_obj->c_type);

    if (fallback_to_obj) {
        char warning_msg[256];
        snprintf(warning_msg, sizeof(warning_msg), "Widget type '%s' not found in API spec. Falling back to a generic 'obj'.", original_json_type_str);
        ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_warning(warning_msg));
    }

    if (init_item) {
        if (cJSON_IsObject(init_item) && init_item->child) {
            const char* func_name = init_item->child->string;
            cJSON* user_args_json = init_item->child;

            if (!api_spec_find_function(ctx->api_spec, func_name)) PARSE_OBJECT_ERROR("In 'init' block for '%s', could not find function definition for '%s'.", ir_obj->c_name, func_name);
            const FunctionDefinition* func_def_init = api_spec_find_function(ctx->api_spec, func_name);

            const FunctionArg* first_expected_arg = func_def_init->args_head;
            bool func_expects_target = (first_expected_arg && first_expected_arg->type && strstr(first_expected_arg->type, "_t*"));
            int expected_argc = count_function_args(func_def_init->args_head);
            int user_argc = cJSON_IsArray(user_args_json) ? count_cjson_array(user_args_json) : (cJSON_IsNull(user_args_json) ? 0 : 1);
            bool prepend_target = (func_expects_target && user_argc == expected_argc - 1);
            IRExprNode* final_args = NULL;
            const FunctionArg* expected_arg_list_for_user = func_def_init->args_head;

            if (prepend_target) {
                ir_expr_list_add(&final_args, ir_new_expr_registry_ref(parent_c_name, first_expected_arg->type));
                if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
            }

            if (cJSON_IsArray(user_args_json)) {
                cJSON* val_item = user_args_json->child;
                while(val_item) {
                    const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                    IRExpr* expr = unmarshal_value(ctx, val_item, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj);
                    if (ctx->error_occurred) { ir_free((IRNode*)final_args); PARSE_OBJECT_ERROR("Error processing 'init' arguments for %s", ir_obj->c_name); }
                    ir_expr_list_add(&final_args, expr);
                    if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
                    val_item = val_item->next;
                }
            } else if (!cJSON_IsNull(user_args_json)) {
                const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                IRExpr* expr = unmarshal_value(ctx, user_args_json, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj);
                if (ctx->error_occurred) { PARSE_OBJECT_ERROR("Error processing 'init' argument for %s", ir_obj->c_name); }
                ir_expr_list_add(&final_args, expr);
            }

            process_and_validate_call(ctx, func_name, &final_args, ir_obj);
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
            ir_obj->constructor_expr = ir_new_expr_func_call(func_name, final_args, ret_type);

        } else PARSE_OBJECT_ERROR("The 'init' property for object '%s' must be a map with a single function call.", ir_obj->c_name);
    } else {
        const char* create_func = (widget_def && widget_def->create) ? widget_def->create : (strcmp(original_json_type_str, "obj") == 0 ? "lv_obj_create" : NULL);
        if (create_func) {
            IRExprNode* args = NULL;
            ir_expr_list_add(&args, ir_new_expr_registry_ref(parent_c_name, "lv_obj_t*"));
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, create_func);
            ir_obj->constructor_expr = ir_new_expr_func_call(create_func, args, ret_type);
            process_and_validate_call(ctx, create_func, &((IRExprFunctionCall*)ir_obj->constructor_expr)->args, ir_obj);
        } else if (widget_def && widget_def->init_func) {
            char sizeof_arg_buf[256];
            char* base_type = get_array_base_type(ir_obj->c_type);
            if(base_type) { snprintf(sizeof_arg_buf, sizeof(sizeof_arg_buf), "sizeof(%s)", base_type); free(base_type); }
            else snprintf(sizeof_arg_buf, sizeof(sizeof_arg_buf), "0 /* Error: could not get base type for %s */", ir_obj->c_type);

            IRExprNode* malloc_args = NULL;
            ir_expr_list_add(&malloc_args, ir_new_expr_literal(sizeof_arg_buf, "size_t"));
            ir_obj->constructor_expr = ir_new_expr_func_call("malloc", malloc_args, ir_obj->c_type);
            IRExprNode* init_args = NULL;
            ir_expr_list_add(&init_args, ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type));
            IRExpr* init_call = ir_new_expr_func_call(widget_def->init_func, init_args, "void");
            process_and_validate_call(ctx, widget_def->init_func, &init_args, ir_obj);
            ir_operation_list_add(&ir_obj->operations, (IRNode*)init_call);
        }
    }

    if (ctx->error_occurred) { free(c_name); cJSON_Delete(new_scope_context); ir_free((IRNode*)ir_obj); return NULL; }

    if (registered_id_from_json) {
        registry_add_generated_var(ctx->registry, registered_id_from_json, ir_obj->c_name, ir_obj->c_type);
        const char* id_for_runtime = (registered_id_from_json[0] == '@') ? registered_id_from_json + 1 : registered_id_from_json;
        IRExpr* obj_ref_expr = ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type);
        IRExpr* reg_call_expr = ir_new_expr_runtime_reg_add(id_for_runtime, obj_ref_expr);
        ir_operation_list_add(&ir_obj->operations, (IRNode*)reg_call_expr);
    }

    // --- Parse 'deferred' before iterating over properties ---
    // deferred: true  → auto-generate function name as "create_ui_<c_name>"
    // deferred: "fn"  → use the provided function name directly
    {
        cJSON* deferred_item = cJSON_GetObjectItem(obj_json, "deferred");
        if (deferred_item) {
            char* fn_name = NULL;
            if (cJSON_IsBool(deferred_item) && cJSON_IsTrue(deferred_item)) {
                // Auto-derive: "create_ui_" + c_name
                size_t len = strlen("create_ui_") + strlen(ir_obj->c_name) + 1;
                fn_name = malloc(len);
                if (fn_name) snprintf(fn_name, len, "create_ui_%s", ir_obj->c_name);
            } else if (cJSON_IsString(deferred_item) && deferred_item->valuestring && deferred_item->valuestring[0]) {
                fn_name = strdup(deferred_item->valuestring);
            }
            if (fn_name) {
                // Sanitize to a valid C identifier
                for (char* p = fn_name; *p; p++) {
                    if (!isalnum((unsigned char)*p) && *p != '_') *p = '_';
                }
                ir_obj->deferred_fn_name = fn_name;
            }
        }
    }

    for(cJSON* item = obj_json->child; item && !ctx->error_occurred; item = item->next) {
        const char* key = item->string;
        if (strncmp(key, "//", 2) == 0 || strcmp(key, "type") == 0 || strcmp(key, "init") == 0 ||
            strcmp(key, "id") == 0 || strcmp(key, "name") == 0 || strcmp(key, "context") == 0 ||
            strcmp(key, "deferred") == 0) continue;

        if (strcmp(key, "children") == 0) {
            if (cJSON_IsArray(item)) {
                process_ui_spec_array(ctx, item, current_base_path, NULL, &ir_obj->operations, ir_obj->c_name, new_scope_context);
                if (ctx->error_occurred) break;
            }
        } else if (strcmp(key, "observes") == 0) {
            if (cJSON_IsObject(item)) {
                cJSON* state_item;
                cJSON_ArrayForEach(state_item, item) {
                    const char* state_name = state_item->string;
                    // Observe value can be a string or an object. Let's handle both.
                    cJSON *bindings_obj = state_item;
                    if (cJSON_IsString(state_item)) {
                        bindings_obj = cJSON_CreateObject();
                        cJSON_AddItemToObject(bindings_obj, state_item->valuestring, cJSON_CreateNull());
                    } else if (!cJSON_IsObject(state_item)) {
                        print_warning("Value for observable '%s' must be an object or a string.", state_name);
                        continue;
                    }


                    cJSON* binding_item;
                    cJSON_ArrayForEach(binding_item, bindings_obj) {
                        const char* binding_key = binding_item->string;
                        observer_update_type_t update_type;

                        if (strcmp(binding_key, "text") == 0) update_type = OBSERVER_TYPE_TEXT;
                        else if (strcmp(binding_key, "style") == 0) update_type = OBSERVER_TYPE_STYLE;
                        else if (strcmp(binding_key, "visible") == 0) update_type = OBSERVER_TYPE_VISIBLE;
                        else if (strcmp(binding_key, "checked") == 0) update_type = OBSERVER_TYPE_CHECKED;
                        else if (strcmp(binding_key, "disabled") == 0) update_type = OBSERVER_TYPE_DISABLED;
                        else if (strcmp(binding_key, "led_on") == 0) update_type = OBSERVER_TYPE_LED_ON;
                        else if (strcmp(binding_key, "value") == 0) update_type = OBSERVER_TYPE_VALUE;
                        else if (strcmp(binding_key, "items") == 0) update_type = OBSERVER_TYPE_ITEMS;
                        else {
                            print_warning("Unknown binding type '%s' for observable '%s'.", binding_key, state_name);
                            continue;
                        }

                        IRExpr* config_expr = unmarshal_value(ctx, binding_item, new_scope_context, "unknown", parent_c_name, ir_obj->c_name, ir_obj);
                        ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_observer(state_name, update_type, config_expr));
                    }

                    if (cJSON_IsString(state_item)) {
                        cJSON_Delete(bindings_obj);
                    }
                }
            }
        } else if (strcmp(key, "action") == 0) {
            if (cJSON_IsObject(item)) {
                cJSON* act_item;
                cJSON_ArrayForEach(act_item, item) {
                    const char* action_name = act_item->string;
                    action_type_t action_type = ACTION_TYPE_TRIGGER; // Default
                    IRExpr* data_expr = NULL;

                    if (cJSON_IsString(act_item)) {
                        if (strcmp(act_item->valuestring, "trigger") == 0) action_type = ACTION_TYPE_TRIGGER;
                        else if (strcmp(act_item->valuestring, "toggle") == 0) action_type = ACTION_TYPE_TOGGLE;
                        else if (strcmp(act_item->valuestring, "value_changed") == 0) action_type = ACTION_TYPE_VALUE_CHANGED;
                        else {
                             print_warning("Unknown action type string '%s' for action '%s'.", act_item->valuestring, action_name);
                             continue;
                        }
                    } else if (cJSON_IsArray(act_item)) {
                        action_type = ACTION_TYPE_CYCLE;
                        data_expr = unmarshal_value(ctx, act_item, new_scope_context, "binding_value_t*", parent_c_name, ir_obj->c_name, ir_obj);
                    } else if (cJSON_IsObject(act_item)) {
                        cJSON* dialog_config = cJSON_GetObjectItemCaseSensitive(act_item, "numeric_input_dialog");
                        if (dialog_config) {
                            action_type = ACTION_TYPE_NUMERIC_DIALOG;
                            data_expr = unmarshal_value(ctx, dialog_config, new_scope_context, "void*", parent_c_name, ir_obj->c_name, ir_obj);
                        } else {
                            print_warning("Unsupported object-based action config for action '%s'.", action_name);
                            continue;
                        }
                    } else {
                         print_warning("Unsupported action config for action '%s'.", action_name);
                         continue;
                    }
                    ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_action(action_name, action_type, data_expr));
                }
            }
        } else {
            const PropertyDefinition* prop_def = api_spec_find_property(ctx->api_spec, ir_obj->json_type, key);
            const char* func_name = (prop_def && prop_def->setter) ? prop_def->setter : (api_spec_has_function(ctx->api_spec, key) ? key : NULL);
            if (!func_name) {
                char warning_msg[256];
                snprintf(warning_msg, sizeof(warning_msg), "Could not resolve property/method '%s' for type '%s'.", key, ir_obj->json_type);
                ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_warning(warning_msg));
                api_spec_free_property(prop_def);
                continue;
            }
            if (!api_spec_find_function(ctx->api_spec, func_name)) {
                char warning_msg[256];
                snprintf(warning_msg, sizeof(warning_msg), "Could not find function definition for '%s'.", func_name);
                ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_warning(warning_msg));
                api_spec_free_property(prop_def);
                continue;
            }
            const FunctionDefinition* func_def = api_spec_find_function(ctx->api_spec, func_name);
            const FunctionArg* first_expected_arg = func_def->args_head;
            bool func_expects_target = (first_expected_arg && first_expected_arg->type && strstr(first_expected_arg->type, "_t*"));
            IRExprNode* final_args = NULL;
            const FunctionArg* expected_arg_list_for_user = func_def->args_head;

            if (func_expects_target) {
                ir_expr_list_add(&final_args, ir_new_expr_registry_ref(ir_obj->c_name, ir_obj->c_type));
                if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
            }

            if (cJSON_IsArray(item)) {
                cJSON* val_item = item->child;
                while(val_item) {
                    const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                    IRExpr* expr = unmarshal_value(ctx, val_item, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj);
                    if (ctx->error_occurred) { ir_free((IRNode*)final_args); PARSE_OBJECT_ERROR("Error processing arguments for '%s' on %s", func_name, ir_obj->c_name); }
                    ir_expr_list_add(&final_args, expr);
                    if (expected_arg_list_for_user) expected_arg_list_for_user = expected_arg_list_for_user->next;
                    val_item = val_item->next;
                }
            } else {
                 const char* expected_type = expected_arg_list_for_user ? expected_arg_list_for_user->type : "unknown";
                 IRExpr* expr = unmarshal_value(ctx, item, new_scope_context, expected_type, parent_c_name, ir_obj->c_name, ir_obj);
                 if (ctx->error_occurred) { ir_free((IRNode*)final_args); PARSE_OBJECT_ERROR("Error processing argument for '%s' on %s", func_name, ir_obj->c_name); }
                 ir_expr_list_add(&final_args, expr);
            }

            process_and_validate_call(ctx, func_name, &final_args, ir_obj);
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
            ir_operation_list_add(&ir_obj->operations, (IRNode*)ir_new_expr_func_call(func_name, final_args, ret_type));
            api_spec_free_property(prop_def);
        }
    }

    if (ctx->error_occurred) { free(c_name); cJSON_Delete(new_scope_context); ir_free((IRNode*)ir_obj); return NULL; }

    free(c_name);
    cJSON_Delete(new_scope_context);

    return ir_obj;
}


// --- Value Unmarshaler ---

// Evaluate a C-like integer expression string using values from ApiSpec
// Supports: parentheses, +, -, <<, >>, &, |, ^ and integer literals and identifiers
// Forward declaration for non-nested parser entry point
static long eval_parse_or(const ApiSpec* spec, const char** pp, bool* ok);
static bool eval_expr_string_as_int(const ApiSpec* spec, const char* expr_src, long* out_value) {
    if (!expr_src || !spec || !out_value) return false;
    const char* p = expr_src;
    /* The original implementation used nested functions which is not
       valid ISO C. Replace nested parsers with small helper functions
       that operate on a pointer-to-pointer to the input string. */

    // forward helpers declared below

    // start pointer will be managed via a pointer-to-pointer
    const char* pptr = p;
    bool ok_local = true;
    long result = eval_parse_or(spec, &pptr, &ok_local);
    p = pptr;
    if (ok_local) { *out_value = result; return true; }
    return false;
}

// Helper parsing functions (non-nested) -------------------------------------------------
static void eval_skip_ws_ptr(const char** pp) {
    const char* p = *pp;
    while(*p && (*p==' ' || *p=='\t' || *p=='\n' || *p=='\r')) p++;
    *pp = p;
}

static long eval_parse_number_or_ident(const ApiSpec* spec, const char** pp, bool* ok) {
    eval_skip_ws_ptr(pp);
    const char* p = *pp;
    if (*p == '\0') { *ok = false; return 0; }
    if (*p == '(') {
        p++; *pp = p;
        long v = eval_parse_or(spec, pp, ok);
        eval_skip_ws_ptr(pp);
        p = *pp;
        if (*p == ')') { p++; *pp = p; }
        else { *ok = false; return 0; }
        return v;
    }
    if (((*p >= '0' && *p <= '9') || *p=='-')) {
        char* endptr; long v = strtol(p, &endptr, 0);
        if (endptr == p) { *ok = false; return 0; }
        *pp = endptr; *ok = true; return v;
    }
    if (((*p >= 'A' && *p <= 'Z') || *p == '_' || (*p >= 'a' && *p <= 'z'))) {
        const char* start = p;
        while(((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) p++;
        size_t len = p - start;
        char name[256]; if (len >= sizeof(name)) { *ok = false; return 0; }
        memcpy(name, start, len); name[len] = '\0';
        *pp = p;
        long v = 0;
        if (api_spec_find_constant_value(spec, name, &v)) { *ok = true; return v; }
        const char* enum_type = api_spec_find_global_enum_type(spec, name);
        if (enum_type) {
            if (api_spec_find_enum_value(spec, enum_type, name, &v)) { *ok = true; return v; }
        }
        if (strcmp(name, "true") == 0) { *ok = true; return 1; }
        if (strcmp(name, "false") == 0) { *ok = true; return 0; }
        *ok = false; return 0;
    }
    *ok = false; return 0;
}

static long eval_parse_addsub(const ApiSpec* spec, const char** pp, bool* ok) {
    bool lok;
    long v = eval_parse_number_or_ident(spec, pp, &lok); if (!lok) { *ok = false; return 0; }
    for(;;) {
        eval_skip_ws_ptr(pp);
        const char* p = *pp;
        if (*p=='+' || *p=='-') {
            char op = *p++; *pp = p;
            long r = eval_parse_number_or_ident(spec, pp, &lok); if (!lok) { *ok = false; return 0; }
            if (op=='+') v += r; else v -= r;
        } else break;
    }
    *ok = true; return v;
}

static long eval_parse_shift(const ApiSpec* spec, const char** pp, bool* ok) {
    long v = eval_parse_addsub(spec, pp, ok); if (!*ok) return 0;
    for(;;) {
        const char* p = *pp; eval_skip_ws_ptr(pp); p = *pp;
        if (p[0]=='<' && p[1]=='<') { *pp = p+2; long r = eval_parse_addsub(spec, pp, ok); v = v << r; }
        else if (p[0]=='>' && p[1]=='>') { *pp = p+2; long r = eval_parse_addsub(spec, pp, ok); v = v >> r; }
        else break;
    }
    *ok = true; return v;
}

static long eval_parse_and(const ApiSpec* spec, const char** pp, bool* ok) {
    long v = eval_parse_shift(spec, pp, ok); if (!*ok) return 0;
    eval_skip_ws_ptr(pp);
    while(**pp=='&') { const char* p = *pp; *pp = p+1; long r = eval_parse_shift(spec, pp, ok); v = v & r; eval_skip_ws_ptr(pp); }
    *ok = true; return v;
}

static long eval_parse_xor(const ApiSpec* spec, const char** pp, bool* ok) {
    long v = eval_parse_and(spec, pp, ok); if (!*ok) return 0;
    eval_skip_ws_ptr(pp);
    while(**pp=='^') { const char* p = *pp; *pp = p+1; long r = eval_parse_and(spec, pp, ok); v = v ^ r; eval_skip_ws_ptr(pp); }
    *ok = true; return v;
}

static long eval_parse_or(const ApiSpec* spec, const char** pp, bool* ok) {
    long v = eval_parse_xor(spec, pp, ok); if (!*ok) return 0;
    eval_skip_ws_ptr(pp);
    while(**pp=='|') { const char* p = *pp; *pp = p+1; long r = eval_parse_xor(spec, pp, ok); v = v | r; eval_skip_ws_ptr(pp); }
    *ok = true; return v;
}

static IRExpr* unmarshal_value(GenContext* ctx, cJSON* value, const cJSON* ui_context, const char* expected_c_type, const char* parent_c_name, const char* target_c_name, IRObject* ir_obj_for_warnings) {
    if (ctx->error_occurred) return NULL;
    if (!value || cJSON_IsNull(value)) return ir_new_expr_literal("NULL", "void*");

    if (cJSON_IsString(value)) {
        const char* s_original = value->valuestring;
        char* s_interpolated = interpolate_home_path(s_original);
        const char* s = s_interpolated ? s_interpolated : s_original;
        IRExpr* result_expr = NULL;


        // @_target refers to the parent object passed to the constructor.
        if (strcmp(s, "@_target") == 0) {
            if (parent_c_name) {
                const char* parent_type = registry_get_c_type_for_id(ctx->registry, parent_c_name);
                result_expr = ir_new_expr_registry_ref(parent_c_name, parent_type ? parent_type : "lv_obj_t*");
            }
        }
        // @_parent or @self refers to the object currently being defined.
        else if (strcmp(s, "@_parent") == 0 || strcmp(s, "@self") == 0) {
            if (target_c_name) {
                const char* target_type = registry_get_c_type_for_id(ctx->registry, target_c_name);
                result_expr = ir_new_expr_registry_ref(target_c_name, target_type ? target_type : "lv_obj_t*");
            }
        }
        else if (strncmp(s, "?|", 2) == 0) {
            const char* part1_start = s + 2;
            const char* separator = strchr(part1_start, '|');
            if (separator) {
                size_t part1_len = separator - part1_start;
                char* part1_str = strndup(part1_start, part1_len);
                const char* part2_str = separator + 1;

                // Recursively unmarshal both parts of the expression.
                cJSON* static_val_json = cJSON_CreateString(part1_str);
                IRExpr* static_expr = unmarshal_value(ctx, static_val_json, ui_context, expected_c_type, parent_c_name, target_c_name, ir_obj_for_warnings);
                cJSON_Delete(static_val_json);

                cJSON* dynamic_val_json = cJSON_CreateString(part2_str);
                IRExpr* dynamic_expr = unmarshal_value(ctx, dynamic_val_json, ui_context, expected_c_type, parent_c_name, target_c_name, ir_obj_for_warnings);
                cJSON_Delete(dynamic_val_json);

                free(part1_str);
                result_expr = ir_new_if_backend(static_expr, dynamic_expr);

            } else {
                if (ir_obj_for_warnings) {
                    char warning_msg[128];
                    snprintf(warning_msg, sizeof(warning_msg), "Invalid conditional string format. Expected '?|static|dynamic', got '%s'.", s);
                    ir_operation_list_add(&ir_obj_for_warnings->operations, (IRNode*)ir_new_warning(warning_msg));
                }
            }
        }
        else if (s[0] == '$') {
            // Support $name and $name/doc-info. When a '/' is present we
            // only use the prefix before the slash for lookup in the ui
            // context. The trailing doc-info is ignored by the generator.
            const char* raw = s + 1;
            const char* slash = strchr(raw, '/');
            size_t varlen = slash ? (size_t)(slash - raw) : strlen(raw);
            char varbuf[256];
            if (varlen >= sizeof(varbuf)) varlen = sizeof(varbuf) - 1;
            memcpy(varbuf, raw, varlen);
            varbuf[varlen] = '\0';
            const cJSON* context_val_json = NULL;

            if (ui_context && cJSON_IsObject(ui_context)) {
                 context_val_json = cJSON_GetObjectItem(ui_context, varbuf);
            }

            if (context_val_json) {
                result_expr = unmarshal_value(ctx, (cJSON*)context_val_json, ui_context, expected_c_type, parent_c_name, target_c_name, ir_obj_for_warnings);
            } else {
                if (ir_obj_for_warnings) {
                    char warning_msg[128];
                    snprintf(warning_msg, sizeof(warning_msg), "Context variable '%s' not found.", varbuf);
                    ir_operation_list_add(&ir_obj_for_warnings->operations, (IRNode*)ir_new_warning(warning_msg));
                }
                // Fall through to treat as a literal string if not found
            }
        }
        if (result_expr == NULL && strchr(s, '|')) {
            long final_val = 0;
            char* temp_str = strdup(s);
            if (!temp_str) { render_abort("Failed to duplicate string for OR-parsing"); ctx->error_occurred = true; return NULL; }
            char* token = strtok(temp_str, "|");
            bool error = false;
            while (token != NULL) {
                char* trimmed_token = trim_whitespace(token);
                if (strlen(trimmed_token) > 0) {
                    long part_val = 0;
                    if (api_spec_find_enum_value(ctx->api_spec, expected_c_type, trimmed_token, &part_val) ||
                        api_spec_find_enum_value(ctx->api_spec, api_spec_find_global_enum_type(ctx->api_spec, trimmed_token), trimmed_token, &part_val) ||
                        api_spec_find_constant_value(ctx->api_spec, trimmed_token, &part_val)) {
                        final_val |= part_val;
                    } else {
                        if (ir_obj_for_warnings) print_warning("Could not resolve part '%s' of OR-expression '%s'", trimmed_token, s);
                        error = true; break;
                    }
                }
                token = strtok(NULL, "|");
            }
            free(temp_str);
            if (!error) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%ld", final_val);
                result_expr = ir_new_expr_literal(buf, (expected_c_type && strcmp(expected_c_type, "unknown") != 0) ? expected_c_type : "float");
            }
        }
        if (result_expr == NULL) {
            char* const_str_val = api_spec_find_constant_string(ctx->api_spec, s);
            if (const_str_val) {
                size_t unescaped_len = 0;
                char* unescaped_val = unescape_c_string(const_str_val, &unescaped_len);
                result_expr = ir_new_expr_static_string(unescaped_val, unescaped_len);
                free(const_str_val);
                free(unescaped_val);
            }
        }

        if (result_expr == NULL) {
            long const_val;
            if (api_spec_find_constant_value(ctx->api_spec, s, &const_val)) {
                char buf[32];
                snprintf(buf, sizeof(buf), "%ld", const_val);
                /* Preserve the original identifier while retaining the
                 * numeric value for dynamic dispatch by creating an enum
                 * expression node carrying both the symbol and the value.
                 */
                result_expr = ir_new_expr_enum(s, const_val, "constant");
            }
        }


        if (result_expr == NULL) {
            size_t len = strlen(s);

            if (s[0] == '@') {
                // A name like "@$my_var" is a special reference to a C identifier.
                // The c_code_printer will output it directly without lookup.
                if (s[1] == '$') {
                    result_expr = ir_new_expr_registry_ref(s, "c_identifier");
                } else {
                    result_expr = ir_new_expr_registry_ref(s, registry_get_c_type_for_id(ctx->registry, s));
                }
            }
            else if (s[0] == '!') {
                size_t unescaped_len = 0;
                char* unescaped_val = unescape_c_string(s + 1, &unescaped_len);
                result_expr = ir_new_expr_static_string(unescaped_val, unescaped_len);
                free(unescaped_val);
            }
            else if (s[0] == '#') {
                long hex_val = strtol(s + 1, NULL, 16);
                char hex_str_arg[32];
                snprintf(hex_str_arg, sizeof(hex_str_arg), "0x%06lX", hex_val);
                IRExprNode* args = NULL;
                ir_expr_list_add(&args, ir_new_expr_literal(hex_str_arg, "uint32_t"));
                result_expr = ir_new_expr_func_call("lv_color_hex", args, "lv_color_t");
            }
            else if (len > 0 && s[len - 1] == '%') {
                char* temp_s = strdup(s);
                if (!temp_s) return NULL;
                temp_s[len - 1] = '\0'; // remove the '%'

                char* trimmed_num_part = trim_whitespace(temp_s);
                char* endptr;
                strtol(trimmed_num_part, &endptr, 10); // Try to parse it as an integer

                if (*endptr == '\0' && endptr != trimmed_num_part) {
                    IRExprNode* args = NULL;
                    ir_expr_list_add(&args, ir_new_expr_literal(trimmed_num_part, "int32_t"));
                    free(temp_s);
                    result_expr = ir_new_expr_func_call("lv_pct", args, "lv_coord_t");
                } else {
                    free(temp_s);
                }
            }
        }

        if (result_expr == NULL) {
            if (expected_c_type && api_spec_is_enum_member(ctx->api_spec, expected_c_type, s)) {
                long enum_val;
                api_spec_find_enum_value(ctx->api_spec, expected_c_type, s, &enum_val);
                result_expr = ir_new_expr_enum(s, enum_val, (char*)expected_c_type);
            }
        }

        if (result_expr == NULL) {
            const char* inferred_enum_type = api_spec_find_global_enum_type(ctx->api_spec, s);
            if (inferred_enum_type) {
                 long enum_val;
                api_spec_find_enum_value(ctx->api_spec, inferred_enum_type, s, &enum_val);
                result_expr = ir_new_expr_enum(s, enum_val, (char*)inferred_enum_type);
            }
        }

        if (result_expr == NULL) {
            /* If the token matches a named constant in the ApiSpec, prefer
             * emitting it as a symbol (IR_EXPR_ENUM) so the C printer will
             * output the identifier unquoted. If the constant has a numeric
             * value we emit a numeric literal instead. This handles cases
             * like LV_GRID_TEMPLATE_LAST which are defined as expressions
             * in the constants map.
             */
            /* Preserve quoted numeric strings (e.g. "0") as strings, but
             * allow the explicit token "NULL" to be interpreted as a null
             * pointer when the expected C type is a pointer. This lets users
             * write NULL in YAML to indicate a null pointer while still
             * preserving numeric text strings for labels.
             */
            if (strcmp(s, "NULL") == 0 && expected_c_type && strchr(expected_c_type, '*')) {
                result_expr = ir_new_expr_literal("NULL", "void*");
            }
            /* If this token is a named constant in the ApiSpec prefer that
             * interpretation: emit an enum symbol node carrying the numeric
             * value when available so backends can choose how to emit it.
             */
            if (api_spec_is_constant(ctx->api_spec, s)) {
                long const_val;
                if (api_spec_find_constant_value(ctx->api_spec, s, &const_val)) {
                    /* Create an enum/constant node so backends can choose
                     * between emitting the identifier or the numeric value.
                     */
                    result_expr = ir_new_expr_enum(s, const_val, "constant");
                } else {
                    // Attempt to evaluate constant expression textually using other spec values
                    const cJSON* consts = api_spec_get_constants(ctx->api_spec);
                    if (consts) {
                        const cJSON* cjson = cJSON_GetObjectItemCaseSensitive(consts, s);
                        if (cjson && cJSON_IsString(cjson) && cjson->valuestring) {
                            long eval_val = 0;
                            if (eval_expr_string_as_int(ctx->api_spec, cjson->valuestring, &eval_val)) {
                                /* Emit an enum expression node carrying the original
                                 * identifier and the evaluated numeric value so the
                                 * C backend can print the identifier while dynamic
                                 * dispatch can use the numeric value.
                                 */
                                result_expr = ir_new_expr_enum(s, eval_val, "constant");
                            } else {
                                result_expr = ir_new_expr_enum(s, 0, "unknown");
                            }
                        } else {
                            result_expr = ir_new_expr_enum(s, 0, "unknown");
                        }
                    } else {
                        result_expr = ir_new_expr_enum(s, 0, "unknown");
                    }
                }
            }
            /* Do not coerce quoted strings into numeric literals here.
             * Quoted numeric text (e.g. "0", "1") should remain strings;
             * numeric unquoted values are parsed as numbers by the YAML
             * parser and will arrive as cJSON numbers instead of strings.
             */

            if (!result_expr) {
                // Do NOT guess identifiers by heuristic. If the token is not a
                // known enum/constant in the ApiSpec, treat it as a string.
                size_t unescaped_len = 0;
                char* unescaped_val = unescape_c_string(s, &unescaped_len);
                result_expr = ir_new_expr_literal_string(unescaped_val, unescaped_len);
                free(unescaped_val);
            }
        }

        if (s_interpolated) {
            free(s_interpolated);
        }
        return result_expr;
    }
    if (cJSON_IsNumber(value)) {
        char buf[32]; snprintf(buf, sizeof(buf), "%g", value->valuedouble);
        return ir_new_expr_literal(buf, "float");
    }
    if (cJSON_IsBool(value)) return ir_new_expr_literal(cJSON_IsTrue(value) ? "true" : "false", "bool");
    if (cJSON_IsArray(value)) {
        char* base_type = get_array_base_type(expected_c_type);
        IRExprNode* elements = NULL;
        cJSON* elem_json;
        cJSON_ArrayForEach(elem_json, value) {
            IRExpr* elem_expr = unmarshal_value(ctx, elem_json, ui_context, base_type, parent_c_name, target_c_name, ir_obj_for_warnings);
            if (ctx->error_occurred) { ir_free((IRNode*)elements); free(base_type); return NULL; }
            ir_expr_list_add(&elements, elem_expr);
        }
        free(base_type);
        return ir_new_expr_array(elements, (char*)expected_c_type);
    }
    if (cJSON_IsObject(value)) {
        cJSON* func_item = value->child;
        if (func_item && func_item->next == NULL && api_spec_has_function(ctx->api_spec, func_item->string)) {
            const char* func_name = func_item->string;
            const FunctionDefinition* func_def = api_spec_find_function(ctx->api_spec, func_name);
            IRExprNode* args_list = NULL;
            const FunctionArg* expected_args = func_def ? func_def->args_head : NULL;
            if (cJSON_IsArray(func_item)) {
                cJSON* arg_item;
                cJSON_ArrayForEach(arg_item, func_item) {
                    const char* expected_type = expected_args ? expected_args->type : "unknown";
                    IRExpr* expr = unmarshal_value(ctx, arg_item, ui_context, expected_type, parent_c_name, target_c_name, ir_obj_for_warnings);
                    if (ctx->error_occurred) { ir_free((IRNode*)args_list); return NULL; }
                    ir_expr_list_add(&args_list, expr);
                    if (expected_args) expected_args = expected_args->next;
                }
            } else if (!cJSON_IsNull(func_item)) {
                const char* expected_type = expected_args ? expected_args->type : "unknown";
                IRExpr* expr = unmarshal_value(ctx, func_item, ui_context, expected_type, parent_c_name, target_c_name, ir_obj_for_warnings);
                if (ctx->error_occurred) { return NULL; }
                ir_expr_list_add(&args_list, expr);
            }
            const char* ret_type = api_spec_get_function_return_type(ctx->api_spec, func_name);
            return ir_new_expr_func_call(func_name, args_list, ret_type);
        }
        IRExprNode* map_elements = NULL;
        cJSON* map_item;
        cJSON_ArrayForEach(map_item, value) {
            IRExprNode* pair_elements = NULL;
            char* key_str = map_item->string;
            if (strcmp(key_str, "true") == 0 || strcmp(key_str, "false") == 0) {
                 ir_expr_list_add(&pair_elements, ir_new_expr_literal(key_str, "bool"));
            } else {
                 char* endptr;
                 strtod(key_str, &endptr);
                 if (*endptr == '\0') {
                    ir_expr_list_add(&pair_elements, ir_new_expr_literal(key_str, "float"));
                 } else {
                    ir_expr_list_add(&pair_elements, ir_new_expr_literal_string(key_str, strlen(key_str)));
                 }
            }

            ir_expr_list_add(&pair_elements, unmarshal_value(ctx, map_item, ui_context, "unknown", parent_c_name, target_c_name, ir_obj_for_warnings));

            ir_expr_list_add(&map_elements, ir_new_expr_array(pair_elements, "void*[]"));
        }
        return ir_new_expr_array(map_elements, "void*[]");

    }
    return ir_new_expr_literal("NULL", "unknown");
}

// --- Type Checking & Helpers ---

static int count_cjson_array(cJSON* array_json) {
    if (!cJSON_IsArray(array_json)) return 0;
    int count = 0;
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, array_json) {
        count++;
    }
    return count;
}

static int count_function_args(const FunctionArg* head) {
    int count = 0;
    for (const FunctionArg* arg = head; arg; arg = arg->next) {
        if (arg->type && strcmp(arg->type, "void") != 0) {
            count++;
        }
    }
    return count;
}

static void merge_json_objects(cJSON* dest, const cJSON* source) {
    if (!cJSON_IsObject(dest) || !cJSON_IsObject(source)) return;
    cJSON* item = NULL;
    cJSON_ArrayForEach(item, source) {
        if (cJSON_HasObjectItem(dest, item->string)) {
            cJSON_ReplaceItemInObject(dest, item->string, cJSON_Duplicate(item, true));
        } else {
            cJSON_AddItemToObject(dest, item->string, cJSON_Duplicate(item, true));
        }
    }
}

static bool types_compatible(const char* expected, const char* actual) {
    if (!expected || !actual) return false;
    if (strcmp(expected, "unknown") == 0 || strcmp(actual, "unknown") == 0) return true;
    if (strcmp(expected, "enum") == 0 && strstr(actual, "_t")) return true;
    if (strcmp(expected, actual) == 0) return true;

    if ((strcmp(expected, "const char*") == 0 && strcmp(actual, "char*") == 0) ||
        (strcmp(expected, "char*") == 0 && strcmp(actual, "const char*") == 0)) return true;

    if (strcmp(expected, "lv_style_t*") == 0 && strcmp(actual, "lv_style_t") == 0) return true;

    const char* num_types[] = {"int", "int32_t", "uint32_t", "lv_coord_t", "lv_style_selector_t", "lv_opa_t", "bool", "lv_anim_enable_t", "float"};
    int num_num_types = sizeof(num_types) / sizeof(char*);
    bool expected_is_num = false;
    bool actual_is_num = false;
    for(int i=0; i < num_num_types; i++) {
        if(strcmp(expected, num_types[i]) == 0) expected_is_num = true;
        if(strcmp(actual, num_types[i]) == 0) actual_is_num = true;
    }
    if (expected_is_num && actual_is_num) return true;

    if (strcmp(expected, "void*") == 0 && strchr(actual, '*') != NULL) return true;

    return false;
}

static void process_and_validate_call(GenContext* ctx, const char* func_name, IRExprNode** args_list_ptr, IRObject* ir_obj_for_warnings) {
    const FunctionDefinition* func_def = api_spec_find_function(ctx->api_spec, func_name);
    if (!func_def) {
        print_warning("Cannot validate call to unknown function '%s'.", func_name);
        return;
    }

    int actual_argc = 0;
    for (IRExprNode* n = *args_list_ptr; n; n = n->next) actual_argc++;

    int expected_argc = 0;
    const FunctionArg* last_expected_arg = NULL;
    for (const FunctionArg* a = func_def->args_head; a; a = a->next) {
        if(a->type && strcmp(a->type, "void") == 0) continue;
        expected_argc++;
        last_expected_arg = a;
    }

    bool func_expects_target = (func_def->args_head && func_def->args_head->type && strstr(func_def->args_head->type, "_t*"));
    int user_provided_argc = func_expects_target ? (actual_argc - 1) : actual_argc;

    if (expected_argc > 0 && func_expects_target && (expected_argc - 1 == 0) && user_provided_argc > 0) {
        IRExprNode* current = (*args_list_ptr)->next;
        (*args_list_ptr)->next = NULL;

        while (current) {
            IRExprNode* temp = current->next;
            ir_free((IRNode*)current->expr);
            free(current);
            current = temp;
        }
        actual_argc = 1;
    }

    if (strncmp(func_name, "lv_obj_set_style_", 17) == 0 && actual_argc == expected_argc - 1) {
        if (last_expected_arg && strcmp(last_expected_arg->type, "lv_style_selector_t") == 0) {
            ir_expr_list_add(args_list_ptr, ir_new_expr_literal("0", "lv_style_selector_t"));
            actual_argc++;
        }
    }

    if (actual_argc != expected_argc) {
        if (g_strict_mode) {
             char err_buf[256];
             snprintf(err_buf, sizeof(err_buf), "Strict mode failure: Argument count mismatch for '%s'. Expected %d, got %d.", func_name, expected_argc, actual_argc);
             render_abort(err_buf);
             ctx->error_occurred = true;
        } else if (ir_obj_for_warnings) {
            char warning_msg[256];
            snprintf(warning_msg, sizeof(warning_msg), "Argument count mismatch for function '%s'. Expected %d, but %d were provided.", func_name, expected_argc, actual_argc);
            ir_operation_list_add(&ir_obj_for_warnings->operations, (IRNode*)ir_new_warning(warning_msg));
        }
        return;
    }

    IRExprNode* actual_arg_node = *args_list_ptr;
    const FunctionArg* expected_arg = func_def->args_head;
    while (actual_arg_node && expected_arg) {
        if(expected_arg->type && strcmp(expected_arg->type, "void") == 0) {
             expected_arg = expected_arg->next;
             continue;
        }
        actual_arg_node = actual_arg_node->next;
        expected_arg = expected_arg->next;
    }
}

static char* sanitize_c_identifier(const char* input_name) {
    if (!input_name || *input_name == '\0') return strdup("unnamed_var");
    size_t len = strlen(input_name);
    char* sanitized = malloc(len + 2);
    if (!sanitized) return strdup("oom_var");
    char* s_ptr = sanitized;
    const char* i_ptr = input_name;
    if (*i_ptr == '@') i_ptr++;
    if (!isalpha((unsigned char)*i_ptr) && *i_ptr != '_') *s_ptr++ = '_';
    while (*i_ptr) {
        *s_ptr++ = (isalnum((unsigned char)*i_ptr) || *i_ptr == '_') ? *i_ptr : '_';
        i_ptr++;
    }
    *s_ptr = '\0';
    return sanitized;
}

static char* generate_unique_var_name(GenContext* ctx, const char* base_name) {
    char sanitized_base[256];
    char* temp_sanitized = sanitize_c_identifier(base_name);
    if (!temp_sanitized) {
        render_abort("Failed to allocate memory for sanitized name.");
        ctx->error_occurred = true;
        return NULL;
    }
    strncpy(sanitized_base, temp_sanitized, sizeof(sanitized_base) - 1);
    sanitized_base[sizeof(sanitized_base) - 1] = '\0';
    free(temp_sanitized);

    char* final_name = malloc(strlen(sanitized_base) + 16);
    if (!final_name) {
        render_abort("Failed to allocate memory for variable name.");
        ctx->error_occurred = true;
        return NULL;
    }
    snprintf(final_name, strlen(sanitized_base) + 16, "%s_%d", sanitized_base, ctx->var_counter++);
    return final_name;
}

static void process_ui_spec_array(GenContext* ctx, cJSON* array_json, const char* current_base_path, IRObject** object_list_head, IROperationNode** operation_list_head, const char* parent_c_name, const cJSON* ui_context) {
    cJSON* item_json;
    cJSON_ArrayForEach(item_json, array_json) {
        if (ctx->error_occurred) break;

            cJSON* include_item = cJSON_GetObjectItem(item_json, "include");
                if (include_item) {
                    // include can be a string (file path) or an object with options
                    const char* include_file = NULL;
                    const cJSON* include_context = NULL;
                    const char* as_prefix = NULL;
                    bool merge_flag = true;

                    // Ensure the include mapping is standalone (no other keys besides comments)
                    int other_keys = 0;
                    cJSON* tmp_k = NULL;
                    cJSON_ArrayForEach(tmp_k, item_json) {
                        if (!tmp_k->string) continue;
                        if (strncmp(tmp_k->string, "//", 2) == 0) continue;
                        if (strcmp(tmp_k->string, "include") == 0) continue;
                        other_keys++;
                    }
                    if (other_keys > 0) {
                        render_abort("Include directive must be a standalone item with no other keys.");
                        ctx->error_occurred = true;
                        break;
                    }

                    if (cJSON_IsString(include_item)) {
                        include_file = include_item->valuestring;
                    } else if (cJSON_IsObject(include_item)) {
                        cJSON* f = cJSON_GetObjectItemCaseSensitive(include_item, "file");
                        if (f && cJSON_IsString(f)) include_file = f->valuestring;
                        cJSON* ctx_item = cJSON_GetObjectItemCaseSensitive(include_item, "context");
                        if (ctx_item && cJSON_IsObject(ctx_item)) include_context = ctx_item;
                        cJSON* as_item = cJSON_GetObjectItemCaseSensitive(include_item, "as");
                        if (as_item && cJSON_IsString(as_item)) as_prefix = as_item->valuestring;
                        cJSON* merge_item = cJSON_GetObjectItemCaseSensitive(include_item, "merge");
                        if (merge_item && cJSON_IsBool(merge_item)) merge_flag = cJSON_IsTrue(merge_item);
                    } else {
                        render_abort("Unsupported include directive type; expected string or mapping.");
                        ctx->error_occurred = true;
                        break;
                    }

                    if (!include_file) {
                        render_abort("Include directive missing 'file' entry.");
                        ctx->error_occurred = true;
                        break;
                    } else {
                        char* expanded_include = interpolate_home_path(include_file);
                        char* include_copy = expanded_include ? expanded_include : strdup(include_file);
                        char* full_path = join_path(current_base_path, include_copy);
                        if (expanded_include) free(expanded_include);
                        free(include_copy);
                        char* included_content = read_file(full_path);
                        if (!included_content) {
                            char err_buf[512];
                            snprintf(err_buf, sizeof(err_buf), "Could not read include file: %s", full_path);
                            render_abort(err_buf);
                            free(full_path);
                            ctx->error_occurred = true;
                            break;
                        }

                        char* error_msg = NULL;
                        cJSON* included_json = yaml_to_cjson(included_content, &error_msg);
                        free(included_content);

                        if (error_msg) {
                            char err_buf[1024];
                            snprintf(err_buf, sizeof(err_buf), "Error in included file '%s': %s", full_path, error_msg);
                            render_abort(err_buf);
                            free(error_msg);
                            free(full_path);
                            ctx->error_occurred = true;
                            if (included_json) cJSON_Delete(included_json);
                            break;
                        }

                        if (included_json && cJSON_IsArray(included_json)) {
                            // Build merged context: ui_context <- include_context
                            cJSON* new_context = NULL;
                            if (ui_context || include_context) {
                                new_context = cJSON_CreateObject();
                                if (ui_context) merge_json_objects(new_context, ui_context);
                                if (include_context) merge_json_objects(new_context, include_context);
                            }

                            // If an 'as' prefix is provided, apply it to ids inside the included JSON
                            if (as_prefix) {
                                prefix_ids_recursive(included_json, as_prefix);
                            }

                            char* new_base_path = get_dirname(full_path);
                            // Process included array items as if they were inline in the parent array
                            process_ui_spec_array(ctx, included_json, new_base_path, object_list_head, operation_list_head, parent_c_name, new_context ? new_context : ui_context);

                            free(new_base_path);
                            if (new_context) cJSON_Delete(new_context);
                        } else {
                            char err_buf[512];
                            snprintf(err_buf, sizeof(err_buf), "Included file '%s' does not contain a top-level YAML/JSON list.", full_path);
                            render_abort(err_buf);
                            ctx->error_occurred = true;
                            if (included_json) cJSON_Delete(included_json);
                            free(full_path);
                            break;
                        }

                        free(full_path);
                        cJSON_Delete(included_json);
                        // Skip further handling of this item (it's an include)
                        continue;
                    }
                }
            else if (cJSON_IsObject(item_json)) {
             cJSON* type_item = cJSON_GetObjectItemCaseSensitive(item_json, "type");
            if (type_item && cJSON_IsString(type_item)) {
                if (strcmp(type_item->valuestring, "component") == 0) continue;
                if (strcmp(type_item->valuestring, "data-binding") == 0) {
                    if (!ui_sim_process_node(item_json)) ctx->error_occurred = true;
                    continue;
                }
            }

            IRObject* new_obj = parse_object(ctx, item_json, parent_c_name, ui_context, current_base_path);
            if (ctx->error_occurred) break;

            if (new_obj) {
                if (object_list_head) {
                    ir_object_list_add(object_list_head, new_obj);
                }
                if (operation_list_head) {
                    ir_operation_list_add(operation_list_head, (IRNode*)new_obj);
                }
            }
        }
    }
}

#include "c_code_printer.h"
#include "ir.h"
#include "api_spec.h"
#include "utils.h" // For render_abort
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// --- ID to C-Name/Type Mapping ---
// The IR uses registered IDs (@my_button), but the C code needs the generated
// variable names (my_button_0) and their types to correctly handle references.
// This map is built by a pre-pass over the IR.
typedef struct IdMapNode {
    char* id;
    char* c_name;
    char* c_type; // C type of the object
    struct IdMapNode* next;
} IdMapNode;

static void id_map_add(IdMapNode** map_head, const char* id, const char* c_name, const char* c_type) {
    if (!id || !c_name || !c_type) return;
    IdMapNode* new_node = malloc(sizeof(IdMapNode));
    if (!new_node) render_abort("Failed to allocate IdMapNode");
    new_node->id = strdup(id);
    new_node->c_name = strdup(c_name);
    new_node->c_type = strdup(c_type);
    new_node->next = *map_head;
    *map_head = new_node;
}

static const IdMapNode* id_map_get_node(IdMapNode* map_head, const char* id) {
    if (!id) return NULL;
    for (IdMapNode* current = map_head; current; current = current->next) {
        if (strcmp(current->id, id) == 0) {
            return current;
        }
    }
    return NULL; // Not found
}

static void id_map_free(IdMapNode* map_head) {
    IdMapNode* current = map_head;
    while (current) {
        IdMapNode* next = current->next;
        free(current->id);
        free(current->c_name);
        free(current->c_type);
        free(current);
        current = next;
    }
}


// --- Forward Declarations ---
static void print_expr(IRExpr* expr, const char* parent_c_name, IdMapNode* map);
static void print_object_list(IRObject* head, int indent_level, const char* parent_c_name, IdMapNode* map);


// --- Printing Helpers ---

static void print_indent(int level) {
    for (int i = 0; i < level; ++i) printf("    ");
}

static void print_expr_list(IRExprNode* head, const char* parent_c_name, IdMapNode* map) {
    bool first = true;
    for (IRExprNode* current = head; current; current = current->next) {
        if (!first) printf(", ");
        print_expr(current->expr, parent_c_name, map);
        first = false;
    }
}

static void print_expr(IRExpr* expr, const char* parent_c_name, IdMapNode* map) {
    if (!expr) { printf("NULL"); return; }

    switch (expr->base.type) {
        case IR_EXPR_LITERAL:
            if (((IRExprLiteral*)expr)->is_string) printf("\"%s\"", ((IRExprLiteral*)expr)->value);
            else printf("%s", ((IRExprLiteral*)expr)->value);
            break;
        case IR_EXPR_STATIC_STRING:
            printf("\"%s\"", ((IRExprStaticString*)expr)->value);
            break;
        case IR_EXPR_ENUM:
            printf("%s", ((IRExprEnum*)expr)->symbol);
            break;
        case IR_EXPR_REGISTRY_REF: {
            const char* name = ((IRExprRegistryRef*)expr)->name;
            if (strcmp(name, "parent") == 0) {
                printf("%s", parent_c_name);
            } else if (name[0] == '@') {
                const IdMapNode* node = id_map_get_node(map, name + 1);
                if (node) {
                    bool is_pointer = strchr(node->c_type, '*') != NULL;
                    if (is_pointer) {
                        printf("%s", node->c_name);
                    } else {
                        // Non-pointer types (like lv_style_t) must be passed by reference.
                        printf("&%s", node->c_name);
                    }
                } else {
                    printf("/* unresolved_ref: %s */", name);
                }
            } else {
                // This is a direct C variable name from the generator (e.g., for init funcs)
                // We need to check if it refers to a non-pointer and needs an '&'
                const IdMapNode* node = id_map_get_node(map, name);
                 if (node) {
                    bool is_pointer = strchr(node->c_type, '*') != NULL;
                    if(is_pointer) {
                        printf("%s", node->c_name);
                    } else {
                        printf("&%s", node->c_name);
                    }
                 } else {
                    // Fallback for names not in the ID map (like the implicit 'parent').
                    printf("%s", name);
                 }
            }
            break;
        }
        case IR_EXPR_CONTEXT_VAR:
            printf("/* CONTEXT_VAR: %s */", ((IRExprContextVar*)expr)->name);
            break;
        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)expr;
            printf("%s(", call->func_name);
            print_expr_list(call->args, parent_c_name, map);
            printf(")");
            break;
        }
        case IR_EXPR_ARRAY: {
            // Assumes usage in an initializer context, e.g. int arr[] = { ... };
            printf("{ ");
            print_expr_list(((IRExprArray*)expr)->elements, parent_c_name, map);
            printf(" }");
            break;
        }
        case IR_EXPR_RUNTIME_REG_ADD: {
            IRExprRuntimeRegAdd* reg = (IRExprRuntimeRegAdd*)expr;
            printf("obj_registry_add(\"%s\", ", reg->id);
            print_expr(reg->object_expr, parent_c_name, map);
            printf(")");
            break;
        }
        default:
            printf("/* UNKNOWN_EXPR */");
            break;
    }
}


// --- Traversal and Code Generation Logic ---

static void build_id_map_recursive(IRObject* head, IdMapNode** map_head) {
    for (IRObject* current = head; current; current = current->next) {
        if (current->registered_id && current->c_name && current->c_type) {
            id_map_add(map_head, current->registered_id, current->c_name, current->c_type);
        }
        // Also map the c_name to itself for lookups from init functions
        id_map_add(map_head, current->c_name, current->c_name, current->c_type);

        if (current->children) build_id_map_recursive(current->children, map_head);
        // if (current->with_blocks) ... // TODO: handle with blocks if needed
    }
}

static void print_object_list(IRObject* head, int indent_level, const char* parent_c_name, IdMapNode* map) {
    for (IRObject* current = head; current; current = current->next) {
        print_indent(indent_level);
        printf("// %s: %s (%s)\n", current->registered_id ? current->registered_id : "unnamed", current->c_name, current->json_type);
        print_indent(indent_level);
        printf("do {\n");

        print_indent(indent_level + 1);
        bool is_pointer = (current->c_type && strchr(current->c_type, '*') != NULL);
        if (is_pointer) {
            printf("%s %s = NULL;\n", current->c_type, current->c_name);
        } else {
            printf("%s %s;\n", current->c_type, current->c_name);
        }

        if (current->constructor_expr) {
            print_indent(indent_level + 1);
            printf("%s = ", current->c_name);
            print_expr(current->constructor_expr, parent_c_name, map);
            printf(";\n");
        }

        for (IRExprNode* call_node = current->setup_calls; call_node; call_node = call_node->next) {
            print_indent(indent_level + 1);
            print_expr(call_node->expr, parent_c_name, map);
            printf(";\n");
        }

        if (current->children) {
            printf("\n");
            print_object_list(current->children, indent_level + 1, current->c_name, map);
        }

        print_indent(indent_level);
        printf("} while (0);\n\n");
    }
}

void c_code_print_backend(IRRoot* root, const ApiSpec* api_spec) {
    (void)api_spec;
    if (!root) { printf("/* IR Root is NULL. */\n"); return; }

    printf("/* AUTO-GENERATED by the 'c_code' backend */\n\n");
    printf("#include \"lvgl.h\"\n");
    printf("#include \"dynamic_lvgl.h\" // For obj_registry_add\n\n");
    printf("void create_ui(lv_obj_t* parent) {\n");

    IdMapNode* id_map = NULL;
    build_id_map_recursive(root->root_objects, &id_map);
    id_map_add(&id_map, "parent", "parent", "lv_obj_t*");

    if (root->root_objects) {
        print_object_list(root->root_objects, 1, "parent", id_map);
    } else {
        print_indent(1);
        printf("/* (No root objects) */\n");
    }

    printf("}\n");
    id_map_free(id_map);
}

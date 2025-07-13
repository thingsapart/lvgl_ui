#include "c_code_printer.h"
#include "ir.h"
#include "api_spec.h"
#include "utils.h" // For render_abort
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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
static void print_expr(IRExpr* expr, const char* parent_c_name, IdMapNode* map, bool pass_by_ref_for_struct);
static void print_object_list(IRObject* head, int indent_level, const char* parent_c_name, IdMapNode* map);
static void print_node(IRNode* node, int indent_level, const char* parent_c_name, const char* target_c_name, IdMapNode* map);


// --- Printing Helpers ---

static void print_indent(int level) {
    for (int i = 0; i < level; ++i) printf("    ");
}

static void print_c_string_literal(const char* str, size_t len) {
    printf("\"");
    for (size_t i = 0; i < len; ++i) {
        unsigned char c = str[i];
        switch (c) {
            case '\n': printf("\\n"); break;
            case '\r': printf("\\r"); break;
            case '\t': printf("\\t"); break;
            case '"':  printf("\\\""); break;
            case '\\': printf("\\\\"); break;
            default:
                if (isprint(c)) {
                    printf("%c", c);
                } else {
                    printf("\\x%02x", c);
                }
                break;
        }
    }
    printf("\"");
}

static void print_expr_list(IRExprNode* head, const char* parent_c_name, IdMapNode* map) {
    bool first = true;
    for (IRExprNode* current = head; current; current = current->next) {
        if (!first) printf(", ");
        // When passing arguments to functions, non-pointer structs should be passed by reference.
        print_expr(current->expr, parent_c_name, map, true);
        first = false;
    }
}

static void print_expr(IRExpr* expr, const char* parent_c_name, IdMapNode* map, bool pass_by_ref_for_struct) {
    if (!expr) { printf("NULL"); return; }

    switch (expr->base.type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)expr;
            if (lit->is_string) {
                print_c_string_literal(lit->value, lit->len);
            } else {
                printf("%s", lit->value);
            }
            break;
        }
        case IR_EXPR_STATIC_STRING: {
            IRExprStaticString* sstr = (IRExprStaticString*)expr;
            print_c_string_literal(sstr->value, sstr->len);
            break;
        }
        case IR_EXPR_ENUM:
            printf("%s", ((IRExprEnum*)expr)->symbol);
            break;
        case IR_EXPR_REGISTRY_REF: {
            const char* name = ((IRExprRegistryRef*)expr)->name;
            const char* c_name_to_print = NULL;
            const char* c_type_of_ref = NULL;

            if (strcmp(name, "parent") == 0) {
                c_name_to_print = parent_c_name;
                 const IdMapNode* parent_node = id_map_get_node(map, parent_c_name);
                 if (parent_node) c_type_of_ref = parent_node->c_type;

            } else {
                const char* lookup_key = (name[0] == '@') ? name + 1 : name;
                const IdMapNode* node = id_map_get_node(map, lookup_key);
                if (node) {
                    c_name_to_print = node->c_name;
                    c_type_of_ref = node->c_type;
                } else {
                    printf("/* unresolved_ref: %s */", name);
                    return;
                }
            }

            if (c_name_to_print && c_type_of_ref) {
                bool is_pointer = strchr(c_type_of_ref, '*') != NULL;
                if (!is_pointer && pass_by_ref_for_struct) {
                     printf("&%s", c_name_to_print);
                } else {
                     printf("%s", c_name_to_print);
                }
            } else if (c_name_to_print) {
                 printf("%s", c_name_to_print); // Fallback if type info is missing
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
            IRExprArray* arr = (IRExprArray*)expr;
            if (strcmp(arr->base.c_type, "binding_value_t*") == 0) {
                 printf("(const binding_value_t[]) { ");
                 for (IRExprNode* n = arr->elements; n; n = n->next) {
                    printf("{ ");
                    if (n->expr->base.type == IR_EXPR_LITERAL) {
                        IRExprLiteral* lit = (IRExprLiteral*)n->expr;
                        if (lit->is_string) {
                            printf(".type=BINDING_TYPE_STRING, .as.s_val=");
                            print_expr(n->expr, parent_c_name, map, false);
                        } else {
                            if (strcmp(lit->value, "true") == 0 || strcmp(lit->value, "false") == 0) {
                                printf(".type=BINDING_TYPE_BOOL, .as.b_val=%s", lit->value);
                            } else if (strchr(lit->value, '.')) {
                                printf(".type=BINDING_TYPE_FLOAT, .as.f_val=%s", lit->value);
                            } else {
                                printf(".type=BINDING_TYPE_INT, .as.i_val=%s", lit->value);
                            }
                        }
                    }
                    printf(" }");
                    if (n->next) printf(", ");
                 }
            } else {
                // Original logic for primitive arrays
                char* base_type = get_array_base_type(arr->base.c_type);
                printf("(%s[]){ ", base_type ? base_type : "void*" );
                print_expr_list(arr->elements, parent_c_name, map);
                free(base_type);
            }
            printf(" }");
            break;
        }
        case IR_EXPR_RUNTIME_REG_ADD: {
            IRExprRuntimeRegAdd* reg = (IRExprRuntimeRegAdd*)expr;
            printf("obj_registry_add(\"%s\", ", reg->id);
            // when registering, we need to pass the address of a struct, but the value of a pointer.
            print_expr(reg->object_expr, parent_c_name, map, true);
            printf(")");
            break;
        }
        default:
            printf("/* UNKNOWN_EXPR */");
            break;
    }
}

static void print_node(IRNode* node, int indent_level, const char* parent_c_name, const char* target_c_name, IdMapNode* map) {
    if (!node) return;
    switch(node->type) {
        case IR_NODE_OBJECT:
            print_object_list((IRObject*)node, indent_level, target_c_name, map);
            break;
        case IR_NODE_WARNING:
            print_indent(indent_level);
            printf("// [GENERATOR HINT] %s\n", ((IRWarning*)node)->message);
            break;
        case IR_NODE_OBSERVER: {
            IRObserver* obs = (IRObserver*)node;
            print_indent(indent_level);
            printf("data_binding_add_observer(\"%s\", %s, %d, \"%s\");\n",
                   obs->state_name,
                   target_c_name,
                   obs->update_type,
                   obs->format_string ? obs->format_string : "");
            break;
        }
        case IR_NODE_ACTION: {
            IRAction* act = (IRAction*)node;
            print_indent(indent_level);
            printf("data_binding_add_action(%s, \"%s\", %d, ",
                   target_c_name,
                   act->action_name,
                   act->action_type);
            if (act->data_expr) {
                print_expr(act->data_expr, parent_c_name, map, false);
                // Also need to print the count
                if (act->data_expr->base.type == IR_EXPR_ARRAY) {
                    int count = 0;
                    for (IRExprNode* n = ((IRExprArray*)act->data_expr)->elements; n; n = n->next) count++;
                    printf(", %d", count);
                }
            } else {
                printf("NULL, 0");
            }
            printf(");\n");
            break;
        }
        default:
            // Must be an expression
            print_indent(indent_level);
            print_expr((IRExpr*)node, parent_c_name, map, false);
            printf(";\n");
            break;
    }
}


// --- Traversal and Code Generation Logic ---

static void build_id_map_recursive(IRObject* head, IdMapNode** map_head) {
    for (IRObject* current = head; current; current = current->next) {
        if (current->registered_id && current->c_name && current->c_type) {
            id_map_add(map_head, current->registered_id, current->c_name, current->c_type);
        }
        // Also map the c_name to itself for lookups
        if (current->c_name && current->c_type) {
            id_map_add(map_head, current->c_name, current->c_name, current->c_type);
        }

        if (current->operations) {
            IROperationNode* op_node = current->operations;
            while (op_node) {
                if (op_node->op_node->type == IR_NODE_OBJECT) {
                    build_id_map_recursive((IRObject*)op_node->op_node, map_head);
                }
                op_node = op_node->next;
            }
        }
        // if (current->with_blocks) ... // TODO: handle with blocks if needed
    }
}

static void print_object_list(IRObject* head, int indent_level, const char* parent_c_name, IdMapNode* map) {
    for (IRObject* current = head; current; current = current->next) {
        if(strncmp(current->json_type, "//", 2) == 0) continue; // Skip comment objects

        bool is_top_level = (indent_level == 1);
        int content_indent = is_top_level ? indent_level : (indent_level + 1);

        print_indent(indent_level);
        printf("// %s: %s (%s)\n", current->registered_id ? current->registered_id : "unnamed", current->c_name, current->json_type);
        
        if (!is_top_level) {
            print_indent(indent_level);
            printf("do {\n");
        }

        print_indent(content_indent);
        bool is_pointer = (current->c_type && strchr(current->c_type, '*') != NULL);
        if (is_pointer) {
            // All pointer types get declared.
            // If there is a constructor, they are initialized on the same line.
            // Otherwise, they are initialized to NULL.
            printf("%s %s = ", current->c_type, current->c_name);
            if (current->constructor_expr) {
                print_expr(current->constructor_expr, parent_c_name, map, false);
            } else {
                printf("NULL");
            }
             printf(";\n");
        } else {
            // Non-pointer types (structs on stack). These shouldn't have value-returning constructors.
            printf("%s %s;\n", current->c_type, current->c_name);
            if (current->constructor_expr) {
                // The constructor must be a void function like `lv_style_init(&style_0)`
                print_indent(content_indent);
                print_expr(current->constructor_expr, parent_c_name, map, false);
                printf(";\n");
            }
        }

        if (current->operations) {
            printf("\n");
            IROperationNode* op_node = current->operations;
            while(op_node) {
                print_node(op_node->op_node, content_indent, parent_c_name, current->c_name, map);
                op_node = op_node->next;
            }
        }

        if (!is_top_level) {
            print_indent(indent_level);
            printf("} while (0);\n\n");
        } else {
            printf("\n");
        }
    }
}

void c_code_print_backend(IRRoot* root, const ApiSpec* api_spec) {
    (void)api_spec;
    if (!root) { printf("/* IR Root is NULL. */\n"); return; }

    printf("/* AUTO-GENERATED by the 'c_code' backend */\n\n");
    printf("#include \"lvgl.h\"\n");
    printf("#include \"c_gen/lvgl_dispatch.h\" // For obj_registry_add\n");
    printf("#include \"data_binding.h\"\n\n");
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


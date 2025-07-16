#include "c_code_printer.h"
#include "ir.h"
#include "api_spec.h"
#include "utils.h" // For render_abort
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

// --- ID to C-Name/Type Mapping ---
typedef struct IdMapNode {
    char* id;
    char* c_name;
    char* c_type;
    struct IdMapNode* next;
} IdMapNode;

// --- Static Array to C-Name Mapping ---
typedef struct MapNode {
    const void* ir_node_ptr; // Key: The IR node pointer (IRExprArray)
    char* c_name;            // Value: The generated C variable name
    struct MapNode* next;
} MapNode;


// --- Forward Declarations ---
static void print_expr(IRExpr* expr, const char* parent_c_name, IdMapNode* id_map, MapNode* array_map, bool pass_by_ref_for_struct);
static void print_object_list(IRObject* head, int indent_level, const char* parent_c_name, IdMapNode* id_map, MapNode* array_map);
static void print_node(IRNode* node, int indent_level, const char* parent_c_name, const char* target_c_name, IdMapNode* id_map, MapNode* array_map);
static void find_and_map_arrays(IRObject* head, MapNode** array_map_head, int* counter);

// --- Map Helpers: ID Map ---
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
    return NULL;
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


// --- Map Helpers: Generic Map ---
static void generic_map_add(MapNode** map_head, const void* ir_node_ptr, const char* c_name) {
    MapNode* new_node = malloc(sizeof(MapNode));
    if (!new_node) render_abort("Failed to allocate MapNode");
    new_node->ir_node_ptr = ir_node_ptr;
    new_node->c_name = strdup(c_name);
    new_node->next = *map_head;
    *map_head = new_node;
}

static const char* generic_map_get_name(MapNode* map_head, const void* ir_node_ptr) {
    for (MapNode* current = map_head; current; current = current->next) {
        if (current->ir_node_ptr == ir_node_ptr) {
            return current->c_name;
        }
    }
    return NULL;
}

static void generic_map_free(MapNode* map_head) {
    MapNode* current = map_head;
    while (current) {
        MapNode* next = current->next;
        free(current->c_name);
        free(current);
        current = next;
    }
}


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

static void print_expr_list(IRExprNode* head, const char* parent_c_name, IdMapNode* id_map, MapNode* array_map) {
    bool first = true;
    for (IRExprNode* current = head; current; current = current->next) {
        if (!first) printf(", ");
        print_expr(current->expr, parent_c_name, id_map, array_map, true);
        first = false;
    }
}

static void print_binding_value(IRExpr* expr, const char* parent_c_name, IdMapNode* id_map, MapNode* array_map) {
    printf("{ ");
    if (expr->base.type == IR_EXPR_LITERAL) {
        IRExprLiteral* lit = (IRExprLiteral*)expr;
        if (lit->is_string) {
            printf(".type=BINDING_TYPE_STRING, .as.s_val=");
            print_expr(expr, parent_c_name, id_map, array_map, false);
        } else if (strcmp(lit->base.c_type, "bool") == 0) {
            printf(".type=BINDING_TYPE_BOOL, .as.b_val=%s", lit->value);
        } else { // It's a number, so treat as float
            printf(".type=BINDING_TYPE_FLOAT, .as.f_val=(float)%s", lit->value);
        }
    }
    printf(" }");
}


static void print_expr(IRExpr* expr, const char* parent_c_name, IdMapNode* id_map, MapNode* array_map, bool pass_by_ref_for_struct) {
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
                 const IdMapNode* parent_node = id_map_get_node(id_map, parent_c_name);
                 if (parent_node) c_type_of_ref = parent_node->c_type;

            } else {
                const char* lookup_key = (name[0] == '@') ? name + 1 : name;
                const IdMapNode* node = id_map_get_node(id_map, lookup_key);
                if (node) {
                    c_name_to_print = node->c_name;
                    c_type_of_ref = node->c_type;
                } else {
                    printf("/* unresolved_ref: %s */ NULL", name);
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
            print_expr_list(call->args, parent_c_name, id_map, array_map);
            printf(")");
            break;
        }
        case IR_EXPR_ARRAY: {
            IRExprArray* arr = (IRExprArray*)expr;
            if (strcmp(arr->base.c_type, "binding_value_t*") == 0) {
                 printf("(const binding_value_t[]) { ");
                 for (IRExprNode* n = arr->elements; n; n = n->next) {
                    print_binding_value(n->expr, parent_c_name, id_map, array_map);
                    if (n->next) printf(", ");
                 }
                 printf(" }");
                 break;
            }

            const char* array_c_name = generic_map_get_name(array_map, arr);
            if (array_c_name) {
                printf("%s", array_c_name);
            } else {
                printf("/* UNMAPPED_ARRAY */ NULL");
            }
            break;
        }
        case IR_EXPR_RUNTIME_REG_ADD: {
            IRExprRuntimeRegAdd* reg = (IRExprRuntimeRegAdd*)expr;
            printf("obj_registry_add(\"%s\", ", reg->id);
            print_expr(reg->object_expr, parent_c_name, id_map, array_map, true);
            printf(")");
            break;
        }
        default:
            printf("/* UNKNOWN_EXPR */");
            break;
    }
}

static void print_node(IRNode* node, int indent_level, const char* parent_c_name, const char* target_c_name, IdMapNode* id_map, MapNode* array_map) {
    if (!node) return;
    switch(node->type) {
        case IR_NODE_OBJECT:
            print_object_list((IRObject*)node, indent_level, target_c_name, id_map, array_map);
            break;
        case IR_NODE_WARNING:
            print_indent(indent_level);
            printf("// [GENERATOR HINT] %s\n", ((IRWarning*)node)->message);
            break;
        case IR_NODE_OBSERVER: {
            IRObserver* obs = (IRObserver*)node;
            print_indent(indent_level);
            printf("data_binding_add_observer(\"%s\", %s, %d, ", obs->state_name, target_c_name, obs->update_type);

            if (obs->config_expr->base.type == IR_EXPR_LITERAL) {
                IRExprLiteral* lit = (IRExprLiteral*)obs->config_expr;
                if (lit->is_string) {
                    print_c_string_literal(lit->value, lit->len);
                    printf(", 0, NULL");
                } else {
                    printf("&(bool){%s}, 0, NULL", lit->value);
                }
            } else if (obs->config_expr->base.type == IR_EXPR_ARRAY) { // Map
                IRExprArray* arr = (IRExprArray*)obs->config_expr;
                IRExpr* default_val_expr = NULL;
                for (IRExprNode* n = arr->elements; n; n = n->next) {
                    IRExprArray* pair = (IRExprArray*)n->expr;
                    IRExprLiteral* key_lit = (IRExprLiteral*)pair->elements->expr;
                    if (key_lit->is_string && strcmp(key_lit->value, "default") == 0) {
                        default_val_expr = pair->elements->next->expr;
                        break;
                    }
                }
                
                printf("(const binding_map_entry_t[]){ ");
                int count = 0;
                bool first = true;
                for (IRExprNode* n = arr->elements; n; n = n->next) {
                    IRExprArray* pair = (IRExprArray*)n->expr;
                    IRExpr* key_expr = pair->elements->expr;
                    if (key_expr->base.type == IR_EXPR_LITERAL && ((IRExprLiteral*)key_expr)->is_string && strcmp(((IRExprLiteral*)key_expr)->value, "default") == 0) {
                        continue;
                    }
                    if (!first) printf(", ");
                    printf("{ .key = ");
                    print_binding_value(key_expr, parent_c_name, id_map, array_map);
                    printf(", .value = { ");
                    if (obs->update_type == OBSERVER_TYPE_STYLE) {
                        printf(".p_val = (void*)");
                        print_expr(pair->elements->next->expr, parent_c_name, id_map, array_map, true);
                    } else {
                        printf(".b_val = ");
                        print_expr(pair->elements->next->expr, parent_c_name, id_map, array_map, false);
                    }
                    printf(" } }");
                    first = false;
                    count++;
                }
                printf(" }, %d, ", count);
                
                if (default_val_expr) {
                    if (obs->update_type == OBSERVER_TYPE_STYLE) {
                        if(default_val_expr->base.type == IR_EXPR_LITERAL && strcmp(((IRExprLiteral*)default_val_expr)->value, "NULL") == 0) {
                            printf("NULL");
                        } else {
                            printf("(const void*)");
                            print_expr(default_val_expr, parent_c_name, id_map, array_map, true);
                        }
                    } else {
                        printf("(const void*)&(bool){");
                        print_expr(default_val_expr, parent_c_name, id_map, array_map, false);
                        printf("}");
                    }
                } else {
                    printf("NULL");
                }
            } else {
                 printf("NULL, 0, NULL");
            }
            printf(");\n");
            break;
        }
        case IR_NODE_ACTION: {
            IRAction* act = (IRAction*)node;
            print_indent(indent_level);
            printf("data_binding_add_action(%s, \"%s\", %d, ", target_c_name, act->action_name, act->action_type);
            if (act->data_expr) {
                print_expr(act->data_expr, parent_c_name, id_map, array_map, false);
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
            print_indent(indent_level);
            print_expr((IRExpr*)node, parent_c_name, id_map, array_map, false);
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
        if (current->c_name && current->c_type) {
            id_map_add(map_head, current->c_name, current->c_name, current->c_type);
        }

        for (IROperationNode* op = current->operations; op; op = op->next) {
            if (op->op_node->type == IR_NODE_OBJECT) {
                build_id_map_recursive((IRObject*)op->op_node, map_head);
            }
        }
    }
}

static void find_and_map_in_expr(IRExpr* expr, MapNode** array_map, int* counter) {
    if (!expr) return;
    if (expr->base.type == IR_EXPR_ARRAY) {
        IRExprArray* arr = (IRExprArray*)expr;
        if (strcmp(arr->base.c_type, "binding_value_t*") != 0 && !generic_map_get_name(*array_map, arr)) {
            // Only map arrays that are NOT observer maps, as those are now inline.
            bool is_observer_map = false;
            if (arr->elements && arr->elements->expr->base.type == IR_EXPR_ARRAY) {
                is_observer_map = true; // Heuristic: nested array is likely an observer map
            }
            if (!is_observer_map) {
                char name_buf[64];
                snprintf(name_buf, sizeof(name_buf), "s_static_array_%d", (*counter)++);
                generic_map_add(array_map, arr, name_buf);
            }
        }
        for (IRExprNode* elem = arr->elements; elem; elem = elem->next) {
            find_and_map_in_expr(elem->expr, array_map, counter);
        }
    } else if (expr->base.type == IR_EXPR_FUNCTION_CALL) {
        for (IRExprNode* arg = ((IRExprFunctionCall*)expr)->args; arg; arg = arg->next) {
            find_and_map_in_expr(arg->expr, array_map, counter);
        }
    }
}

static void find_and_map_arrays(IRObject* head, MapNode** array_map, int* counter) {
    for (IRObject* current = head; current; current = current->next) {
        find_and_map_in_expr(current->constructor_expr, array_map, counter);
        for (IROperationNode* op = current->operations; op; op = op->next) {
            if (op->op_node->type == IR_NODE_OBJECT) {
                find_and_map_arrays((IRObject*)op->op_node, array_map, counter);
            } else {
                find_and_map_in_expr((IRExpr*)op->op_node, array_map, counter);
            }
        }
    }
}


static void print_object_list(IRObject* head, int indent_level, const char* parent_c_name, IdMapNode* id_map, MapNode* array_map) {
    for (IRObject* current = head; current; current = current->next) {
        if(strncmp(current->json_type, "//", 2) == 0) continue;

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
            printf("%s %s = ", current->c_type, current->c_name);
            if (current->constructor_expr) {
                print_expr(current->constructor_expr, parent_c_name, id_map, array_map, false);
            } else {
                printf("NULL");
            }
             printf(";\n");
        } else {
            printf("%s %s;\n", current->c_type, current->c_name);
            if (current->constructor_expr) {
                print_indent(content_indent);
                print_expr(current->constructor_expr, parent_c_name, id_map, array_map, false);
                printf(";\n");
            }
        }

        if (current->operations) {
            printf("\n");
            for (IROperationNode* op_node = current->operations; op_node; op_node = op_node->next) {
                print_node(op_node->op_node, content_indent, parent_c_name, current->c_name, id_map, array_map);
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

    IdMapNode* id_map = NULL;
    MapNode* array_map = NULL;
    int static_counter = 0;

    build_id_map_recursive(root->root_objects, &id_map);
    id_map_add(&id_map, "parent", "parent", "lv_obj_t*");
    find_and_map_arrays(root->root_objects, &array_map, &static_counter);

    printf("/* AUTO-GENERATED by the 'c_code' backend */\n\n");
    printf("#include \"lvgl.h\"\n");
    printf("#include \"c_gen/lvgl_dispatch.h\" // For obj_registry_add\n");
    printf("#include \"data_binding.h\"\n\n");
    
    printf("void create_ui(lv_obj_t* parent) {\n");

    if (array_map) {
        print_indent(1);
        printf("// --- Static Arrays for LVGL properties ---\n");
        for (MapNode* current = array_map; current; current = current->next) {
            const IRExprArray* arr = current->ir_node_ptr;
            char* base_type = get_array_base_type(arr->base.c_type);
            print_indent(1);
            printf("static const %s %s[] = { ", base_type, current->c_name);
            print_expr_list(arr->elements, "parent", id_map, array_map);
            printf(" };\n");
            free(base_type);
        }
        printf("\n");
    }

    if (root->root_objects) {
        print_object_list(root->root_objects, 1, "parent", id_map, array_map);
    } else {
        print_indent(1);
        printf("/* (No root objects) */\n");
    }

    printf("}\n");

    id_map_free(id_map);
    generic_map_free(array_map);
}

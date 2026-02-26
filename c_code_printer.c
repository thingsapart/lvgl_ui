#include "c_code_printer.h"
#include "ir.h"
#include "api_spec.h"
#include "utils.h" // For render_abort
#include "data_binding.h" // For NumericDialogConfig
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


// --- Deferred Object Collection ---
typedef struct DeferredNode {
    IRObject* obj;
    struct DeferredNode* next;
} DeferredNode;

// --- Hoisted Variable (cross-scope deferred refs + all malloc-constructed vars) ---
// Variables declared in create_ui or inside a deferred function that are either
// cross-scope referenced OR heap-allocated are promoted to file-scope statics.
typedef struct HoistedVarNode {
    char*  c_name;
    char*  c_type;
    bool   needs_free;        // true → LVGL_UI_FREE in destroy_ui / destroy_*
    char*  owner_fn_name;     // NULL = create_ui scope; else = the create_* fn that owns it
    struct HoistedVarNode* next;
} HoistedVarNode;

// C-name string set used during cross-scope detection.
typedef struct CNameSetNode {
    char*              c_name;
    struct CNameSetNode* next;
} CNameSetNode;

// --- Forward Declarations ---
static void print_expr(IRExpr* expr, const char* parent_c_name, IdMapNode* id_map, MapNode* array_map, bool pass_by_ref_for_struct);
static void print_object_list(IRObject* head, int indent_level, const char* parent_c_name, IdMapNode* id_map, MapNode* array_map, HoistedVarNode* hoisted_vars);
static void print_node(IRNode* node, int indent_level, const char* parent_c_name, const char* target_c_name, IdMapNode* id_map, MapNode* array_map, HoistedVarNode* hoisted_vars);
static void find_and_map_arrays(IRObject* head, MapNode** array_map_head, int* counter);
static void id_map_dump(IdMapNode* map_head);
static void collect_deferred_objects(IRObject* head, DeferredNode** list_tail_ptr, DeferredNode** list_head_ptr);
static void emit_deferred_function(IRObject* obj, IdMapNode* global_id_map, HoistedVarNode* hoisted_vars);

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

static void id_map_dump(IdMapNode* map_head) {
    fprintf(stderr, "\n--- C Code Printer ID Map Dump ---\n");
    if (!map_head) {
        fprintf(stderr, "  (empty)\n");
    }
    for (IdMapNode* current = map_head; current; current = current->next) {
        fprintf(stderr, "  ID: %-25s -> C Name: %-20s (C Type: %s)\n", current->id, current->c_name, current->c_type);
    }
    fprintf(stderr, "--- END ID Map Dump ---\n\n");
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
        case IR_EXPR_IF_BACKEND: {
            IRIfBackend* if_node = (IRIfBackend*)expr;
            print_expr(if_node->static_expr, parent_c_name, id_map, array_map, pass_by_ref_for_struct);
            break;
        }
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

            // Handle special @$ C identifier reference
            if (strncmp(name, "@$", 2) == 0) {
                printf("%s", name + 2);
                return;
            }

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
                    fprintf(stderr, "[C Code Printer] " ANSI_BOLD_RED "Error:" ANSI_RESET " Unresolved reference '%s'.\n", name);
                    id_map_dump(id_map);
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
            // Remap malloc to the configurable UI allocator.
            const char* fn_name = (strcmp(call->func_name, "malloc") == 0)
                                  ? "LVGL_UI_MALLOC" : call->func_name;
            printf("%s(", fn_name);
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

static void print_node(IRNode* node, int indent_level, const char* parent_c_name, const char* target_c_name, IdMapNode* id_map, MapNode* array_map, HoistedVarNode* hoisted_vars) {
    if (!node) return;
    switch(node->type) {
        case IR_NODE_OBJECT:
            print_object_list((IRObject*)node, indent_level, target_c_name, id_map, array_map, hoisted_vars);
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

            if (act->action_type == ACTION_TYPE_NUMERIC_DIALOG) {
                printf("data_binding_add_action(%s, \"%s\", %d, NULL, 0, ", target_c_name, act->action_name, act->action_type);
                printf("&(const NumericDialogConfig){ ");
                if (act->data_expr && act->data_expr->base.type == IR_EXPR_ARRAY) {
                    IRExprArray* map_arr = (IRExprArray*)act->data_expr;
                    bool first = true;
                    for (IRExprNode* n = map_arr->elements; n; n = n->next) {
                         IRExprArray* pair = (IRExprArray*)n->expr;
                         IRExprLiteral* key_lit = (IRExprLiteral*)pair->elements->expr;
                         const char* c_field_name = NULL;

                         if (strcmp(key_lit->value, "min") == 0) c_field_name = "min_val";
                         else if (strcmp(key_lit->value, "max") == 0) c_field_name = "max_val";
                         else if (strcmp(key_lit->value, "initial") == 0) c_field_name = "initial_val";
                         else if (strcmp(key_lit->value, "format") == 0) c_field_name = "format_str";
                         else if (strcmp(key_lit->value, "text") == 0) c_field_name = "text";

                         if (c_field_name) {
                            if (!first) printf(", ");
                            printf(".%s = ", c_field_name);
                            print_expr(pair->elements->next->expr, parent_c_name, id_map, array_map, false);
                            first = false;
                         }
                    }
                }
                printf(" });\n");
            } else if (act->action_type == ACTION_TYPE_CYCLE) {
                printf("data_binding_add_action(%s, \"%s\", %d, ", target_c_name, act->action_name, act->action_type);
                if (act->data_expr) {
                    print_expr(act->data_expr, parent_c_name, id_map, array_map, false);
                    int count = 0;
                    if (act->data_expr->base.type == IR_EXPR_ARRAY) {
                       for (IRExprNode* n = ((IRExprArray*)act->data_expr)->elements; n; n = n->next) count++;
                    }
                    printf(", %d, NULL);\n", count);
                } else {
                     printf("NULL, 0, NULL);\n");
                }
            } else { // TRIGGER, TOGGLE
                printf("data_binding_add_action(%s, \"%s\", %d, NULL, 0, NULL);\n", target_c_name, act->action_name, act->action_type);
            }
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
    } else if (expr->base.type == IR_EXPR_IF_BACKEND) {
        IRIfBackend* if_node = (IRIfBackend*)expr;
        find_and_map_in_expr(if_node->static_expr, array_map, counter);
        find_and_map_in_expr(if_node->dynamic_expr, array_map, counter);
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


// --- Deferred Function Helpers ---

// Recursively collect all IRObjects that have a deferred_fn_name set.
// Appends to the singly-linked list formed by [*list_head_ptr, *list_tail_ptr].
// ===========================================================================
// --- Hoisted Variable Helpers ---
// ===========================================================================

static bool c_name_set_contains(const CNameSetNode* s, const char* name) {
    for (; s; s = s->next) if (strcmp(s->c_name, name) == 0) return true;
    return false;
}
static void c_name_set_add(CNameSetNode** s, const char* name) {
    if (!name || c_name_set_contains(*s, name)) return;
    CNameSetNode* n = malloc(sizeof(CNameSetNode));
    if (!n) return;
    n->c_name = strdup(name); n->next = *s; *s = n;
}
static void c_name_set_free(CNameSetNode* s) {
    while (s) { CNameSetNode* nx = s->next; free(s->c_name); free(s); s = nx; }
}

static bool is_hoisted(const HoistedVarNode* h, const char* c_name) {
    for (; h; h = h->next) if (strcmp(h->c_name, c_name) == 0) return true;
    return false;
}
static void hoisted_var_add(HoistedVarNode** h, const char* c_name,
                             const char* c_type, bool needs_free,
                             const char* owner_fn_name) {
    if (is_hoisted(*h, c_name)) return;
    HoistedVarNode* n = malloc(sizeof(HoistedVarNode));
    if (!n) return;
    n->c_name = strdup(c_name); n->c_type = strdup(c_type);
    n->needs_free = needs_free;
    n->owner_fn_name = owner_fn_name ? strdup(owner_fn_name) : NULL;
    n->next = *h; *h = n;
}
static void hoisted_var_free_list(HoistedVarNode* h) {
    while (h) {
        HoistedVarNode* nx = h->next;
        free(h->c_name); free(h->c_type); free(h->owner_fn_name); free(h); h = nx;
    }
}

// Collect every c_name declared inside a subtree (not the root itself).
static void collect_local_c_names(IRObject* head, CNameSetNode** out) {
    for (IRObject* o = head; o; o = o->next) {
        if (o->c_name) c_name_set_add(out, o->c_name);
        for (IROperationNode* op = o->operations; op; op = op->next)
            if (op->op_node->type == IR_NODE_OBJECT)
                collect_local_c_names((IRObject*)op->op_node, out);
    }
}

// Walk an expression tree, collecting registry refs that resolve to c_names
// not present in the local set — these are cross-scope (hoistable) references.
static void find_external_refs_in_expr(IRExpr* e, const CNameSetNode* local_set,
                                        IdMapNode* id_map, HoistedVarNode** out) {
    if (!e) return;
    switch (e->base.type) {
        case IR_EXPR_REGISTRY_REF: {
            const char* name = ((IRExprRegistryRef*)e)->name;
            if (strcmp(name, "parent") == 0 || strncmp(name, "@$", 2) == 0) break;
            const char* lookup = (name[0] == '@') ? name + 1 : name;
            const IdMapNode* node = id_map_get_node(id_map, lookup);
            if (!node) break;
            if (!c_name_set_contains(local_set, node->c_name))
                hoisted_var_add(out, node->c_name, node->c_type, false, NULL);
            break;
        }
        case IR_EXPR_FUNCTION_CALL:
            for (IRExprNode* a = ((IRExprFunctionCall*)e)->args; a; a = a->next)
                find_external_refs_in_expr(a->expr, local_set, id_map, out);
            break;
        case IR_EXPR_IF_BACKEND:
            find_external_refs_in_expr(((IRIfBackend*)e)->static_expr, local_set, id_map, out);
            find_external_refs_in_expr(((IRIfBackend*)e)->dynamic_expr, local_set, id_map, out);
            break;
        case IR_EXPR_ARRAY:
            for (IRExprNode* a = ((IRExprArray*)e)->elements; a; a = a->next)
                find_external_refs_in_expr(a->expr, local_set, id_map, out);
            break;
        default: break;
    }
}

// Recursively scan an IR subtree for external refs (the body of a deferred fn).
static void scan_subtree_for_external_refs(IRObject* head, const CNameSetNode* local_set,
                                            IdMapNode* id_map, HoistedVarNode** out) {
    for (IRObject* o = head; o; o = o->next) {
        find_external_refs_in_expr(o->constructor_expr, local_set, id_map, out);
        for (IROperationNode* op = o->operations; op; op = op->next) {
            if (op->op_node->type == IR_NODE_OBJECT) {
                scan_subtree_for_external_refs((IRObject*)op->op_node, local_set, id_map, out);
            } else {
                IRNode* n = op->op_node;
                if (n->type == IR_NODE_OBSERVER)
                    find_external_refs_in_expr(((IRObserver*)n)->config_expr, local_set, id_map, out);
                else if (n->type == IR_NODE_ACTION)
                    find_external_refs_in_expr(((IRAction*)n)->data_expr, local_set, id_map, out);
                else
                    find_external_refs_in_expr((IRExpr*)n, local_set, id_map, out);
            }
        }
    }
}

// Find the IRObject in the IR tree whose c_name matches.
static IRObject* find_ir_obj_by_c_name(IRObject* head, const char* c_name) {
    for (IRObject* o = head; o; o = o->next) {
        if (o->c_name && strcmp(o->c_name, c_name) == 0) return o;
        for (IROperationNode* op = o->operations; op; op = op->next)
            if (op->op_node->type == IR_NODE_OBJECT) {
                IRObject* found = find_ir_obj_by_c_name((IRObject*)op->op_node, c_name);
                if (found) return found;
            }
    }
    return NULL;
}

// Walk the IR subtree collecting all malloc-constructed objects.
// Respects deferred scope boundaries: if `o` has deferred_fn_name, its IR_NODE_OBJECT
// children belong to that deferred scope (owner_fn_name = o->deferred_fn_name).
// Objects that are not malloc-constructed are skipped (they live on the LVGL heap or
// are LVGL widgets managed by LVGL itself — no LVGL_UI_FREE needed).
static void collect_malloc_vars_for_scope(IRObject* head, const char* owner_fn_name,
                                           HoistedVarNode** out) {
    for (IRObject* o = head; o; o = o->next) {
        // Is this object heap-allocated via malloc?
        if (o->c_name && o->constructor_expr &&
            o->constructor_expr->base.type == IR_EXPR_FUNCTION_CALL &&
            strcmp(((IRExprFunctionCall*)o->constructor_expr)->func_name, "malloc") == 0) {
            hoisted_var_add(out, o->c_name, o->c_type, true, owner_fn_name);
        }
        // Recurse into children, honouring deferred scope boundaries.
        for (IROperationNode* op = o->operations; op; op = op->next) {
            if (op->op_node->type == IR_NODE_OBJECT) {
                IRObject* child = (IRObject*)op->op_node;
                // If `o` itself is deferred, its IR_NODE_OBJECT children are emitted
                // inside create_fn — they belong to that deferred scope.
                const char* child_owner = o->deferred_fn_name ? o->deferred_fn_name : owner_fn_name;
                collect_malloc_vars_for_scope(child, child_owner, out);
            }
        }
    }
}

// Build the list of variables that must be hoisted to file scope.
// Two categories are collected:
//   1. ALL malloc-constructed objects anywhere in the IR tree (via collect_malloc_vars_for_scope).
//      These carry a correct owner_fn_name and needs_free=true.
//   2. Non-malloc cross-scope refs (objects created without malloc but referenced by a
//      deferred function that can't see them). These get needs_free=false, owner_fn_name=NULL.
static HoistedVarNode* collect_hoisted_vars(DeferredNode* deferred_list,
                                             IdMapNode* global_id_map,
                                             IRObject* ir_root_objects) {
    HoistedVarNode* hoisted = NULL;

    // Pass 1: hoist every malloc-constructed object in the full tree.
    collect_malloc_vars_for_scope(ir_root_objects, NULL, &hoisted);

    // Pass 2: hoist any remaining cross-scope refs (non-malloc objects that a
    // deferred function references from the outer scope).
    for (DeferredNode* dn = deferred_list; dn; dn = dn->next) {
        IRObject* def = dn->obj;
        // Build local set: deferred object itself (→ "parent") + all its children.
        CNameSetNode* local = NULL;
        if (def->c_name) c_name_set_add(&local, def->c_name);
        for (IROperationNode* op = def->operations; op; op = op->next)
            if (op->op_node->type == IR_NODE_OBJECT)
                collect_local_c_names((IRObject*)op->op_node, &local);
        // Scan child subtrees for external refs not already in the hoisted list.
        for (IROperationNode* op = def->operations; op; op = op->next)
            if (op->op_node->type == IR_NODE_OBJECT)
                scan_subtree_for_external_refs((IRObject*)op->op_node, local, global_id_map, &hoisted);
        c_name_set_free(local);
    }

    return hoisted;
}

// Derive a destroy function name from a create function name.
//   "create_home_tab" -> "destroy_home_tab"
//   "build_page"      -> "destroy_build_page"
static char* make_destroy_fn_name(const char* create_name) {
    static const char pfx[] = "create_";
    const char* suffix = (strncmp(create_name, pfx, sizeof(pfx)-1) == 0)
                         ? create_name + sizeof(pfx) - 1 : create_name;
    bool used_suffix = (suffix != create_name);
    size_t len = strlen("destroy_") + strlen(suffix) + 1;
    char* buf = malloc(len);
    if (buf) snprintf(buf, len, "destroy_%s", suffix);
    (void)used_suffix;
    return buf;
}

// ===========================================================================

static void collect_deferred_objects(IRObject* head, DeferredNode** list_tail_ptr, DeferredNode** list_head_ptr) {
    for (IRObject* obj = head; obj; obj = obj->next) {
        if (obj->deferred_fn_name) {
            DeferredNode* dn = malloc(sizeof(DeferredNode));
            if (!dn) continue;
            dn->obj = obj;
            dn->next = NULL;
            if (!*list_head_ptr) {
                *list_head_ptr = dn;
                *list_tail_ptr = dn;
            } else {
                (*list_tail_ptr)->next = dn;
                *list_tail_ptr = dn;
            }
        }
        // Recurse into child operations (even if this object itself is deferred,
        // a deferred child could itself have deferred grandchildren)
        for (IROperationNode* op = obj->operations; op; op = op->next) {
            if (op->op_node->type == IR_NODE_OBJECT) {
                collect_deferred_objects((IRObject*)op->op_node, list_tail_ptr, list_head_ptr);
            }
        }
    }
}

// Emit one deferred function: static void fn_name(lv_obj_t* parent) { ... }
// The function body contains only the child objects of `obj` (IR_NODE_OBJECT operations).
// Non-child operations (style calls etc.) of `obj` itself remain in the caller.
static void emit_deferred_function(IRObject* obj, IdMapNode* global_id_map, HoistedVarNode* hoisted_vars) {
    if (!obj || !obj->deferred_fn_name) return;

    printf("static void %s(lv_obj_t* parent) {\n", obj->deferred_fn_name);

    // Build a local id_map:
    //  1. Copy all global entries (so @references to other objects still resolve)
    //  2. Prepend obj->c_name → "parent" last so it shadows the global entry,
    //     remapping the deferred object's variable name to the function's parameter.
    IdMapNode* local_id_map = NULL;
    for (IdMapNode* n = global_id_map; n; n = n->next) {
        id_map_add(&local_id_map, n->id, n->c_name, n->c_type);
    }
    id_map_add(&local_id_map, "parent", "parent", "lv_obj_t*");
    id_map_add(&local_id_map, obj->c_name, "parent", "lv_obj_t*"); // shadows global entry

    // Build a local array_map for static arrays needed by the children
    MapNode* local_array_map = NULL;
    int local_counter = 0;
    for (IROperationNode* op = obj->operations; op; op = op->next) {
        if (op->op_node->type == IR_NODE_OBJECT) {
            find_and_map_arrays((IRObject*)op->op_node, &local_array_map, &local_counter);
        }
    }

    // Emit any static arrays required by children
    if (local_array_map) {
        print_indent(1);
        printf("// --- Static Arrays ---\n");
        for (MapNode* am = local_array_map; am; am = am->next) {
            const IRExprArray* arr = am->ir_node_ptr;
            char* base_type = get_array_base_type(arr->base.c_type);
            print_indent(1);
            printf("static const %s %s[] = { ", base_type, am->c_name);
            print_expr_list(arr->elements, "parent", local_id_map, local_array_map);
            printf(" };\n");
            free(base_type);
        }
        printf("\n");
    }

    // Emit only the child objects of the deferred object.
    // Pass hoisted_vars so that file-scope statics get plain assignment (no type prefix).
    for (IROperationNode* op = obj->operations; op; op = op->next) {
        if (op->op_node->type == IR_NODE_OBJECT) {
            print_object_list((IRObject*)op->op_node, 1, "parent", local_id_map, local_array_map, hoisted_vars);
        }
    }

    printf("}\n\n");

    // Emit the paired destroy_* function.
    // It cleans the children of the panel but keeps the panel itself alive
    // (so the deferred_loader can call create_* again on the same slot).
    // It also frees any malloc-constructed objects that belong to this deferred scope.
    char* destroy_name = make_destroy_fn_name(obj->deferred_fn_name);
    if (destroy_name) {
        printf("static void %s(lv_obj_t** obj_ptr) {\n", destroy_name);
        printf("    if (!obj_ptr || !*obj_ptr) return;\n");
        printf("    lv_obj_clean(*obj_ptr);\n");
        // Free every hoisted var owned by this deferred function.
        bool any_freed = false;
        for (HoistedVarNode* h = hoisted_vars; h; h = h->next) {
            if (h->needs_free && h->owner_fn_name &&
                strcmp(h->owner_fn_name, obj->deferred_fn_name) == 0) {
                printf("    LVGL_UI_FREE(%s); %s = NULL;\n", h->c_name, h->c_name);
                any_freed = true;
            }
        }
        (void)any_freed;
        printf("    *obj_ptr = NULL;\n");
        printf("}\n\n");
        free(destroy_name);
    }

    id_map_free(local_id_map);
    generic_map_free(local_array_map);
}

static void print_object_list(IRObject* head, int indent_level, const char* parent_c_name, IdMapNode* id_map, MapNode* array_map, HoistedVarNode* hoisted_vars) {
    for (IRObject* current = head; current; current = current->next) {
        if(strncmp(current->json_type, "//", 2) == 0) continue;

        bool is_top_level = (indent_level == 1);
        int content_indent = is_top_level ? indent_level : (indent_level + 1);

        print_indent(indent_level);
        printf("// %s: %s (%s)\n", current->registered_id ? current->registered_id : "unnamed", current->c_name, current->json_type);

        if (strcmp(current->json_type, "font") == 0) {
            print_indent(content_indent);
            printf("#ifdef LOAD_FONTS_TTF\n");
            print_indent(content_indent);
            printf("%s %s = ", current->c_type, current->c_name);
            if (current->constructor_expr) {
                print_expr(current->constructor_expr, parent_c_name, id_map, array_map, false);
            } else {
                printf("NULL");
            }
            printf(";\n");

            print_indent(content_indent);
            printf("#else\n");
            print_indent(content_indent);
            if (current->registered_id) {
                printf("extern const lv_font_t %s;\n", current->registered_id);
                print_indent(content_indent);
                printf("%s %s = &%s;\n",
                       current->c_type, current->c_name, current->registered_id);
            } else {
                printf("%s %s = NULL; /* ERROR: Font for static build must have an 'id' */\n",
                       current->c_type, current->c_name);
            }

            print_indent(content_indent);
            printf("#endif\n\n");
            // Fonts have no operations or children, so we can continue to the next sibling.
            continue;
        }


        if (!is_top_level) {
            print_indent(indent_level);
            printf("do {\n");
        }

        print_indent(content_indent);
        bool is_pointer = (current->c_type && strchr(current->c_type, '*') != NULL);
        bool obj_is_hoisted = hoisted_vars && is_hoisted(hoisted_vars, current->c_name);

        if (strcmp(current->c_type, "const char*") == 0) {
            if (!obj_is_hoisted) printf("%s ", current->c_type);
            printf("%s = ", current->c_name);
            if (current->constructor_expr) {
                print_expr(current->constructor_expr, parent_c_name, id_map, array_map, false);
            } else {
                printf("NULL");
            }
            printf(";\n");
        } else if (is_pointer) {
            if (!obj_is_hoisted) printf("%s ", current->c_type);
            printf("%s = ", current->c_name);
            if (current->constructor_expr) {
                print_expr(current->constructor_expr, parent_c_name, id_map, array_map, false);
            } else {
                printf("NULL");
            }
             printf(";\n");
        } else {
            if (!obj_is_hoisted) printf("%s ", current->c_type);
            printf("%s;\n", current->c_name);
            if (current->constructor_expr) {
                print_indent(content_indent);
                print_expr(current->constructor_expr, parent_c_name, id_map, array_map, false);
                printf(";\n");
            }
        }

        if (current->operations) {
            printf("\n");
            if (current->deferred_fn_name) {
                // Deferred: emit non-child operations inline, then register with the deferred loader.
                bool has_non_child_ops = false;
                for (IROperationNode* op_node = current->operations; op_node; op_node = op_node->next) {
                    if (op_node->op_node->type != IR_NODE_OBJECT) { has_non_child_ops = true; break; }
                }
                if (has_non_child_ops) {
                    for (IROperationNode* op_node = current->operations; op_node; op_node = op_node->next) {
                        if (op_node->op_node->type != IR_NODE_OBJECT) {
                            print_node(op_node->op_node, content_indent, parent_c_name, current->c_name, id_map, array_map, hoisted_vars);
                        }
                    }
                }
                // Register this child with its scroll-container parent for lazy loading.
                print_indent(content_indent);
                printf("deferred_loader_register(%s, %s, %s);\n",
                       parent_c_name, current->c_name, current->deferred_fn_name);
            } else {
                for (IROperationNode* op_node = current->operations; op_node; op_node = op_node->next) {
                    print_node(op_node->op_node, content_indent, parent_c_name, current->c_name, id_map, array_map, hoisted_vars);
                }
                // If any direct child was deferred, install the lazy-load event handler now.
                bool has_deferred_children = false;
                for (IROperationNode* op_node = current->operations; op_node; op_node = op_node->next) {
                    if (op_node->op_node->type == IR_NODE_OBJECT &&
                        ((IRObject*)op_node->op_node)->deferred_fn_name) {
                        has_deferred_children = true;
                        break;
                    }
                }
                if (has_deferred_children) {
                    print_indent(content_indent);
                    printf("deferred_loader_init(%s);\n", current->c_name);
                }
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
    /* Ensure default LVGL font identifiers are known to the C-code backend */
    id_map_add(&id_map, "LV_FONT_DEFAULT", "LV_FONT_DEFAULT", "lv_font_t*");
    id_map_add(&id_map, "lv_font_default", "LV_FONT_DEFAULT", "lv_font_t*");
    id_map_add(&id_map, "@LV_FONT_DEFAULT", "LV_FONT_DEFAULT", "lv_font_t*");
    id_map_add(&id_map, "@lv_font_default", "LV_FONT_DEFAULT", "lv_font_t*");
    find_and_map_arrays(root->root_objects, &array_map, &static_counter);

    printf("/* AUTO-GENERATED by the 'c_code' backend */\n\n");
    printf("#include \"lvgl.h\"\n");
    printf("#include \"lvgl_ui.h\"\n");
    printf("#include <stdlib.h> // For malloc\n\n");

    // --- Allocator macros (override by defining before including generated code) ---
    printf("// --- Memory Allocator (define LVGL_UI_MALLOC/FREE before this file to override) ---\n");
    printf("#ifndef LVGL_UI_MALLOC\n");
    printf("  #if defined(ESP32_HW) && defined(BOARD_HAS_PSRAM)\n");
    printf("    #include \"esp_heap_caps.h\"\n");
    printf("    #define LVGL_UI_MALLOC(sz) heap_caps_malloc((sz), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)\n");
    printf("    #define LVGL_UI_FREE(ptr)  heap_caps_free(ptr)\n");
    printf("  #else\n");
    printf("    #define LVGL_UI_MALLOC(sz) malloc((sz))\n");
    printf("    #define LVGL_UI_FREE(ptr)  free(ptr)\n");
    printf("  #endif\n");
    printf("#endif\n");
    printf("#ifndef LVGL_UI_FREE\n");
    printf("  #define LVGL_UI_FREE(ptr) free(ptr)\n");
    printf("#endif\n\n");

    /* Emit any custom includes requested in the API spec (c_gen). */
    if (api_spec && api_spec->includes_c_gen) {
        const cJSON* inc_node = api_spec->includes_c_gen;
        printf("// --- Custom Includes from API Spec ---\n");
        if (cJSON_IsString((cJSON*)inc_node)) {
            const char* s = cJSON_GetStringValue((cJSON*)inc_node);
            if (s && s[0]) {
                if (strncmp(s, "#include", 8) == 0) printf("%s\n", s);
                else if (s[0] == '<' || s[0] == '"') printf("#include %s\n", s);
                else printf("#include \"%s\"\n", s);
            }
        } else if (cJSON_IsArray((cJSON*)inc_node)) {
            cJSON* it = NULL;
            cJSON_ArrayForEach(it, (cJSON*)inc_node) {
                if (!cJSON_IsString(it)) continue;
                const char* s = it->valuestring;
                if (!s || !s[0]) continue;
                if (strncmp(s, "#include", 8) == 0) printf("%s\n", s);
                else if (s[0] == '<' || s[0] == '"') printf("#include %s\n", s);
                else printf("#include \"%s\"\n", s);
            }
        }
        printf("\n");
    }

    // --- Collect deferred objects and hoisted variables ---
    DeferredNode* deferred_head = NULL;
    DeferredNode* deferred_tail = NULL;
    collect_deferred_objects(root->root_objects, &deferred_tail, &deferred_head);
    HoistedVarNode* hoisted_vars = collect_hoisted_vars(deferred_head, id_map, root->root_objects);

    // --- Emit hoisted variable declarations (file-scope statics) ---
    if (hoisted_vars) {
        printf("// --- Hoisted Variables (shared between create_ui and deferred functions) ---\n");
        for (HoistedVarNode* h = hoisted_vars; h; h = h->next) {
            bool hv_is_ptr = strchr(h->c_type, '*') != NULL;
            if (hv_is_ptr)
                printf("static %s %s = NULL;\n", h->c_type, h->c_name);
            else
                printf("static %s %s;\n", h->c_type, h->c_name);
        }
        printf("\n");
    }

    // --- Collect and emit deferred create_*/destroy_* pairs (before create_ui in C) ---
    if (deferred_head) {
        printf("// --- Deferred UI Functions ---\n\n");
        for (DeferredNode* dn = deferred_head; dn; dn = dn->next) {
            emit_deferred_function(dn->obj, id_map, hoisted_vars);
        }
    }
    // Free the DeferredNode list (not the IRObjects — those belong to the IR)
    for (DeferredNode* dn = deferred_head; dn; ) {
        DeferredNode* next = dn->next;
        free(dn);
        dn = next;
    }

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
        print_object_list(root->root_objects, 1, "parent", id_map, array_map, hoisted_vars);
    } else {
        print_indent(1);
        printf("/* (No root objects) */\n");
    }

    printf("}\n\n");

    // --- Emit destroy_ui ---
    printf("void destroy_ui(lv_obj_t** root_ptr) {\n");
    printf("    if (!root_ptr || !*root_ptr) return;\n");
    printf("    lv_obj_delete(*root_ptr);\n");
    printf("    *root_ptr = NULL;\n");
    if (hoisted_vars) {
        printf("    // Free all hoisted (file-scope) allocations.\n");
        printf("    // destroy_* fns already NULL these after their own frees, so free(NULL) is safe.\n");
        for (HoistedVarNode* h = hoisted_vars; h; h = h->next) {
            if (h->needs_free) {
                printf("    LVGL_UI_FREE(%s); %s = NULL;\n", h->c_name, h->c_name);
            }
        }
    }
    printf("}\n");

    id_map_free(id_map);
    generic_map_free(array_map);
    hoisted_var_free_list(hoisted_vars);
}

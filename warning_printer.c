#include "warning_printer.h"
#include "ir.h"
#include "utils.h"
#include <stdio.h>
#include <string.h>

#define MAX_PATH_DEPTH 64

// --- Forward Declarations ---
static void process_object_for_warnings(IRObject* obj, IRObject** path, int depth);
static bool check_list_for_warnings(IRObject* head);

// --- Helper Functions ---

static const char* get_object_display_name(IRObject* obj) {
    if (obj->registered_id && obj->registered_id[0] != '\0') {
        return obj->registered_id;
    }
    if (obj->c_name && obj->c_name[0] != '\0') {
        return obj->c_name;
    }
    return obj->json_type;
}

static bool check_object_for_warnings(IRObject* obj) {
    if (!obj) return false;
    if (obj->operations) {
        for (IROperationNode* op = obj->operations; op; op = op->next) {
            if (op->op_node->type == IR_NODE_WARNING) {
                return true;
            }
            if (op->op_node->type == IR_NODE_OBJECT) {
                if (check_object_for_warnings((IRObject*)op->op_node)) {
                    return true;
                }
            }
        }
    }
    return false;
}

static bool check_list_for_warnings(IRObject* head) {
    for (IRObject* current = head; current; current = current->next) {
        if (check_object_for_warnings(current)) {
            return true;
        }
    }
    return false;
}

// --- Main Traversal Logic ---

static void process_object_for_warnings(IRObject* obj, IRObject** path, int depth) {
    if (depth >= MAX_PATH_DEPTH || !obj) return;

    path[depth] = obj;

    // Check operations of the current object for warnings
    if (obj->operations) {
        for (IROperationNode* op = obj->operations; op; op = op->next) {
            if (op->op_node->type == IR_NODE_WARNING) {
                IRWarning* warn = (IRWarning*)op->op_node;

                // Print the path to the object containing the warning
                printf("  Path: ");
                for (int i = 0; i <= depth; ++i) {
                    const char* name = get_object_display_name(path[i]);
                    // Add '@' prefix back for registered IDs for clarity
                    if (path[i]->registered_id && path[i]->registered_id[0] != '\0') {
                        printf("@%s%s", name, (i < depth) ? " -> " : "");
                    } else {
                        printf("%s%s", name, (i < depth) ? " -> " : "");
                    }
                }
                printf("\n");

                // Print the warning/hint message itself
                if (strstr(warn->message, "consider using") || strstr(warn->message, "For clarity")) {
                     print_hint("%s\n", warn->message);
                } else {
                     print_warning("%s\n", warn->message);
                }
            }
        }
    }

    // Recurse into children, which are also in the operations list
    if (obj->operations) {
        for (IROperationNode* op = obj->operations; op; op = op->next) {
            if (op->op_node->type == IR_NODE_OBJECT) {
                process_object_for_warnings((IRObject*)op->op_node, path, depth + 1);
            }
        }
    }

    path[depth] = NULL; // Backtrack
}

void warning_print_backend(IRRoot* root) {
    if (!root || !check_list_for_warnings(root->root_objects)) {
        return;
    }

    printf("\n--- Summary of Generator Hints and Warnings ---\n");
    IRObject* path[MAX_PATH_DEPTH] = {0};

    // Iterate through the top-level (root) objects
    for (IRObject* current = root->root_objects; current; current = current->next) {
        process_object_for_warnings(current, path, 0);
    }

    printf("-------------------------------------------\n\n");
}

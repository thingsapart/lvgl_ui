#include "view_inspector.h"
#include "ir.h"
#include "api_spec.h"
#include "utils.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VIEW_INSPECTOR_DEBUG    0   // Set to 1 to enable printf debugging
#define MAX_INSPECTOR_NODES     2048
#define MAX_DISPLAY_NAME_LEN    256
#define PROP_TABLE_ROW_VPAD     8   // Vertical padding for property rows

// --- Data Structures ---

typedef struct InspectorTreeNode {
    IRNode* ir_node;
    void* lv_obj_ptr;
    bool expanded;
    int depth;

    // Tree structure
    struct InspectorTreeNode* parent;
    struct InspectorTreeNode* children_head;
    struct InspectorTreeNode* next;
} InspectorTreeNode;

typedef struct {
    lv_obj_t* parent_container;
    lv_obj_t* tree_table;
    lv_obj_t* prop_table;

    InspectorTreeNode* root_inspector_node;
    InspectorTreeNode* selected_node;
    
    ApiSpec* api_spec; // Added to access widget definitions

    // Flat list for easy lookup
    InspectorTreeNode* all_nodes[MAX_INSPECTOR_NODES];
    int node_count;

    // Map from visible table row to the node it represents
    InspectorTreeNode* row_map[MAX_INSPECTOR_NODES];
    int visible_node_count;

} InspectorContext;

static InspectorContext ctx;

// --- Forward Declarations ---
static void on_tree_selection_changed(lv_event_t* e);
static void tree_table_draw_event_cb(lv_event_t * e);
static void prop_table_draw_event_cb(lv_event_t* e);
static void rebuild_tree_view(void);
static void populate_property_view(InspectorTreeNode* node);
static void build_inspector_tree_recursive(IRNode* ir_node, InspectorTreeNode* parent_node, int depth);
static const char* get_ir_node_type_str(int type);
static void sprint_expr_value(IRExpr* expr, char* buf, size_t buf_size);

// --- Helper Functions ---
static const char* get_node_type_icon_str(IRNode* node) {
    if (!node) return LV_SYMBOL_DUMMY;
    switch(node->type) {
        case IR_NODE_OBJECT: {
            IRObject* obj = (IRObject*)node;
            if (strcmp(obj->json_type, "style") == 0) return LV_SYMBOL_TINT;
            if (strcmp(obj->json_type, "use-view") == 0) return LV_SYMBOL_PASTE;
            return LV_SYMBOL_FILE;
        }
        case IR_NODE_COMPONENT_DEF: return LV_SYMBOL_DRIVE;
        case IR_EXPR_FUNCTION_CALL: return LV_SYMBOL_PLAY;
        case IR_NODE_WARNING: return LV_SYMBOL_WARNING;
        case IR_EXPR_ARRAY: return LV_SYMBOL_LIST;
        default: return LV_SYMBOL_BULLET;
    }
}

static void get_node_display_name(InspectorTreeNode* node, char* buf, size_t buf_size) {
    if (!node || !node->ir_node) {
        strncpy(buf, "N/A", buf_size - 1);
        buf[buf_size - 1] = '\0';
        return;
    }

    char name_buf[256] = "unnamed";
    if (node->ir_node->type == IR_NODE_OBJECT) {
        IRObject* obj = (IRObject*)node->ir_node;
        char id_buf[128] = "";
        if (obj->registered_id) {
            snprintf(id_buf, sizeof(id_buf), "@%s/", obj->registered_id);
        }
        snprintf(name_buf, sizeof(name_buf), "%s %s%s", obj->json_type, id_buf, obj->c_name);

    } else if (node->ir_node->type == IR_EXPR_FUNCTION_CALL) {
        snprintf(name_buf, sizeof(name_buf), "%s()", ((IRExprFunctionCall*)node->ir_node)->func_name);
    } else if (node->ir_node->type == IR_NODE_WARNING) {
        strncpy(name_buf, "Warning", sizeof(name_buf)-1);
    } else {
        strncpy(name_buf, get_ir_node_type_str(node->ir_node->type), sizeof(name_buf)-1);
    }

    const char* expand_char = "";
    if (node->children_head) {
        expand_char = node->expanded ? LV_SYMBOL_MINUS : LV_SYMBOL_PLUS;
    }

    // Create indent string
    char indent_str[128] = {0};
    if (node->depth > 0) {
        memset(indent_str, ' ', (size_t)node->depth * 4);
    }
    
    snprintf(buf, buf_size, "%s%s %s %s",
             indent_str, expand_char, get_node_type_icon_str(node->ir_node), name_buf);
}


static InspectorTreeNode* create_inspector_node(IRNode* ir_node, InspectorTreeNode* parent_node, int depth) {
    if (ctx.node_count >= MAX_INSPECTOR_NODES) return NULL;

    InspectorTreeNode* new_node = calloc(1, sizeof(InspectorTreeNode));
    if (!new_node) return NULL;

    new_node->ir_node = ir_node;
    new_node->parent = parent_node;
    new_node->depth = depth;
    new_node->expanded = true;

    ctx.all_nodes[ctx.node_count++] = new_node;

    #if VIEW_INSPECTOR_DEBUG
        char indent[64] = {0};
        if(depth > 0) memset(indent, ' ', depth * 2);
        printf("%s[Debug Inspector] Created node %d: depth=%d, type=%s\n", indent, ctx.node_count - 1, depth, get_ir_node_type_str(ir_node->type));
    #endif

    return new_node;
}

// --- Tree Building Logic ---

static void build_inspector_tree_from_operations(IROperationNode* op_head, InspectorTreeNode* parent_node, int depth) {
    if (!op_head) return;
    
    InspectorTreeNode** current_child_ptr = &parent_node->children_head;
    IROperationNode* op_node = op_head;
    while(op_node) {
        InspectorTreeNode* new_node = create_inspector_node(op_node->op_node, parent_node, depth);
        if (new_node) {
            *current_child_ptr = new_node;
            current_child_ptr = &new_node->next;
            build_inspector_tree_recursive(op_node->op_node, new_node, depth);
        }
        op_node = op_node->next;
    }
}

static void build_inspector_tree_recursive(IRNode* ir_node, InspectorTreeNode* parent_node, int depth) {
    if (!ir_node || !parent_node) return;

    if (ir_node->type == IR_NODE_OBJECT) {
        IRObject* obj = (IRObject*)ir_node;
        if (obj->operations) {
            build_inspector_tree_from_operations(obj->operations, parent_node, depth + 1);
        }
    }
}

static void build_inspector_tree(IRRoot* ir_root) {
    if (!ir_root) return;
    // Create an invisible root node to hold the top-level items
    ctx.root_inspector_node = create_inspector_node((IRNode*)ir_root, NULL, -1);
    if (!ctx.root_inspector_node) return;
    ctx.root_inspector_node->expanded = true;

    InspectorTreeNode** current_child_ptr = &ctx.root_inspector_node->children_head;
    for (IRObject* obj = ir_root->root_objects; obj; obj = obj->next) {
        InspectorTreeNode* node = create_inspector_node((IRNode*)obj, ctx.root_inspector_node, 0);
        if (node) {
            *current_child_ptr = node;
            current_child_ptr = &node->next;
            build_inspector_tree_recursive((IRNode*)obj, node, 0);
        }
    }
}

// --- UI Drawing and Events ---

static void populate_visible_nodes_recursive(InspectorTreeNode* node, int* current_row) {
    if (!node || *current_row >= MAX_INSPECTOR_NODES) return;

    if (node != ctx.root_inspector_node) { // Don't show the invisible root
        ctx.row_map[*current_row] = node;
        (*current_row)++;
    }

    if (node->expanded && node->children_head) {
        for (InspectorTreeNode* child = node->children_head; child; child = child->next) {
            populate_visible_nodes_recursive(child, current_row);
        }
    }
}

static void rebuild_tree_view() {
    // 1. Traverse our internal tree to build a flat list of visible nodes.
    memset(ctx.row_map, 0, sizeof(ctx.row_map));
    int visible_count = 0;
    populate_visible_nodes_recursive(ctx.root_inspector_node, &visible_count);
    ctx.visible_node_count = visible_count;

    #if VIEW_INSPECTOR_DEBUG
        printf("[Debug Inspector] Rebuilding tree view with %d visible nodes.\n", visible_count);
    #endif

    // 2. Set the table row count and populate cell text values.
    lv_table_set_row_count(ctx.tree_table, ctx.visible_node_count);
    char display_buf[MAX_DISPLAY_NAME_LEN];
    for (int i = 0; i < ctx.visible_node_count; i++) {
        get_node_display_name(ctx.row_map[i], display_buf, sizeof(display_buf));
        lv_table_set_cell_value(ctx.tree_table, i, 0, display_buf);
        #if VIEW_INSPECTOR_DEBUG
            printf("  - Row %d: %s\n", i, display_buf);
        #endif
    }
}

static void tree_table_draw_event_cb(lv_event_t * e) {
    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    
    // If the cells are drawn...
    if(base_dsc->part != LV_PART_ITEMS) return;

    uint32_t row = base_dsc->id1;
    if (row >= ctx.visible_node_count) return;

    InspectorTreeNode* node = ctx.row_map[row];
    if (!node) return;

    if(lv_draw_task_get_type(draw_task) == LV_DRAW_TASK_TYPE_FILL) {
        // Highlight selected row
        if (node == ctx.selected_node) {
            lv_draw_fill_dsc_t * fill_dsc = lv_draw_task_get_fill_dsc(draw_task);
            if(fill_dsc) {
                fill_dsc->color = lv_palette_main(LV_PALETTE_BLUE);
                fill_dsc->opa = LV_OPA_COVER;
            }
        }
    }
    else if(lv_draw_task_get_type(draw_task) == LV_DRAW_TASK_TYPE_LABEL) {
        lv_draw_label_dsc_t * label_dsc = lv_draw_task_get_label_dsc(draw_task);
        if (node == ctx.selected_node) {
             label_dsc->color = lv_color_white();
        }
    }
}

static void on_tree_selection_changed(lv_event_t* e) {
    lv_obj_t* table = lv_event_get_target(e);
    uint32_t row, col;
    lv_table_get_selected_cell(table, &row, &col);

    if (row >= ctx.visible_node_count) return;

    InspectorTreeNode* node = ctx.row_map[row];
    if (!node) return;

    // Toggle expansion if it has children
    if (node->children_head) {
        node->expanded = !node->expanded;
    }
    
    // Always update selection
    ctx.selected_node = node;
    
    rebuild_tree_view();
    populate_property_view(node);
    lv_obj_invalidate(table);
}

static void add_prop(const char* key, const char* value_fmt, ...) {
    uint32_t row = lv_table_get_row_count(ctx.prop_table);
    lv_table_set_row_count(ctx.prop_table, row + 1);

    lv_table_set_cell_value(ctx.prop_table, row, 0, key);

    char value_buf[256];
    va_list args;
    va_start(args, value_fmt);
    vsnprintf(value_buf, sizeof(value_buf), value_fmt, args);
    va_end(args);

    lv_table_set_cell_value(ctx.prop_table, row, 1, value_buf);
}

static const char* flex_flow_to_str(lv_flex_flow_t flow) {
    switch(flow) {
        case LV_FLEX_FLOW_ROW: return "Row";
        case LV_FLEX_FLOW_COLUMN: return "Column";
        case LV_FLEX_FLOW_ROW_WRAP: return "Row Wrap";
        case LV_FLEX_FLOW_COLUMN_WRAP: return "Column Wrap";
        case LV_FLEX_FLOW_ROW_REVERSE: return "Row Reverse";
        case LV_FLEX_FLOW_COLUMN_REVERSE: return "Column Reverse";
        case LV_FLEX_FLOW_ROW_WRAP_REVERSE: return "Row Wrap Reverse";
        case LV_FLEX_FLOW_COLUMN_WRAP_REVERSE: return "Column Wrap Reverse";
        default: return "Unknown";
    }
}

static void populate_property_view(InspectorTreeNode* node) {
    lv_table_set_row_count(ctx.prop_table, 0);
    if (!node || !node->ir_node) {
        add_prop("Info", "Select an item from the tree view above.");
        return;
    }

    add_prop("IR Type", "%s", get_ir_node_type_str(node->ir_node->type));
    
    switch (node->ir_node->type) {
        case IR_NODE_OBJECT: {
            IRObject* obj = (IRObject*)node->ir_node;
            add_prop("ID", "%s", obj->registered_id ? obj->registered_id : "N/A");
            add_prop("JSON Type", "%s", obj->json_type);
            add_prop("C Type", "%s", obj->c_type);
            add_prop("LV Ptr", "%p", node->lv_obj_ptr);
            
            bool is_widget = false;
            if(strcmp(obj->c_type, "lv_obj_t*") == 0) {
                is_widget = true;
            } else {
                const WidgetDefinition* wd = api_spec_find_widget(ctx.api_spec, obj->json_type);
                if (wd && wd->inherits && strcmp(wd->inherits, "obj") == 0) {
                    is_widget = true;
                }
            }
            
            if (node->lv_obj_ptr && is_widget) {
                lv_obj_t* lv_obj = (lv_obj_t*)node->lv_obj_ptr;
                if (lv_obj_is_valid(lv_obj)) {
                     add_prop("---", "Live Properties ---");
                     add_prop("X", "%"LV_PRId32, lv_obj_get_x(lv_obj));
                     add_prop("Y", "%"LV_PRId32, lv_obj_get_y(lv_obj));
                     add_prop("Width", "%"LV_PRId32, lv_obj_get_width(lv_obj));
                     add_prop("Height", "%"LV_PRId32, lv_obj_get_height(lv_obj));
                     
                     lv_flex_flow_t flex_flow = lv_obj_get_style_flex_flow(lv_obj, LV_PART_MAIN);
                     add_prop("Flex Flow", flex_flow_to_str(flex_flow));
                     
                     if(lv_obj_get_style_grid_column_dsc_array(lv_obj, 0)) {
                         add_prop("Layout", "Grid");
                     } else if (flex_flow != LV_FLEX_FLOW_ROW) { /* ROW is default */
                         add_prop("Layout", "Flex");
                     } else {
                         add_prop("Layout", "Simple");
                     }
                }
            }
            break;
        }

        case IR_EXPR_FUNCTION_CALL: {
            IRExprFunctionCall* call = (IRExprFunctionCall*)node->ir_node;
            add_prop("Function", "%s", call->func_name);
            add_prop("Return Type", "%s", call->base.c_type);
            add_prop("---", "Arguments ---");
            int i = 0;
            char arg_key[32];
            char arg_val_str[256];
            for (IRExprNode* arg_node = call->args; arg_node; arg_node = arg_node->next) {
                snprintf(arg_key, sizeof(arg_key), "Arg %d", i++);
                sprint_expr_value(arg_node->expr, arg_val_str, sizeof(arg_val_str));
                add_prop(arg_key, "%s", arg_val_str);
            }
            if (i == 0) {
                add_prop("Arguments", "(None)");
            }
            break;
        }

        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)node->ir_node;
            add_prop("Type", "%s", lit->base.c_type);
            if(lit->is_string) add_prop("Value", "\"%s\"", lit->value);
            else add_prop("Value", "%s", lit->value);
            break;
        }

        case IR_EXPR_ENUM: {
            IRExprEnum* en = (IRExprEnum*)node->ir_node;
            add_prop("Type", "%s", en->base.c_type);
            add_prop("Symbol", "%s", en->symbol);
            add_prop("Value", "%ld (0x%lX)", (long)en->value, (long)en->value);
            break;
        }

        case IR_NODE_WARNING: {
             IRWarning* warn = (IRWarning*)node->ir_node;
             add_prop("Message", "%s", warn->message);
             break;
        }

        default:
            add_prop("Info", "No details available for this node type.");
            break;
    }
}

static void prop_table_draw_event_cb(lv_event_t* e) {
    lv_draw_task_t * draw_task = lv_event_get_draw_task(e);
    lv_draw_dsc_base_t * base_dsc = (lv_draw_dsc_base_t *)lv_draw_task_get_draw_dsc(draw_task);
    
    if(base_dsc->part == LV_PART_ITEMS) {
        uint32_t row = base_dsc->id1;
        
        // Make every 2nd row grayish
        if((row > 0) && (row % 2) != 0) {
            lv_draw_fill_dsc_t * fill_draw_dsc = lv_draw_task_get_fill_dsc(draw_task);
            if(fill_draw_dsc) {
                fill_draw_dsc->color = lv_color_mix(lv_palette_main(LV_PALETTE_GREY), fill_draw_dsc->color, LV_OPA_10);
            }
        }
    }
}

// --- Public API Implementation ---

void view_inspector_init(lv_obj_t* parent, IRRoot* ir_root, ApiSpec* api_spec) {
    memset(&ctx, 0, sizeof(InspectorContext));
    ctx.parent_container = parent;
    ctx.api_spec = api_spec; // Store the API spec

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);

    // --- Create Tree View (Top Pane) ---
    ctx.tree_table = lv_table_create(parent);
    lv_obj_set_flex_grow(ctx.tree_table, 1);
    lv_obj_set_width(ctx.tree_table, lv_pct(100));
    lv_table_set_column_count(ctx.tree_table, 1);
    lv_table_set_col_width(ctx.tree_table, 0, lv_pct(100));

    // --- Create Property View (Bottom Pane) ---
    ctx.prop_table = lv_table_create(parent);
    lv_obj_set_height(ctx.prop_table, lv_pct(40));
    lv_obj_set_width(ctx.prop_table, lv_pct(100));
    lv_table_set_column_count(ctx.prop_table, 2);
    
    // Style property table for compact row height.
    static lv_style_t style_prop_table_rows;
    lv_style_init(&style_prop_table_rows);
    lv_style_set_pad_ver(&style_prop_table_rows, PROP_TABLE_ROW_VPAD);
    lv_style_set_pad_hor(&style_prop_table_rows, 5);
    lv_obj_add_style(ctx.prop_table, &style_prop_table_rows, LV_PART_ITEMS);

    lv_obj_update_layout(ctx.prop_table);

    lv_table_set_col_width(ctx.prop_table, 0, 120);
    lv_table_set_col_width(ctx.prop_table, 1, lv_obj_get_content_width(ctx.prop_table) - 120);
    
    // Build the internal tree from the IR
    build_inspector_tree(ir_root);

    // Populate the UI widgets
    rebuild_tree_view();
    populate_property_view(NULL); // Initial state

    // Add event handlers
    lv_obj_add_event_cb(ctx.tree_table, tree_table_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_event_cb(ctx.tree_table, on_tree_selection_changed, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_flag(ctx.tree_table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
    
    lv_obj_add_event_cb(ctx.prop_table, prop_table_draw_event_cb, LV_EVENT_DRAW_TASK_ADDED, NULL);
    lv_obj_add_flag(ctx.prop_table, LV_OBJ_FLAG_SEND_DRAW_TASK_EVENTS);
}

void view_inspector_set_object_pointer(IRNode* ir_node, void* lv_ptr) {
    if (!ir_node || !lv_ptr) return;

    for (int i = 0; i < ctx.node_count; i++) {
        if (ctx.all_nodes[i] && ctx.all_nodes[i]->ir_node == ir_node) {
            ctx.all_nodes[i]->lv_obj_ptr = lv_ptr;
            // If this node is currently selected, refresh its properties
            if (ctx.selected_node == ctx.all_nodes[i]) {
                populate_property_view(ctx.selected_node);
            }
            return;
        }
    }
}

static const char* get_ir_node_type_str(int type) {
    switch(type) {
        case IR_NODE_ROOT: return "Root";
        case IR_NODE_OBJECT: return "Object";
        case IR_NODE_COMPONENT_DEF: return "Component Def";
        case IR_NODE_PROPERTY: return "Property";
        case IR_NODE_WITH_BLOCK: return "With Block";
        case IR_NODE_WARNING: return "Warning";
        case IR_EXPR_LITERAL: return "Literal Expr";
        case IR_EXPR_ENUM: return "Enum Expr";
        case IR_EXPR_FUNCTION_CALL: return "Function Call";
        case IR_EXPR_ARRAY: return "Array Expr";
        case IR_EXPR_REGISTRY_REF: return "Registry Ref";
        case IR_EXPR_CONTEXT_VAR: return "Context Var";
        case IR_EXPR_STATIC_STRING: return "Static String";
        case IR_EXPR_RUNTIME_REG_ADD: return "Runtime Reg Add";
        case IR_EXPR_RAW_POINTER: return "Raw Pointer";
        default: return "Unknown";
    }
}

static void sprint_expr_value(IRExpr* expr, char* buf, size_t buf_size) {
    if (!expr) {
        snprintf(buf, buf_size, "NULL");
        return;
    }
    switch(expr->base.type) {
        case IR_EXPR_LITERAL: {
            IRExprLiteral* lit = (IRExprLiteral*)expr;
            if (lit->is_string) snprintf(buf, buf_size, "\"%s\"", lit->value);
            else snprintf(buf, buf_size, "%s", lit->value);
            break;
        }
        case IR_EXPR_STATIC_STRING:
            snprintf(buf, buf_size, "!\"%s\"", ((IRExprStaticString*)expr)->value);
            break;
        case IR_EXPR_ENUM:
            snprintf(buf, buf_size, "%s", ((IRExprEnum*)expr)->symbol);
            break;
        case IR_EXPR_REGISTRY_REF:
            snprintf(buf, buf_size, "%s", ((IRExprRegistryRef*)expr)->name);
            break;
        case IR_EXPR_FUNCTION_CALL:
            snprintf(buf, buf_size, "%s(...)", ((IRExprFunctionCall*)expr)->func_name);
            break;
        case IR_EXPR_ARRAY:
            snprintf(buf, buf_size, "[Array]");
            break;
        default:
            snprintf(buf, buf_size, "(%s)", get_ir_node_type_str(expr->base.type));
            break;
    }
}

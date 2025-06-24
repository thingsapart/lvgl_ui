#ifndef IR_H
#define IR_H

#include <stdint.h>

// Forward declaration for IRNode to resolve circular dependency for function pointers
struct IRNode;

// --- Function pointer types for polymorphism ---
typedef void (*IRFreeFunc)(struct IRNode* node);
typedef void (*IRCodegenFunc)(struct IRNode* node, int indent_level);

// --- Base IR Node ---
// This will be the first member of all IR expression and statement structs
// to allow safe casting.
typedef struct IRNode {
    enum {
        IR_EXPR_LITERAL,
        IR_EXPR_VARIABLE,
        IR_EXPR_FUNC_CALL,
        IR_EXPR_ARRAY,
        IR_EXPR_ADDRESS_OF,
        IR_STMT_BLOCK,
        IR_STMT_VAR_DECL,
        IR_STMT_FUNC_CALL,
        IR_STMT_COMMENT,
        IR_STMT_WIDGET_ALLOCATE,
        IR_STMT_OBJECT_ALLOCATE,
        // Add other statement types here
    } type;
    IRFreeFunc free;
    IRCodegenFunc codegen;
} IRNode;

// --- Expressions ---

// Base struct for all expressions (all start with IRNode base)
typedef IRNode IRExpr;

// Linked list node for expressions (e.g., function arguments, array elements)
typedef struct IRExprNode {
    IRExpr* expr;
    struct IRExprNode* next;
} IRExprNode;

// Literal (e.g., "hello", 123, true)
typedef struct {
    IRNode base;
    char* value; // Store all literals as strings for now, parse during codegen
                 // For strings, 'value' will include the quotes, e.g., ""actual_string_value""
} IRExprLiteral;

// Variable reference
typedef struct {
    IRNode base;
    char* name;
} IRExprVariable;

// Function call expression
typedef struct {
    IRNode base;
    char* func_name;
    IRExprNode* args; // Linked list of argument expressions
} IRExprFuncCall;

// Array expression (e.g., {1, 2, 3} or new int[]{1, 2, 3})
typedef struct {
    IRNode base;
    IRExprNode* elements; // Linked list of element expressions
} IRExprArray;

// Address-of expression (e.g. &myVar)
typedef struct {
    IRNode base;
    IRExpr* expr; // The expression whose address is taken
} IRExprAddressOf;

// --- Statements ---

// Base struct for all statements (all start with IRNode base)
typedef IRNode IRStmt;

// Linked list node for statements (e.g., in a block)
typedef struct IRStmtNode {
    IRStmt* stmt;
    struct IRStmtNode* next;
} IRStmtNode;

// Block of statements (e.g., { stmt1; stmt2; })
typedef struct {
    IRNode base;
    IRStmtNode* stmts; // Linked list of statements
} IRStmtBlock;

// Variable declaration (e.g., string myVar = "value";)
typedef struct {
    IRNode base;
    char* type_name;
    char* var_name;
    IRExpr* initializer; // Optional initializer expression
} IRStmtVarDecl;

// Function call statement (a function call that doesn't return a value used in an expression)
// e.g. print("hello");
typedef struct {
    IRNode base;
    IRExprFuncCall* call; // Embeds/points to a function call expression structure
} IRStmtFuncCall;

// Comment statement
typedef struct {
    IRNode base;
    char* text; // The comment text (without // or /* */ markers)
} IRStmtComment;

// For declaring and creating a standard widget, e.g., lv_obj_t* btn1 = lv_btn_create(parent);
typedef struct {
    IRStmt base;
    char* c_var_name;           // e.g., "btn1"
    char* widget_c_type_name;   // e.g., "lv_obj_t" (usually this for LVGL post-creation)
    char* create_func_name;     // e.g., "lv_btn_create"
    IRExpr* parent_expr;        // Expression for the parent object (e.g., ir_new_variable("parent_ui_obj")). Can be NULL for screen.
} IRStmtWidgetAllocate;

// For declaring, malloc-ing, and initializing an object, e.g., lv_style_t* style1 = (lv_style_t*)malloc(sizeof(lv_style_t)); if (style1) { lv_style_init(style1); }
typedef struct {
    IRStmt base;
    char* c_var_name;           // e.g., "style1"
    char* object_c_type_name;   // e.g., "lv_style_t" (the actual type, not pointer)
    char* init_func_name;       // e.g., "lv_style_init" (assumes it takes Type*)
} IRStmtObjectAllocate;


// --- Factory functions for Expressions ---
IRExpr* ir_new_literal(const char* value);
IRExpr* ir_new_variable(const char* name);
IRExpr* ir_new_func_call_expr(const char* func_name, IRExprNode* args);
IRExpr* ir_new_array(IRExprNode* elements);
IRExpr* ir_new_address_of(IRExpr* expr);
IRExpr* ir_new_literal_string(const char* value);
// REMOVED: IRExpr* ir_new_cast_expr(const char* target_type_name, IRExpr* expr_to_cast);


// --- Factory functions for Statements ---
IRStmtBlock* ir_new_block();
void ir_block_add_stmt(IRStmtBlock* block, IRStmt* stmt); // Helper to add to a block

IRStmt* ir_new_var_decl(const char* type_name, const char* var_name, IRExpr* initializer);
IRStmt* ir_new_func_call_stmt(const char* func_name, IRExprNode* args);
IRStmt* ir_new_comment(const char* text);
// REMOVED: IRStmt* ir_new_assignment_stmt(IRExpr* lvalue, IRExpr* rvalue);
// REMOVED: IRStmt* ir_new_if_stmt(IRExpr* condition, IRStmtBlock* if_body, IRStmtBlock* else_body);
IRStmt* ir_new_widget_allocate_stmt(const char* c_var_name, const char* widget_c_type_name, const char* create_func_name, IRExpr* parent_expr);
IRStmt* ir_new_object_allocate_stmt(const char* c_var_name, const char* object_c_type_name, const char* init_func_name);

// --- Factory functions for Linked List Nodes ---
IRExprNode* ir_new_expr_node(IRExpr* expr);
void ir_expr_list_add(IRExprNode** head, IRExpr* expr); // Helper to add to an expr list


// --- Memory Management ---
void ir_free(IRNode* node); // Master free function, dispatches based on type

// --- Node Value Accessors ---
const char* ir_node_get_string(IRNode* node);
intptr_t ir_node_get_int(IRNode* node);
intptr_t ir_node_get_int_robust(IRNode* node, const char* enum_type_name);

#endif // IR_H

#ifndef IR_H
#define IR_H

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
        // Add other expression types here
        IR_STMT_BLOCK,
        IR_STMT_VAR_DECL,
        IR_STMT_FUNC_CALL, // For function calls as statements
        IR_STMT_COMMENT,
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


// --- Factory functions for Expressions ---
IRExpr* ir_new_literal(const char* value);
IRExpr* ir_new_variable(const char* name);
IRExpr* ir_new_func_call_expr(const char* func_name, IRExprNode* args);
IRExpr* ir_new_array(IRExprNode* elements);
IRExpr* ir_new_address_of(IRExpr* expr);
IRExpr* ir_new_literal_string(const char* value);


// --- Factory functions for Statements ---
IRStmtBlock* ir_new_block();
void ir_block_add_stmt(IRStmtBlock* block, IRStmt* stmt); // Helper to add to a block

IRStmt* ir_new_var_decl(const char* type_name, const char* var_name, IRExpr* initializer);
IRStmt* ir_new_func_call_stmt(const char* func_name, IRExprNode* args);
IRStmt* ir_new_comment(const char* text);

// --- Factory functions for Linked List Nodes ---
IRExprNode* ir_new_expr_node(IRExpr* expr);
void ir_expr_list_add(IRExprNode** head, IRExpr* expr); // Helper to add to an expr list


// --- Memory Management ---
void ir_free(IRNode* node); // Master free function, dispatches based on type

#endif // IR_H

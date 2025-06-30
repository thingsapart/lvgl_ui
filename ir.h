#ifndef IR_H
#define IR_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h> // For size_t

// Forward declarations
struct IRNode;
struct IRExpr;
struct IRProperty;
struct IRObject;
struct IRWithBlock;
struct IRComponent;

// --- Base IR Node ---
typedef struct IRNode {
    enum {
        // High-level nodes
        IR_NODE_ROOT,
        IR_NODE_OBJECT, // Represents a widget or a style object
        IR_NODE_COMPONENT_DEF,

        // Property-related nodes
        IR_NODE_PROPERTY, // Now primarily for key-value pairs like 'use-view' context
        IR_NODE_WITH_BLOCK,

        // Expression nodes
        IR_EXPR_LITERAL,
        IR_EXPR_ENUM,
        IR_EXPR_FUNCTION_CALL,
        IR_EXPR_ARRAY,
        IR_EXPR_REGISTRY_REF, // @name
        IR_EXPR_CONTEXT_VAR,  // $name
        IR_EXPR_STATIC_STRING, // !string
        IR_EXPR_RUNTIME_REG_ADD,
        IR_EXPR_RAW_POINTER   // NEW: For renderer's internal use
    } type;
} IRNode;


// --- Expressions ---

// Base struct for all expressions
typedef struct IRExpr {
    IRNode base;
    char* c_type; // The C type of the expression's result.
} IRExpr;


// Linked list node for expressions (e.g., function arguments, array elements)
typedef struct IRExprNode {
    IRExpr* expr;
    struct IRExprNode* next;
} IRExprNode;

// Generic literal (number, bool, NULL, string)
typedef struct {
    IRExpr base;
    char* value;
    size_t len;     // Length of the value if it's a string, can contain nulls
    bool is_string;
} IRExprLiteral;

// Static string: a heap-allocated, persistent string.
typedef struct {
    IRExpr base;
    char* value;
    size_t len;     // Length of the string, can contain nulls
} IRExprStaticString;

// Enum literal (e.g., LV_ALIGN_CENTER)
typedef struct {
    IRExpr base;
    char* symbol;
    intptr_t value;
} IRExprEnum;

// Function call expression
typedef struct {
    IRExpr base;
    char* func_name;
    IRExprNode* args; // Linked list of argument expressions
} IRExprFunctionCall;

// Array expression
typedef struct {
    IRExpr base;
    IRExprNode* elements; // Linked list of element expressions
} IRExprArray;

// Reference to a registered object: @name
typedef struct {
    IRExpr base;
    char* name;
} IRExprRegistryRef;

// Reference to a context variable: $name
typedef struct {
    IRExpr base;
    char* name;
} IRExprContextVar;

// Represents a call to the runtime object registry
typedef struct {
    IRExpr base;
    char* id; // The string ID for registration
    IRExpr* object_expr; // Expression for the object pointer (usually a RegistryRef)
} IRExprRuntimeRegAdd;

// NEW: Represents a raw, unevaluated pointer value.
// This is used internally by the renderer to pass intermediate results
// from nested function calls to the dispatcher.
typedef struct {
    IRExpr base;
    void* ptr;
} IRExprRawPointer;


// --- High-Level UI Constructs ---

// A property on an object (e.g., width: 100)
typedef struct IRProperty {
    IRNode base;
    char* name;
    IRExpr* value;
    struct IRProperty* next;
} IRProperty;

// A 'with-do' block
typedef struct IRWithBlock {
    IRNode base;
    IRExpr* target_expr;
    IRExprNode* setup_calls; // List of function calls within the block
    struct IRObject* children_root; // For `with { do: { children: [...] } }`
    struct IRWithBlock* next;
} IRWithBlock;

// A node in the ordered list of operations for an IRObject
typedef struct IROperationNode {
    IRNode* op_node; // Can be an IRObject (for a child) or an IRExpr (for a setup call)
    struct IROperationNode* next;
} IROperationNode;

// Represents a widget, style, or other UI object from the spec
typedef struct IRObject {
    IRNode base;
    char* c_name;           // Generated C variable name for this object.
    char* json_type;        // The "type" from the UI spec ("button", "label", "style", "use-view").
    char* c_type;           // The C type of this object (e.g. "lv_obj_t*", "lv_style_t*")
    char* registered_id;    // If "named" or "id" is present, this is the string key.
    IRExpr* constructor_expr;
    IROperationNode* operations;
    char* use_view_component_id;
    IRProperty* use_view_context;
    IRWithBlock* with_blocks;
    struct IRObject* next;
} IRObject;

// A component definition
typedef struct IRComponent {
    IRNode base;
    char* id;
    IRObject* root_widget;
    struct IRComponent* next;
} IRComponent;

// The top-level container for the entire UI spec IR
typedef struct {
    IRNode base;
    IRComponent* components;
    IRObject* root_objects;
} IRRoot;


// --- Factory functions for Expressions ---
IRExpr* ir_new_expr_literal(const char* value, const char* c_type);
IRExpr* ir_new_expr_literal_string(const char* value, size_t len);
IRExpr* ir_new_expr_static_string(const char* value, size_t len);
IRExpr* ir_new_expr_enum(const char* symbol, intptr_t val, const char* enum_c_type);
IRExpr* ir_new_expr_func_call(const char* func_name, IRExprNode* args, const char* return_c_type);
IRExpr* ir_new_expr_array(IRExprNode* elements, const char* array_c_type);
IRExpr* ir_new_expr_registry_ref(const char* name, const char* c_type);
IRExpr* ir_new_expr_context_var(const char* name, const char* c_type);
IRExpr* ir_new_expr_runtime_reg_add(const char* id, IRExpr* object_expr);
IRExpr* ir_new_expr_raw_pointer(void* ptr, const char* c_type);

// --- Factory functions for High-Level Constructs ---
IRRoot* ir_new_root();
IRObject* ir_new_object(const char* c_name, const char* json_type, const char* c_type, const char* registered_id);
IRComponent* ir_new_component_def(const char* id, IRObject* root_widget);
IRProperty* ir_new_property(const char* name, IRExpr* value);
IRWithBlock* ir_new_with_block(IRExpr* target, IRExprNode* calls, IRObject* children);

// --- List management helpers ---
void ir_expr_list_add(IRExprNode** head, IRExpr* expr);
void ir_object_list_add(IRObject** head, IRObject* object);
void ir_property_list_add(IRProperty** head, IRProperty* prop);
void ir_with_block_list_add(IRWithBlock** head, IRWithBlock* block);
void ir_component_def_list_add(IRComponent** head, IRComponent* comp);
void ir_operation_list_add(IROperationNode** head, IRNode* node);

// --- Memory Management ---
void ir_free(IRNode* node);

// --- Node Value Accessors ---
const char* ir_node_get_string(IRNode* node);
intptr_t ir_node_get_int(IRNode* node);
bool ir_node_get_bool(IRNode* node);

#endif // IR_H

#ifndef IR_H
#define IR_H

#include <stdint.h>
#include <stdbool.h>

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
        IR_NODE_PROPERTY,
        IR_NODE_WITH_BLOCK,

        // Expression nodes
        IR_EXPR_LITERAL,
        IR_EXPR_ENUM,
        IR_EXPR_FUNCTION_CALL,
        IR_EXPR_ARRAY,
        IR_EXPR_REGISTRY_REF, // @name
        IR_EXPR_CONTEXT_VAR,  // $name
        IR_EXPR_STATIC_STRING // !string
    } type;
} IRNode;


// --- Expressions ---

// Base struct for all expressions
typedef IRNode IRExpr;

// Linked list node for expressions (e.g., function arguments, array elements)
typedef struct IRExprNode {
    IRExpr* expr;
    struct IRExprNode* next;
} IRExprNode;

// Generic literal (number, bool, NULL, string)
typedef struct {
    IRNode base;
    char* value;
    bool is_string;
} IRExprLiteral;

// Static string: a heap-allocated, persistent string.
typedef struct {
    IRNode base;
    char* value;
} IRExprStaticString;

// Enum literal (e.g., LV_ALIGN_CENTER)
typedef struct {
    IRNode base;
    char* symbol;
    intptr_t value;
} IRExprEnum;

// Function call expression
typedef struct {
    IRNode base;
    char* func_name;
    IRExprNode* args; // Linked list of argument expressions
} IRExprFunctionCall;

// Array expression
typedef struct {
    IRNode base;
    IRExprNode* elements; // Linked list of element expressions
} IRExprArray;

// Reference to a registered object: @name
typedef struct {
    IRNode base;
    char* name;
} IRExprRegistryRef;

// Reference to a context variable: $name
typedef struct {
    IRNode base;
    char* name;
} IRExprContextVar;


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
    IRProperty* properties;
    struct IRObject* children_root; // For `with { do: { children: [...] } }`
    struct IRWithBlock* next;
} IRWithBlock;

// Represents a widget, style, or other UI object from the spec
typedef struct IRObject {
    IRNode base;
    char* c_name;           // Generated C variable name for this object.
    char* json_type;        // The "type" from the UI spec ("button", "label", "style", "use-view").
    char* registered_id;    // If "named" or "id" is present, this is the string key.

    // Fields specific to 'use-view' type
    char* use_view_component_id; // from "id" field of a use-view
    IRProperty* use_view_context; // from "context" field of a use-view

    IRProperty* properties;   // All other properties, including overrides for use-view
    IRWithBlock* with_blocks;
    struct IRObject* children; // Linked list of child objects
    struct IRObject* next;     // Sibling in the list
} IRObject;

// A component definition
typedef struct IRComponent {
    IRNode base;
    char* id; // The component's name (e.g., "@my_component")
    IRObject* root_widget;
    struct IRComponent* next;
} IRComponent;

// The top-level container for the entire UI spec IR
typedef struct {
    IRNode base;
    IRComponent* components;
    IRObject* root_objects; // Top-level widgets/styles
} IRRoot;


// --- Factory functions for Expressions ---
IRExpr* ir_new_expr_literal(const char* value);
IRExpr* ir_new_expr_literal_string(const char* value);
IRExpr* ir_new_expr_static_string(const char* value);
IRExpr* ir_new_expr_enum(const char* symbol, intptr_t val);
IRExpr* ir_new_expr_func_call(const char* func_name, IRExprNode* args);
IRExpr* ir_new_expr_array(IRExprNode* elements);
IRExpr* ir_new_expr_registry_ref(const char* name);
IRExpr* ir_new_expr_context_var(const char* name);

// --- Factory functions for High-Level Constructs ---
IRRoot* ir_new_root();
IRObject* ir_new_object(const char* c_name, const char* json_type, const char* registered_id);
IRComponent* ir_new_component_def(const char* id, IRObject* root_widget);
IRProperty* ir_new_property(const char* name, IRExpr* value);
IRWithBlock* ir_new_with_block(IRExpr* target, IRProperty* props, IRObject* children);

// --- List management helpers ---
void ir_expr_list_add(IRExprNode** head, IRExpr* expr);
void ir_object_list_add(IRObject** head, IRObject* object);
void ir_property_list_add(IRProperty** head, IRProperty* prop);
void ir_with_block_list_add(IRWithBlock** head, IRWithBlock* block);
void ir_component_def_list_add(IRComponent** head, IRComponent* comp);

// --- Memory Management ---
void ir_free(IRNode* node);

// --- Node Value Accessors (for dynamic dispatcher) ---
// These functions are designed to be called on "simple" IR expression nodes
// that have been resolved by the renderer (Literal, Enum, RegistryRef, StaticString).
const char* ir_node_get_string(IRNode* node);
intptr_t ir_node_get_int(IRNode* node);
bool ir_node_get_bool(IRNode* node);

#endif // IR_H

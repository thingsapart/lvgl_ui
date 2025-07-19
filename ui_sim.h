#ifndef UI_SIM_H
#define UI_SIM_H

#include <cJSON.h>
#include "data_binding.h"
#include "lvgl.h"

// --- Public API ---

/**
 * @brief Initializes or re-initializes the UI Simulator system.
 * Frees all previously allocated resources, making it safe for live-reloading.
 */
void ui_sim_init(void);

/**
 * @brief Parses a 'data-binding' cJSON node from the UI specification.
 * This builds the internal state model, action handlers, and update rules for the simulator.
 * This should be called by the generator before the UI is rendered.
 * If a previous definition existed, it is freed and replaced.
 *
 * @param node The cJSON object with "type": "data-binding".
 * @return true on successful parsing, false on failure.
 */
bool ui_sim_process_node(cJSON* node);

/**
 * @brief Prepares the UI Simulator to run.
 * This function should be called after the UI has been fully rendered.
 * It registers the simulator's action handler and notifies the UI of all initial state
 * values. IT DOES NOT start the periodic updates.
 */
void ui_sim_start(void);

/**
 * @brief Stops the UI Simulator and cleans up resources.
 * This is automatically called by ui_sim_init().
 */
void ui_sim_stop(void);

/**
 * @brief Advances the simulation by one tick.
 * This should be called periodically by an external loop (e.g., the main application
 * loop or a test runner).
 * @param dt The delta-time in seconds since the last tick (e.g., 0.033 for 30fps).
 */
void ui_sim_tick(float dt);


// --- Internal Data Structures (exposed for potential debugging) ---

#define UI_SIM_MAX_STATES 128
#define UI_SIM_MAX_ACTIONS 128
#define UI_SIM_MAX_FUNC_ARGS 8
#define UI_SIM_MAX_SCHEDULED_ACTIONS 64

// --- Expressions ---

typedef struct SimExpressionNode SimExpressionNode;

typedef struct SimExpression {
    enum {
        SIM_EXPR_LITERAL,      // A literal value (float, bool, string)
        SIM_EXPR_STATE_REF,    // A reference to another state variable (e.g., "spindle_on" or "!spindle_on")
        SIM_EXPR_ACTION_VALUE, // The incoming 'value' from a UI action (e.g., "value.bool")
        SIM_EXPR_FUNCTION      // A function call (e.g., [add, 1, 2])
    } type;

    union {
        binding_value_t literal;
        struct {
            char* state_name;
            bool is_negated; // For the "!" shorthand
        } state_ref;
        binding_value_type_t action_value_type;
        struct {
            char* func_name;
            SimExpressionNode* args_head;
        } function;
    } as;
} SimExpression;

struct SimExpressionNode {
    SimExpression* expr;
    SimExpressionNode* next;
};


// --- Modifications ---

typedef struct SimModification {
    enum {
        MOD_SET,
        MOD_INC,
        MOD_DEC,
        MOD_TOGGLE,
        MOD_CYCLE,
        MOD_RANGE
    } type;

    char* target_state_name;
    SimExpression* value_expr;     // Expression for the new value (used by set, inc, dec)
    SimExpression* condition_expr; // Optional 'when' condition
    struct SimModification* next;
} SimModification;


// --- Actions ---

typedef struct {
    char* name;
    SimModification* modifications_head;
} SimAction;

// --- Scheduled Actions ---
typedef struct {
    uint32_t tick;
    char* name;
    binding_value_t value;
} SimScheduledAction;


// --- State ---

typedef struct {
    char* name;
    binding_value_t value;
    bool is_derived;
    SimExpression* derived_expr;

    // Runtime state
    bool is_dirty;
    uint32_t cycle_index;
} SimStateVariable;


#endif // UI_SIM_H

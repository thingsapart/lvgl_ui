#ifndef UI_SIM_H
#define UI_SIM_H

#include <stdbool.h>
#include <stdint.h>
#include "cJSON.h"
#include "data_binding.h"

#define UI_SIM_MAX_STATES 64
#define UI_SIM_MAX_ACTIONS 128
#define UI_SIM_MAX_SCHEDULED_ACTIONS 64
#define UI_SIM_MAX_FUNC_ARGS 16

// --- Global Configuration ---
extern bool g_ui_sim_trace_no_time_enabled;

// --- Data Structures ---

typedef enum {
    SIM_EXPR_LITERAL,
    SIM_EXPR_STATE_REF,
    SIM_EXPR_FUNCTION,
    SIM_EXPR_ACTION_VALUE,
} SimExpressionType;

typedef struct SimExpression {
    SimExpressionType type;
    union {
        binding_value_t literal;
        struct {
            char* state_name;
            bool is_negated;
        } state_ref;
        struct {
            char* func_name;
            struct SimExpressionNode* args_head;
        } function;
        binding_value_type_t action_value_type;
    } as;
} SimExpression;

typedef struct SimExpressionNode {
    SimExpression* expr;
    struct SimExpressionNode* next;
} SimExpressionNode;

typedef enum {
    MOD_SET,
    MOD_INC,
    MOD_DEC,
    MOD_TOGGLE,
    MOD_CYCLE,
    MOD_RANGE,
} SimModificationType;

typedef struct SimModification {
    SimModificationType type;
    char* target_state_name;
    SimExpression* value_expr;
    SimExpression* condition_expr;
    struct SimModification* next;
} SimModification;

typedef struct {
    char* name;
    binding_value_t value;
    bool is_dirty;
    bool is_derived;
    SimExpression* derived_expr;
} SimStateVariable;

typedef struct {
    char* name;
    SimModification* modifications_head;
} SimAction;

typedef struct {
    uint32_t tick;
    char* name;
    binding_value_t value;
} SimScheduledAction;


// --- Public API ---

/**
 * @brief Initializes or re-initializes the UI simulator, clearing all existing state.
 */
void ui_sim_init(void);

/**
 * @brief Parses a `data-binding` cJSON node and configures the simulator.
 * This is the main entry point for defining the simulation's behavior from a UI spec.
 * @param node The cJSON object representing the `data-binding` block.
 * @return true on successful parsing, false on error.
 */
bool ui_sim_process_node(cJSON* node);

/**
 * @brief Starts the simulator. It registers its action handler and performs the initial
 * state notifications. Does nothing if already active or if no definition has been processed.
 */
void ui_sim_start(void);

/**
 * @brief Stops the simulator. It will no longer process ticks or actions.
 */
void ui_sim_stop(void);

/**
 * @brief Advances the simulator by one time step.
 * This function executes any scheduled actions for the current tick, runs the `updates` logic,
 * increments the internal `time` state, and notifies the UI of any changes.
 * @param dt The delta time for this tick, used to increment the internal `time` state.
 */
void ui_sim_tick(float dt);

#endif // UI_SIM_H

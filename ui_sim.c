#include "ui_sim.h"
#include "debug_log.h"
#include "utils.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
#include <stdarg.h>

// --- Global Configuration for Testing ---
extern bool g_ui_sim_trace_enabled; // This will be defined in main.c

// --- Parsing Context for Better Error Reporting ---
typedef struct {
    const char* current_block; // "state", "actions", "updates"
    const char* current_key;   // e.g., "counter", "program_run"
} SimParseContext;


// --- Global Context for the UI Simulator System ---
typedef struct {
    bool is_active;
    bool has_definition;

    SimStateVariable states[UI_SIM_MAX_STATES];
    uint32_t state_count;

    SimAction actions[UI_SIM_MAX_ACTIONS];
    uint32_t action_count;

    SimModification* updates_head;
} SimContext;

static SimContext g_sim;

// --- Forward Declarations: Parsers ---
static void free_expression(SimExpression* expr);
static void free_modification_list(SimModification* head);
static SimExpression* parse_expression(cJSON* json, SimParseContext* ctx);
static SimModification* parse_modification(const char* state_name, cJSON* mod_json, SimParseContext* ctx);
static SimModification* parse_modification_block(cJSON* obj_json, SimParseContext* ctx);
static bool parse_state(cJSON* state_array, SimParseContext* ctx);
static bool parse_actions(cJSON* action_array, SimParseContext* ctx);
static bool parse_updates(cJSON* update_array, SimParseContext* ctx);

// --- Forward Declarations: Runtime ---
static void sim_action_handler(const char* action_name, binding_value_t value, void* user_data);
static void execute_modifications_list(SimModification* head, binding_value_t action_value);
static binding_value_t evaluate_expression(SimExpression* expr, binding_value_t action_value);
static void notify_changed_states(void);
static SimStateVariable* find_state(const char* name);
static bool set_state_value(SimStateVariable* state, binding_value_t new_value);
static bool values_are_equal(binding_value_t v1, binding_value_t v2);
static void trace_print_value(binding_value_t v);


// --- Error Handling Helper ---
static void sim_abort(SimParseContext* ctx, const char *format, ...) {
    char message_buffer[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message_buffer, sizeof(message_buffer), format, args);
    va_end(args);

    char context_buffer[256] = "";
    if (ctx) {
        snprintf(context_buffer, sizeof(context_buffer), "\n> In block: %s\n> On key:   %s\n\n",
            ctx->current_block ? ctx->current_block : "N/A",
            ctx->current_key ? ctx->current_key : "N/A");
    }

    char final_buffer[1024];
    snprintf(final_buffer, sizeof(final_buffer), "UI-Sim Error%s%s", context_buffer, message_buffer);

    render_abort(final_buffer);
}


// --- Public API ---

void ui_sim_init(void) {
    ui_sim_stop();

    for (uint32_t i = 0; i < g_sim.state_count; i++) {
        free(g_sim.states[i].name);
        if (g_sim.states[i].value.type == BINDING_TYPE_STRING) free((void*)g_sim.states[i].value.as.s_val);
        free_expression(g_sim.states[i].derived_expr);
    }
    for (uint32_t i = 0; i < g_sim.action_count; i++) {
        free(g_sim.actions[i].name);
        free_modification_list(g_sim.actions[i].modifications_head);
    }
    free_modification_list(g_sim.updates_head);

    memset(&g_sim, 0, sizeof(SimContext));
    DEBUG_LOG(LOG_MODULE_DATABINDING, "UI Simulator initialized.");
}

bool ui_sim_process_node(cJSON* node) {
    if (!node || !cJSON_IsObject(node)) {
        print_warning("UI-Sim: 'data-binding' block is not a valid object.");
        return false;
    }

    ui_sim_init();
    SimParseContext ctx = {0};

    ctx.current_block = "state";
    cJSON* state_json = cJSON_GetObjectItem(node, "state");
    if (state_json && !parse_state(state_json, &ctx)) return false;

    ctx.current_block = "actions";
    cJSON* actions_json = cJSON_GetObjectItem(node, "actions");
    if (actions_json && !parse_actions(actions_json, &ctx)) return false;

    ctx.current_block = "updates";
    cJSON* updates_json = cJSON_GetObjectItem(node, "updates");
    if (updates_json && !parse_updates(updates_json, &ctx)) return false;

    g_sim.has_definition = true;
    DEBUG_LOG(LOG_MODULE_DATABINDING, "Successfully processed UI-Sim definition.");
    return true;
}

void ui_sim_start(void) {
    if (!g_sim.has_definition || g_sim.is_active) {
        return;
    }

    DEBUG_LOG(LOG_MODULE_DATABINDING, "Starting UI Simulator...");
    data_binding_register_action_handler(sim_action_handler, NULL);

    if (g_ui_sim_trace_enabled) {
        for (uint32_t i = 0; i < g_sim.state_count; i++) {
            printf("STATE_SET: %s = ", g_sim.states[i].name);
            trace_print_value(g_sim.states[i].value);
            printf(" (old: null)\n");
        }
    }

    for (uint32_t i = 0; i < g_sim.state_count; i++) {
        g_sim.states[i].is_dirty = true;
    }
    notify_changed_states();

    g_sim.is_active = true;
}

void ui_sim_stop(void) {
    g_sim.is_active = false;
    DEBUG_LOG(LOG_MODULE_DATABINDING, "UI Simulator stopped.");
}

void ui_sim_tick(float dt) {
    if (!g_sim.is_active) return;

    SimStateVariable* time_state = find_state("time");
    if (time_state) {
        binding_value_t old_val = time_state->value;
        binding_value_t new_val = {.type=BINDING_TYPE_FLOAT, .as.f_val = old_val.as.f_val + dt};
        set_state_value(time_state, new_val);
    }

    execute_modifications_list(g_sim.updates_head, (binding_value_t){.type = BINDING_TYPE_NULL});
    notify_changed_states();
}


// --- Memory Management Helpers ---

static void free_expression_list(SimExpressionNode* head) {
    while(head) {
        SimExpressionNode* next = head->next;
        free_expression(head->expr);
        free(head);
        head = next;
    }
}

static void free_expression(SimExpression* expr) {
    if (!expr) return;
    switch(expr->type) {
        case SIM_EXPR_LITERAL:
            if (expr->as.literal.type == BINDING_TYPE_STRING) free((void*)expr->as.literal.as.s_val);
            break;
        case SIM_EXPR_STATE_REF:
            free(expr->as.state_ref.state_name);
            break;
        case SIM_EXPR_FUNCTION:
            free(expr->as.function.func_name);
            free_expression_list(expr->as.function.args_head);
            break;
        case SIM_EXPR_ACTION_VALUE:
             break;
    }
    free(expr);
}

static void free_modification_list(SimModification* head) {
    while(head) {
        SimModification* next = head->next;
        free(head->target_state_name);
        free_expression(head->value_expr);
        free_expression(head->condition_expr);
        free(head);
        head = next;
    }
}

// --- Parsing Logic ---

static bool parse_state(cJSON* state_array, SimParseContext* ctx) {
    cJSON* item;
    cJSON_ArrayForEach(item, state_array) {
        if (g_sim.state_count >= UI_SIM_MAX_STATES) {
            sim_abort(ctx, "Exceeded maximum number of states (%d).", UI_SIM_MAX_STATES);
            return false;
        }
        if (!cJSON_IsObject(item) || !item->child) {
            sim_abort(ctx, "Invalid 'state' entry. Each entry must be an object with one key, e.g., '- my_var: 0.0'.");
            return false;
        }

        cJSON* state_def = item->child;
        ctx->current_key = state_def->string;
        SimStateVariable* state = &g_sim.states[g_sim.state_count];
        state->name = strdup(state_def->string);
        if (!state->name) { sim_abort(ctx, "Out of memory"); return false; }

        if (cJSON_IsArray(state_def)) {
            if (cJSON_GetArraySize(state_def) != 2) {
                 sim_abort(ctx, "Invalid state array format. Must be [type, initial_value].");
                 return false;
            }
            cJSON* type_json = cJSON_GetArrayItem(state_def, 0);
            cJSON* val_json = cJSON_GetArrayItem(state_def, 1);
            const char* type_str = cJSON_GetStringValue(type_json);
            if (!type_str) { sim_abort(ctx, "State type must be a string (e.g., \"float\")."); return false; }

            if (strcmp(type_str, "float") == 0) state->value.type = BINDING_TYPE_FLOAT;
            else if (strcmp(type_str, "bool") == 0) state->value.type = BINDING_TYPE_BOOL;
            else if (strcmp(type_str, "string") == 0) state->value.type = BINDING_TYPE_STRING;
            else { sim_abort(ctx, "Unknown state type '%s'. Use 'float', 'bool', or 'string'.", type_str); return false; }

            if (state->value.type == BINDING_TYPE_STRING) state->value.as.s_val = strdup(cJSON_GetStringValue(val_json));
            else if (state->value.type == BINDING_TYPE_BOOL) state->value.as.b_val = cJSON_IsTrue(val_json);
            else state->value.as.f_val = (float)val_json->valuedouble;
        } else if (cJSON_IsString(state_def)) {
            const char* type_str = state_def->valuestring;
            if (strcmp(type_str, "float") == 0) { state->value = (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = 0.0f}; }
            else if (strcmp(type_str, "bool") == 0) { state->value = (binding_value_t){.type = BINDING_TYPE_BOOL, .as.b_val = false}; }
            else if (strcmp(type_str, "string") == 0) { state->value = (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = strdup("")}; } // *** FIX: Default to empty string ***
            else { sim_abort(ctx, "Unknown state type '%s'. Use 'float', 'bool', or 'string'.", type_str); return false; }
        } else if (cJSON_IsNumber(state_def)) {
            state->value = (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = (float)state_def->valuedouble};
        } else if (cJSON_IsBool(state_def)) {
            state->value = (binding_value_t){.type = BINDING_TYPE_BOOL, .as.b_val = cJSON_IsTrue(state_def)};
        } else if (cJSON_IsObject(state_def) && cJSON_HasObjectItem(state_def, "derived_expr")) {
            state->is_derived = true;
            state->derived_expr = parse_expression(cJSON_GetObjectItem(state_def, "derived_expr"), ctx);
        } else {
             sim_abort(ctx, "Invalid format for state variable. Must be a type, a value, or [type, value].");
             return false;
        }
        g_sim.state_count++;
    }
    ctx->current_key = NULL;
    return true;
}

static bool parse_actions(cJSON* action_array, SimParseContext* ctx) {
    cJSON* item;
    cJSON_ArrayForEach(item, action_array) {
        if (g_sim.action_count >= UI_SIM_MAX_ACTIONS) {
            sim_abort(ctx, "Exceeded maximum number of actions (%d).", UI_SIM_MAX_ACTIONS);
            return false;
        }
        if (!cJSON_IsObject(item) || !item->child) {
            sim_abort(ctx, "Invalid 'actions' entry. Each entry must be an object with one key, e.g., '- my_action: { ... }'.");
            return false;
        }
        cJSON* action_def = item->child;
        ctx->current_key = action_def->string;
        SimAction* action = &g_sim.actions[g_sim.action_count];
        action->name = strdup(action_def->string);
        if (!action->name) { sim_abort(ctx, "Out of memory"); return false; }
        action->modifications_head = parse_modification_block(action_def, ctx);
        if (!action->modifications_head) return false;
        g_sim.action_count++;
    }
    ctx->current_key = NULL;
    return true;
}

static bool parse_updates(cJSON* update_array, SimParseContext* ctx) {
    cJSON* item;
    cJSON_ArrayForEach(item, update_array) {
        if (!cJSON_IsObject(item)) {
            sim_abort(ctx, "Invalid 'updates' entry. Each entry must be an object describing modifications.");
            return false;
        }
        SimModification* new_mods = parse_modification_block(item, ctx);
        if (!new_mods) return false;
        if (!g_sim.updates_head) {
            g_sim.updates_head = new_mods;
        } else {
            SimModification* tail = g_sim.updates_head;
            while(tail->next) tail = tail->next;
            tail->next = new_mods;
        }
    }
    ctx->current_key = NULL;
    return true;
}

static SimModification* parse_modification_block(cJSON* obj_json, SimParseContext* ctx) {
    SimModification* head = NULL;
    SimModification* tail = NULL;

    cJSON* item;
    cJSON_ArrayForEach(item, obj_json) {
        const char* key = item->string;
        ctx->current_key = key;

        SimModification* new_mod = parse_modification(key, item, ctx);
        if (!new_mod) {
            free_modification_list(head);
            return NULL;
        }

        if (!head) {
            head = tail = new_mod;
        } else {
            tail->next = new_mod;
            tail = new_mod;
        }
        while(tail->next) tail = tail->next;
    }
    return head;
}


static SimModification* parse_modification(const char* state_name, cJSON* mod_json, SimParseContext* ctx) {
    SimModification* mod = calloc(1, sizeof(SimModification));
    if (!mod) { sim_abort(ctx, "Out of memory"); return NULL; }
    mod->target_state_name = strdup(state_name);
    if (!mod->target_state_name) { sim_abort(ctx, "Out of memory"); free(mod); return NULL; }

    if (cJSON_IsObject(mod_json) && mod_json->child) {
        cJSON* when_json = cJSON_GetObjectItem(mod_json, "when");
        if(when_json) mod->condition_expr = parse_expression(when_json, ctx);

        cJSON* then_json = cJSON_GetObjectItem(mod_json, "then");
        if(then_json) {
            if(!when_json) {
                sim_abort(ctx, "'then' clause can only be used inside a 'when' clause.");
                free_modification_list(mod);
                return NULL;
            }
             mod->next = parse_modification_block(then_json, ctx);
        }

        const char* mod_type_str = mod_json->child->string;
        if(strcmp(mod_type_str, "set") == 0) mod->type = MOD_SET;
        else if(strcmp(mod_type_str, "inc") == 0) mod->type = MOD_INC;
        else if(strcmp(mod_type_str, "dec") == 0) mod->type = MOD_DEC;
        else if(strcmp(mod_type_str, "toggle") == 0) mod->type = MOD_TOGGLE;
        else if(strcmp(mod_type_str, "cycle") == 0) mod->type = MOD_CYCLE;
        else if(strcmp(mod_type_str, "range") == 0) mod->type = MOD_RANGE;
        else {
            mod->type = MOD_SET;
            mod->value_expr = parse_expression(mod_json, ctx);
            return mod;
        }
        mod->value_expr = parse_expression(mod_json->child, ctx);
    } else {
        mod->type = MOD_SET;
        mod->value_expr = parse_expression(mod_json, ctx);
    }
    return mod;
}

static SimExpression* parse_expression(cJSON* json, SimParseContext* ctx) {
    if (!json) return NULL;
    SimExpression* expr = calloc(1, sizeof(SimExpression));
    if (!expr) { sim_abort(ctx, "Out of memory"); return NULL; }

    if (cJSON_IsNumber(json)) {
        expr->type = SIM_EXPR_LITERAL;
        expr->as.literal = (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = (float)json->valuedouble};
    } else if (cJSON_IsBool(json)) {
        expr->type = SIM_EXPR_LITERAL;
        expr->as.literal = (binding_value_t){.type = BINDING_TYPE_BOOL, .as.b_val = cJSON_IsTrue(json)};
    } else if (cJSON_IsString(json)) {
        const char* s = json->valuestring;
        if (strcmp(s, "value.float") == 0) {
            expr->type = SIM_EXPR_ACTION_VALUE;
            expr->as.action_value_type = BINDING_TYPE_FLOAT;
        } else if (strcmp(s, "value.bool") == 0) {
            expr->type = SIM_EXPR_ACTION_VALUE;
            expr->as.action_value_type = BINDING_TYPE_BOOL;
        } else if (strcmp(s, "value.string") == 0) {
             expr->type = SIM_EXPR_ACTION_VALUE;
            expr->as.action_value_type = BINDING_TYPE_STRING;
        } else {
            bool is_negated = (s[0] == '!');
            const char* state_name_ref = is_negated ? s + 1 : s;
            if (find_state(state_name_ref)) {
                expr->type = SIM_EXPR_STATE_REF;
                expr->as.state_ref.state_name = strdup(state_name_ref);
                expr->as.state_ref.is_negated = is_negated;
            } else {
                expr->type = SIM_EXPR_LITERAL;
                expr->as.literal = (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = strdup(s)};
            }
        }
    } else if (cJSON_IsArray(json)) {
        expr->type = SIM_EXPR_FUNCTION;
        cJSON* func_name_json = cJSON_GetArrayItem(json, 0);
        if (!cJSON_IsString(func_name_json)) {
            expr->as.function.func_name = strdup("pair");
            SimExpressionNode* arg_tail = NULL;
            cJSON* element;
            cJSON_ArrayForEach(element, json) {
                SimExpressionNode* new_arg_node = calloc(1, sizeof(SimExpressionNode));
                new_arg_node->expr = parse_expression(element, ctx);
                 if (!expr->as.function.args_head) {
                    expr->as.function.args_head = arg_tail = new_arg_node;
                } else {
                    arg_tail->next = new_arg_node;
                    arg_tail = new_arg_node;
                }
            }
            return expr;
        }
        expr->as.function.func_name = strdup(cJSON_GetStringValue(func_name_json));

        SimExpressionNode* arg_tail = NULL;
        for (int i = 1; i < cJSON_GetArraySize(json); i++) {
            SimExpressionNode* new_arg_node = calloc(1, sizeof(SimExpressionNode));
            new_arg_node->expr = parse_expression(cJSON_GetArrayItem(json, i), ctx);
            if (!expr->as.function.args_head) {
                expr->as.function.args_head = arg_tail = new_arg_node;
            } else {
                arg_tail->next = new_arg_node;
                arg_tail = new_arg_node;
            }
        }
    } else if (cJSON_IsObject(json) && cJSON_HasObjectItem(json, "case")) {
        expr->type = SIM_EXPR_FUNCTION;
        expr->as.function.func_name = strdup("case");
        cJSON* case_array = cJSON_GetObjectItem(json, "case");
        if (!cJSON_IsArray(case_array)) {
            sim_abort(ctx, "Value for 'case' must be an array of [condition, value] pairs.");
            free(expr); return NULL;
        }

        SimExpressionNode* arg_tail = NULL;
        cJSON* pair_json;
        cJSON_ArrayForEach(pair_json, case_array) {
             SimExpressionNode* new_arg_node = calloc(1, sizeof(SimExpressionNode));
             new_arg_node->expr = parse_expression(pair_json, ctx);
             if (!expr->as.function.args_head) {
                expr->as.function.args_head = arg_tail = new_arg_node;
            } else {
                arg_tail->next = new_arg_node;
                arg_tail = new_arg_node;
            }
        }
    } else {
        free(expr);
        sim_abort(ctx, "Invalid expression format. Must be a literal (e.g. 1.0, true, \"text\"), a state name (e.g. 'my_state'), or a function (e.g. [add, 1, 2]).");
        return NULL;
    }
    return expr;
}

// --- Runtime Logic ---

static SimAction* find_action(const char* name) {
    for(uint32_t i=0; i < g_sim.action_count; i++) {
        if(strcmp(g_sim.actions[i].name, name) == 0) return &g_sim.actions[i];
    }
    return NULL;
}

static SimStateVariable* find_state(const char* name) {
    for(uint32_t i=0; i < g_sim.state_count; i++) {
        if(strcmp(g_sim.states[i].name, name) == 0) return &g_sim.states[i];
    }
    return NULL;
}

static bool set_state_value(SimStateVariable* state, binding_value_t new_value) {
    if (!values_are_equal(state->value, new_value)) {
        if (g_ui_sim_trace_enabled) {
            printf("STATE_SET: %s = ", state->name);
            trace_print_value(new_value);
            printf(" (old: ");
            trace_print_value(state->value);
            printf(")\n");
        }
        if(state->value.type == BINDING_TYPE_STRING) free((void*)state->value.as.s_val);

        state->value = new_value;
        state->is_dirty = true;
        return true;
    } else {
        if (new_value.type == BINDING_TYPE_STRING) {
            free((void*)new_value.as.s_val);
        }
    }
    return false;
}

static void notify_changed_states(void) {
    bool derived_changed;
    do {
        derived_changed = false;
        for(uint32_t i = 0; i < g_sim.state_count; i++) {
            if (g_sim.states[i].is_derived) {
                binding_value_t derived_val = evaluate_expression(g_sim.states[i].derived_expr, (binding_value_t){.type=BINDING_TYPE_NULL});
                if(set_state_value(&g_sim.states[i], derived_val)) {
                    derived_changed = true;
                }
            }
        }
    } while(derived_changed);

    for(uint32_t i = 0; i < g_sim.state_count; i++) {
        if (g_sim.states[i].is_dirty) {
            if (g_ui_sim_trace_enabled) {
                printf("NOTIFY: %s = ", g_sim.states[i].name);
                trace_print_value(g_sim.states[i].value);
                printf("\n");
            }
            data_binding_notify_state_changed(g_sim.states[i].name, g_sim.states[i].value);
            g_sim.states[i].is_dirty = false;
        }
    }
}

static void sim_action_handler(const char* action_name, binding_value_t value, void* user_data) {
    (void)user_data;
    if (g_ui_sim_trace_enabled) {
        printf("ACTION: %s value=", action_name);
        trace_print_value(value);
        printf("\n");
    }
    SimAction* action = find_action(action_name);
    if (action) {
        execute_modifications_list(action->modifications_head, value);
        notify_changed_states();
    } else {
        print_warning("UI-Sim: Received unhandled action '%s'.", action_name);
    }
}

static void execute_modifications_list(SimModification* head, binding_value_t action_value) {
    for (SimModification* mod = head; mod; mod = mod->next) {
        bool condition_met = true;
        if (mod->condition_expr) {
            binding_value_t cond_val = evaluate_expression(mod->condition_expr, action_value);
            if (cond_val.type == BINDING_TYPE_BOOL) {
                condition_met = cond_val.as.b_val;
            } else {
                print_warning("UI-Sim: 'when' condition for state '%s' did not evaluate to a boolean.", mod->target_state_name);
                condition_met = false;
            }
        }

        if (condition_met) {
            SimStateVariable* target_state = find_state(mod->target_state_name);
            if (!target_state) {
                print_warning("UI-Sim: Attempted to modify unknown state '%s'.", mod->target_state_name);
                continue;
            }

            switch (mod->type) {
                case MOD_SET: {
                    binding_value_t val = evaluate_expression(mod->value_expr, action_value);
                    set_state_value(target_state, val);
                    break;
                }
                case MOD_INC: {
                    binding_value_t val = evaluate_expression(mod->value_expr, action_value);
                    if (target_state->value.type == BINDING_TYPE_FLOAT && val.type == BINDING_TYPE_FLOAT) {
                        binding_value_t new_val = {.type=BINDING_TYPE_FLOAT, .as.f_val=target_state->value.as.f_val + val.as.f_val};
                        set_state_value(target_state, new_val);
                    }
                    if(val.type == BINDING_TYPE_STRING) free((void*)val.as.s_val);
                    break;
                }
                case MOD_DEC: {
                     binding_value_t val = evaluate_expression(mod->value_expr, action_value);
                    if (target_state->value.type == BINDING_TYPE_FLOAT && val.type == BINDING_TYPE_FLOAT) {
                        binding_value_t new_val = {.type=BINDING_TYPE_FLOAT, .as.f_val=target_state->value.as.f_val - val.as.f_val};
                        set_state_value(target_state, new_val);
                    }
                    if(val.type == BINDING_TYPE_STRING) free((void*)val.as.s_val);
                    break;
                }
                case MOD_TOGGLE: {
                    if (target_state->value.type == BINDING_TYPE_BOOL) {
                        binding_value_t new_val = {.type=BINDING_TYPE_BOOL, .as.b_val=!target_state->value.as.b_val};
                        set_state_value(target_state, new_val);
                    } else {
                        print_warning("UI-Sim: 'toggle' can only be used on boolean states. State '%s' is not a boolean.", target_state->name);
                    }
                    break;
                }
                case MOD_CYCLE: {
                    if (mod->value_expr->type == SIM_EXPR_FUNCTION && strcmp(mod->value_expr->as.function.func_name, "cycle") == 0 && mod->value_expr->as.function.args_head) {
                        uint32_t count = 0;
                        for(SimExpressionNode* n = mod->value_expr->as.function.args_head; n; n=n->next) count++;
                        if (count > 0) {
                            target_state->cycle_index = (target_state->cycle_index + 1) % count;
                            SimExpressionNode* node_to_eval = mod->value_expr->as.function.args_head;
                            for(uint32_t i=0; i < target_state->cycle_index; i++) node_to_eval = node_to_eval->next;
                            binding_value_t val = evaluate_expression(node_to_eval->expr, action_value);
                            set_state_value(target_state, val);
                        }
                    }
                    break;
                }
                 case MOD_RANGE: {
                    if (mod->value_expr->type == SIM_EXPR_FUNCTION && strcmp(mod->value_expr->as.function.func_name, "range") == 0 && mod->value_expr->as.function.args_head) {
                        SimExpressionNode* n = mod->value_expr->as.function.args_head;
                        binding_value_t min_v = evaluate_expression(n->expr, action_value);
                        binding_value_t max_v = evaluate_expression(n->next->expr, action_value);
                        binding_value_t step_v = evaluate_expression(n->next->next->expr, action_value);

                        if(target_state->value.type == BINDING_TYPE_FLOAT) {
                            float current = target_state->value.as.f_val;
                            current += step_v.as.f_val;
                            if (step_v.as.f_val > 0 && current > max_v.as.f_val) current = min_v.as.f_val;
                            else if (step_v.as.f_val < 0 && current < min_v.as.f_val) current = max_v.as.f_val;
                            set_state_value(target_state, (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val=current});
                        } else {
                             print_warning("UI-Sim: 'range' can only be used on float states. State '%s' is not a float.", target_state->name);
                        }
                        if(min_v.type == BINDING_TYPE_STRING) free((void*)min_v.as.s_val);
                        if(max_v.type == BINDING_TYPE_STRING) free((void*)max_v.as.s_val);
                        if(step_v.type == BINDING_TYPE_STRING) free((void*)step_v.as.s_val);
                    }
                    break;
                }
            }
        }
    }
}

static binding_value_t evaluate_expression(SimExpression* expr, binding_value_t action_value) {
    if (!expr) return (binding_value_t){.type = BINDING_TYPE_NULL};

    switch(expr->type) {
        case SIM_EXPR_LITERAL:
            if (expr->as.literal.type == BINDING_TYPE_STRING) {
                return (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = strdup(expr->as.literal.as.s_val)};
            }
            return expr->as.literal;
        case SIM_EXPR_ACTION_VALUE:
            if(action_value.type == expr->as.action_value_type) return action_value;
            print_hint("UI-Sim Hint: Action payload 'value' was requested as the wrong type.");
            return (binding_value_t){.type = BINDING_TYPE_NULL};
        case SIM_EXPR_STATE_REF: {
            SimStateVariable* state = find_state(expr->as.state_ref.state_name);
            if (state) {
                if(expr->as.state_ref.is_negated && state->value.type == BINDING_TYPE_BOOL) {
                    return (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=!state->value.as.b_val};
                }
                if (state->value.type == BINDING_TYPE_STRING) {
                    return (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = strdup(state->value.as.s_val)};
                }
                return state->value;
            }
            print_warning("UI-Sim: Expression referenced unknown state '%s'.", expr->as.state_ref.state_name);
            return (binding_value_t){.type = BINDING_TYPE_NULL};
        }
        case SIM_EXPR_FUNCTION: {
            SimExpression* arg_exprs[UI_SIM_MAX_FUNC_ARGS];
            binding_value_t arg_values[UI_SIM_MAX_FUNC_ARGS];
            int argc = 0;
            for(SimExpressionNode* n = expr->as.function.args_head; n && argc < UI_SIM_MAX_FUNC_ARGS; n=n->next, argc++) {
                arg_exprs[argc] = n->expr;
                arg_values[argc] = evaluate_expression(n->expr, action_value);
            }

            #define IS_FLOAT(v) ((v).type == BINDING_TYPE_FLOAT)
            #define IS_BOOL(v) ((v).type == BINDING_TYPE_BOOL)

            binding_value_t ret = {.type = BINDING_TYPE_NULL};
            const char* name = expr->as.function.func_name;

            if(strcmp(name, "add") == 0 && argc==2 && IS_FLOAT(arg_values[0]) && IS_FLOAT(arg_values[1])) ret = (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val=arg_values[0].as.f_val + arg_values[1].as.f_val};
            else if(strcmp(name, "sub") == 0 && argc==2 && IS_FLOAT(arg_values[0]) && IS_FLOAT(arg_values[1])) ret = (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val=arg_values[0].as.f_val - arg_values[1].as.f_val};
            else if(strcmp(name, "mul") == 0 && argc==2 && IS_FLOAT(arg_values[0]) && IS_FLOAT(arg_values[1])) ret = (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val=arg_values[0].as.f_val * arg_values[1].as.f_val};
            else if(strcmp(name, "div") == 0 && argc==2 && IS_FLOAT(arg_values[0]) && IS_FLOAT(arg_values[1])) ret = (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val=arg_values[1].as.f_val==0.0f? 0.0f : arg_values[0].as.f_val / arg_values[1].as.f_val};
            else if(strcmp(name, "sin") == 0 && argc==1 && IS_FLOAT(arg_values[0])) ret = (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val=sinf(arg_values[0].as.f_val)};
            else if(strcmp(name, "cos") == 0 && argc==1 && IS_FLOAT(arg_values[0])) ret = (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val=cosf(arg_values[0].as.f_val)};
            else if(strcmp(name, "clamp") == 0 && argc==3 && IS_FLOAT(arg_values[0]) && IS_FLOAT(arg_values[1]) && IS_FLOAT(arg_values[2])) { float v=arg_values[0].as.f_val; float min=arg_values[1].as.f_val; float max=arg_values[2].as.f_val; ret = (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val=fmaxf(min, fminf(v, max))};}
            else if(strcmp(name, "==") == 0 && argc==2) ret = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=values_are_equal(arg_values[0], arg_values[1])};
            else if(strcmp(name, "!=") == 0 && argc==2) ret = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=!values_are_equal(arg_values[0], arg_values[1])};
            else if(strcmp(name, ">") == 0 && argc==2 && IS_FLOAT(arg_values[0]) && IS_FLOAT(arg_values[1])) ret = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=arg_values[0].as.f_val > arg_values[1].as.f_val};
            else if(strcmp(name, "<") == 0 && argc==2 && IS_FLOAT(arg_values[0]) && IS_FLOAT(arg_values[1])) ret = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=arg_values[0].as.f_val < arg_values[1].as.f_val};
            else if(strcmp(name, ">=") == 0 && argc==2 && IS_FLOAT(arg_values[0]) && IS_FLOAT(arg_values[1])) ret = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=arg_values[0].as.f_val >= arg_values[1].as.f_val};
            else if(strcmp(name, "<=") == 0 && argc==2 && IS_FLOAT(arg_values[0]) && IS_FLOAT(arg_values[1])) ret = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=arg_values[0].as.f_val <= arg_values[1].as.f_val};
            else if(strcmp(name, "and") == 0 && argc > 0) { bool r = true; for(int i=0; i<argc; i++) { if(!IS_BOOL(arg_values[i]) || !arg_values[i].as.b_val) { r=false; break; } } ret = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=r}; }
            else if(strcmp(name, "or") == 0 && argc > 0) { bool r = false; for(int i=0; i<argc; i++) { if(IS_BOOL(arg_values[i]) && arg_values[i].as.b_val) { r=true; break; } } ret = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=r}; }
            else if(strcmp(name, "not") == 0 && argc == 1 && IS_BOOL(arg_values[0])) ret = (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val=!arg_values[0].as.b_val};
            else if(strcmp(name, "case") == 0 && argc > 0) {
                 for(int i=0; i<argc; i++) {
                     SimExpression* pair_expr = arg_exprs[i];
                     if(pair_expr->type == SIM_EXPR_FUNCTION && strcmp(pair_expr->as.function.func_name, "pair") == 0 && pair_expr->as.function.args_head) {
                        SimExpression* cond_expr = pair_expr->as.function.args_head->expr;
                        binding_value_t cond_val = evaluate_expression(cond_expr, action_value);

                        if (IS_BOOL(cond_val) && cond_val.as.b_val) {
                             SimExpression* val_expr = pair_expr->as.function.args_head->next->expr;
                             ret = evaluate_expression(val_expr, action_value);
                             if (cond_val.type == BINDING_TYPE_STRING) free((void*)cond_val.as.s_val);
                             break;
                        }
                        if (cond_val.type == BINDING_TYPE_STRING) free((void*)cond_val.as.s_val);
                     }
                 }
            }

            for(int i=0; i < argc; i++) {
                 if (arg_values[i].type == BINDING_TYPE_STRING) free((void*)arg_values[i].as.s_val);
            }
            return ret;
        }
    }
    return (binding_value_t){.type=BINDING_TYPE_NULL};
}

// --- Utility Functions ---

static bool values_are_equal(binding_value_t v1, binding_value_t v2) {
    if (v1.type != v2.type) return false;
    switch(v1.type) {
        case BINDING_TYPE_NULL: return true;
        case BINDING_TYPE_BOOL: return v1.as.b_val == v2.as.b_val;
        case BINDING_TYPE_FLOAT: return fabsf(v1.as.f_val - v2.as.f_val) < 1e-6;
        case BINDING_TYPE_STRING:
            if (v1.as.s_val == NULL || v2.as.s_val == NULL) return v1.as.s_val == v2.as.s_val;
            return strcmp(v1.as.s_val, v2.as.s_val) == 0;
    }
    return false;
}

static void trace_print_value(binding_value_t v) {
    switch(v.type) {
        case BINDING_TYPE_NULL: printf("null"); break;
        case BINDING_TYPE_FLOAT: printf("%.3f", v.as.f_val); break;
        case BINDING_TYPE_BOOL: printf("%s", v.as.b_val ? "true" : "false"); break;
        case BINDING_TYPE_STRING: printf("\"%s\"", v.as.s_val ? v.as.s_val : ""); break;
    }
}

#include "cnc_app.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// The global instance of our CNC machine's state
static CNC_State g_cnc_state;

// Helper to get the string representation of the current program status
const char* get_status_string() {
    if (g_cnc_state.program_running) {
        return "RUNNING";
    }
    if (g_cnc_state.spindle_on) {
        return "SPINDLE ON";
    }
    return "IDLE";
}

void cnc_app_notify_all(void) {
    data_binding_notify_state_changed("position|x", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.x_pos});
    data_binding_notify_state_changed("position|y", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.y_pos});
    data_binding_notify_state_changed("position|z", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.z_pos});
    data_binding_notify_state_changed("spindle|is_on", (binding_value_t){.type = BINDING_TYPE_BOOL, .as.b_val = g_cnc_state.spindle_on});
    data_binding_notify_state_changed("feedrate|override", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.feed_override});
    data_binding_notify_state_changed("program|status", (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = get_status_string()});
    data_binding_notify_state_changed("spindle|rpm", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.spindle_rpm});
    data_binding_notify_state_changed("jog|step", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.jog_step});
}

void cnc_app_init(void) {
    // Initialize state
    g_cnc_state.x_pos = 0.0f;
    g_cnc_state.y_pos = 0.0f;
    g_cnc_state.z_pos = 25.0f;
    g_cnc_state.spindle_rpm = 0.0f;
    g_cnc_state.spindle_on = false;
    g_cnc_state.program_running = false;
    g_cnc_state.feed_override = 100.0f;
    g_cnc_state.jog_step = 1.0f;
    g_cnc_state.sim_angle = 0.0f;
    g_cnc_state.sim_radius = 0.0f;

    // Register our action handler with the data binding system
    data_binding_register_action_handler(cnc_action_handler, NULL);

    // Notify initial state to the UI
    cnc_app_notify_all();
}

void cnc_action_handler(const char* action_name, binding_value_t value, void* user_data) {
    printf("CNC ACTION: name='%s' | value_type=%d\n", action_name, value.type);
    bool position_changed = false;
    bool program_state_changed = false;
    bool spindle_state_changed = false;

    if (strcmp(action_name, "program|run") == 0) {
        if (!g_cnc_state.program_running) {
            g_cnc_state.program_running = true;
            g_cnc_state.spindle_on = true;
            program_state_changed = true;
            spindle_state_changed = true;
        }
    } else if (strcmp(action_name, "program|pause") == 0) {
        if (g_cnc_state.program_running) {
            g_cnc_state.program_running = false;
            program_state_changed = true;
        }
    } else if (strcmp(action_name, "program|stop") == 0) {
        g_cnc_state.program_running = false;
        g_cnc_state.spindle_on = false;
        g_cnc_state.sim_angle = 0.0f;
        g_cnc_state.sim_radius = 0.0f;
        position_changed = true;
        program_state_changed = true;
        spindle_state_changed = true;
    } else if (strcmp(action_name, "spindle|toggle") == 0) {
        g_cnc_state.spindle_on = value.as.b_val;
        spindle_state_changed = true;
        program_state_changed = true; // Status string depends on spindle state
    } else if (strcmp(action_name, "feedrate|override") == 0) {
        g_cnc_state.feed_override = value.as.f_val;
        data_binding_notify_state_changed("feedrate|override", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.feed_override});
    } else if (strcmp(action_name, "position|home") == 0) {
        g_cnc_state.program_running = false;
        g_cnc_state.spindle_on = false;
        g_cnc_state.x_pos = 0.0f;
        g_cnc_state.y_pos = 0.0f;
        g_cnc_state.z_pos = 25.0f;
        g_cnc_state.sim_angle = 0.0f;
        g_cnc_state.sim_radius = 0.0f;
        position_changed = true;
        program_state_changed = true;
        spindle_state_changed = true;
    } else if (strcmp(action_name, "jog|set_step") == 0) {
        g_cnc_state.jog_step = value.as.f_val;
        data_binding_notify_state_changed("jog|step", (binding_value_t){.type=BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.jog_step});
    } else if (strcmp(action_name, "jog|move|x_minus") == 0) {
        if (g_cnc_state.program_running) return;
        g_cnc_state.x_pos -= g_cnc_state.jog_step;
        position_changed = true;
    } else if (strcmp(action_name, "jog|move|x_plus") == 0) {
        if (g_cnc_state.program_running) return;
        g_cnc_state.x_pos += g_cnc_state.jog_step;
        position_changed = true;
    } else if (strcmp(action_name, "jog|move|y_minus") == 0) {
        if (g_cnc_state.program_running) return;
        g_cnc_state.y_pos -= g_cnc_state.jog_step;
        position_changed = true;
    } else if (strcmp(action_name, "jog|move|y_plus") == 0) {
        if (g_cnc_state.program_running) return;
        g_cnc_state.y_pos += g_cnc_state.jog_step;
        position_changed = true;
    } else if (strcmp(action_name, "jog|move|z_minus") == 0) {
        if (g_cnc_state.program_running) return;
        g_cnc_state.z_pos -= g_cnc_state.jog_step;
        position_changed = true;
    } else if (strcmp(action_name, "jog|move|z_plus") == 0) {
        if (g_cnc_state.program_running) return;
        g_cnc_state.z_pos += g_cnc_state.jog_step;
        position_changed = true;
    }

    if (position_changed) {
        data_binding_notify_state_changed("position|x", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.x_pos});
        data_binding_notify_state_changed("position|y", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.y_pos});
        data_binding_notify_state_changed("position|z", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.z_pos});
    }
    if (program_state_changed) {
        data_binding_notify_state_changed("program|status", (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = get_status_string()});
    }
    if (spindle_state_changed) {
        data_binding_notify_state_changed("spindle|is_on", (binding_value_t){.type = BINDING_TYPE_BOOL, .as.b_val = g_cnc_state.spindle_on});
    }
}

void cnc_app_tick(void) {
    static int tick_count = 0;
    bool needs_notify = false;

    if (g_cnc_state.program_running) {
        float speed_multiplier = g_cnc_state.feed_override / 100.0f;
        g_cnc_state.sim_angle += 0.05f * speed_multiplier;
        if (g_cnc_state.sim_radius < 50.0f) {
             g_cnc_state.sim_radius += 0.01f * speed_multiplier;
        }
        g_cnc_state.x_pos = g_cnc_state.sim_radius * cosf(g_cnc_state.sim_angle);
        g_cnc_state.y_pos = g_cnc_state.sim_radius * sinf(g_cnc_state.sim_angle);
        needs_notify = true;
    }

    float old_rpm = g_cnc_state.spindle_rpm;
    if (g_cnc_state.spindle_on) {
        g_cnc_state.spindle_rpm = 8000.0f + 200.0f * sinf(tick_count * 0.1f);
    } else {
        g_cnc_state.spindle_rpm = 0.0f;
    }
    if (old_rpm != g_cnc_state.spindle_rpm) needs_notify = true;


    if (needs_notify || (tick_count % 20 == 0)) { // Update less frequently if idle
        data_binding_notify_state_changed("position|x", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.x_pos});
        data_binding_notify_state_changed("position|y", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.y_pos});
        data_binding_notify_state_changed("spindle|rpm", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.spindle_rpm});
        data_binding_notify_state_changed("program|status", (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = get_status_string()});
    }

    tick_count++;
}

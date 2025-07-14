#include "cnc_app.h"
#include <stdio.h>
#include <string.h>
#include <math.h>

// The global instance of our CNC machine's state
static CNC_State g_cnc_state;

void cnc_app_init(void) {
    // Initialize state
    g_cnc_state.x_pos = 0.0f;
    g_cnc_state.y_pos = 0.0f;
    g_cnc_state.z_pos = 25.0f;
    g_cnc_state.spindle_rpm = 0.0f;
    g_cnc_state.spindle_on = false;
    g_cnc_state.program_running = false;
    g_cnc_state.feed_override = 100;
    g_cnc_state.jog_step = 1.0f;
    g_cnc_state.sim_angle = 0.0f;
    g_cnc_state.sim_radius = 0.0f;

    // Register our action handler with the data binding system
    data_binding_register_action_handler(cnc_action_handler);

    // Notify initial state to the UI
    data_binding_notify_state_changed("position|x", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.x_pos});
    data_binding_notify_state_changed("position|y", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.y_pos});
    data_binding_notify_state_changed("position|z", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.z_pos});
    data_binding_notify_state_changed("spindle|is_on", (binding_value_t){.type = BINDING_TYPE_BOOL, .as.b_val = g_cnc_state.spindle_on});
    data_binding_notify_state_changed("feedrate|override", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.feed_override});
    data_binding_notify_state_changed("program|status", (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = "IDLE"});
    data_binding_notify_state_changed("spindle|rpm", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.spindle_rpm});
    data_binding_notify_state_changed("jog|step", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.jog_step});
}

void cnc_action_handler(const char* action_name, binding_value_t value) {
    printf("CNC ACTION: name='%s' | value_type=%d\n", action_name, value.type);
    bool position_changed = false;
    bool program_state_changed = false;

    if (strcmp(action_name, "program|run") == 0) {
        if (!g_cnc_state.program_running) {
            g_cnc_state.program_running = true;
            program_state_changed = true;
        }
    } else if (strcmp(action_name, "program|pause") == 0) {
        if (g_cnc_state.program_running) {
            g_cnc_state.program_running = false;
            program_state_changed = true;
        }
    } else if (strcmp(action_name, "program|stop") == 0) {
        g_cnc_state.program_running = false;
        // Reset simulation state as well
        //g_cnc_state.x_pos = 0.0f;
        //g_cnc_state.y_pos = 0.0f;
        //g_cnc_state.z_pos = 25.0f;
        g_cnc_state.sim_angle = 0.0f;
        g_cnc_state.sim_radius = 0.0f;
        position_changed = true;
        program_state_changed = true;
    } else if (strcmp(action_name, "spindle|toggle") == 0) {
        g_cnc_state.spindle_on = value.as.b_val;
        // The widget state is bound, but we can notify the general status
        program_state_changed = true;
    } else if (strcmp(action_name, "feedrate|override") == 0) {
        g_cnc_state.feed_override = value.as.f_val;
        // The button label is bound and will update automatically
    } else if (strcmp(action_name, "position|home") == 0) {
        g_cnc_state.program_running = false; // Stop program on home
        g_cnc_state.x_pos = 0.0f;
        g_cnc_state.y_pos = 0.0f;
        g_cnc_state.z_pos = 25.0f;
        g_cnc_state.sim_angle = 0.0f;
        g_cnc_state.sim_radius = 0.0f;
        position_changed = true;
        program_state_changed = true;
    } else if (strcmp(action_name, "jog|set_step") == 0) {
        g_cnc_state.jog_step = value.as.f_val;
        data_binding_notify_state_changed("jog|step", value);
    } else if (strcmp(action_name, "jog|move|x_minus") == 0) {
        if (g_cnc_state.program_running) return; // Can't jog while a program is running
        g_cnc_state.x_pos -= g_cnc_state.jog_step;
        position_changed = true;
    } else if (strcmp(action_name, "jog|move|x_plus") == 0) {
        if (g_cnc_state.program_running) return; // Can't jog while a program is running
        g_cnc_state.x_pos += g_cnc_state.jog_step;
        position_changed = true;
    } else if (strcmp(action_name, "jog|move|y_minus") == 0) {
        if (g_cnc_state.program_running) return; // Can't jog while a program is running
        g_cnc_state.y_pos -= g_cnc_state.jog_step;
        position_changed = true;
    } else if (strcmp(action_name, "jog|move|y_plus") == 0) {
        if (g_cnc_state.program_running) return; // Can't jog while a program is running
        g_cnc_state.y_pos += g_cnc_state.jog_step;
        position_changed = true;
    } else if (strcmp(action_name, "jog|move|z_minus") == 0) {
        if (g_cnc_state.program_running) return; // Can't jog while a program is running
        g_cnc_state.z_pos += g_cnc_state.jog_step;
        position_changed = true;
    } else if (strcmp(action_name, "jog|move|z_plus") == 0) {
        if (g_cnc_state.program_running) return; // Can't jog while a program is running
        g_cnc_state.z_pos += g_cnc_state.jog_step;
        position_changed = true;
    }

    // After an action, notify relevant states immediately for responsiveness
    if (position_changed) {
        data_binding_notify_state_changed("position|x", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.x_pos});
        data_binding_notify_state_changed("position|y", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.y_pos});
        data_binding_notify_state_changed("position|z", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.z_pos});
    }
    if (program_state_changed) {
        const char* status_str = g_cnc_state.program_running ? "RUNNING" : (g_cnc_state.spindle_on ? "SPINDLE ON" : "IDLE");
        data_binding_notify_state_changed("program|status", (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = status_str});
    }
}

void cnc_app_tick(void) {
    if (g_cnc_state.program_running) {
        // Calculate speed based on feed override
        float speed_multiplier = g_cnc_state.feed_override / 100.0f;

        // Update simulation state for a spiral path
        g_cnc_state.sim_angle += 0.05f * speed_multiplier;
        g_cnc_state.sim_radius += 0.01f * speed_multiplier;
        if (g_cnc_state.sim_radius > 50.0f) {
            g_cnc_state.sim_radius = 0.0f; // Reset spiral
        }

        // Update machine position
        g_cnc_state.x_pos = g_cnc_state.sim_radius * cosf(g_cnc_state.sim_angle);
        g_cnc_state.y_pos = g_cnc_state.sim_radius * sinf(g_cnc_state.sim_angle);
    }

    // Update spindle RPM based on whether it's on and program is running
    if (g_cnc_state.spindle_on && g_cnc_state.program_running) {
        // Simulate a slightly varying RPM
        g_cnc_state.spindle_rpm = 8000.0f + 200.0f * sinf(g_cnc_state.sim_angle * 5.0f);
    } else {
        g_cnc_state.spindle_rpm = 0.0f;
    }

    // Notify the UI about changes in observed state
    data_binding_notify_state_changed("position|x", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.x_pos});
    data_binding_notify_state_changed("position|y", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.y_pos});
    data_binding_notify_state_changed("position|z", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.z_pos});
    data_binding_notify_state_changed("spindle|rpm", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.spindle_rpm});
    
    // Also notify the general status
    const char* status_str = g_cnc_state.program_running ? "RUNNING" : (g_cnc_state.spindle_on ? "SPINDLE ON" : "IDLE");
    data_binding_notify_state_changed("program|status", (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = status_str});
}

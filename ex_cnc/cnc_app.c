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
    g_cnc_state.sim_angle = 0.0f;
    g_cnc_state.sim_radius = 0.0f;

    // Register our action handler with the data binding system
    data_binding_register_action_handler(cnc_action_handler);

    // Notify initial state to the UI
    data_binding_notify_state_changed("position::absolute::x", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.x_pos});
    data_binding_notify_state_changed("position::absolute::y", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.y_pos});
    data_binding_notify_state_changed("spindle_on", (binding_value_t){.type = BINDING_TYPE_BOOL, .as.b_val = g_cnc_state.spindle_on});
    data_binding_notify_state_changed("feed_override", (binding_value_t){.type = BINDING_TYPE_INT, .as.i_val = g_cnc_state.feed_override});
    data_binding_notify_state_changed("program_running", (binding_value_t){.type = BINDING_TYPE_BOOL, .as.b_val = g_cnc_state.program_running});
}

void cnc_action_handler(const char* action_name, binding_value_t value) {
    printf("CNC ACTION: name='%s' | value_type=%d\n", action_name, value.type);

    if (strcmp(action_name, "program::run_toggle") == 0) {
        g_cnc_state.program_running = !g_cnc_state.program_running;
        printf("  -> Program running set to: %s\n", g_cnc_state.program_running ? "ON" : "OFF");
        data_binding_notify_state_changed("program_running", (binding_value_t){.type=BINDING_TYPE_BOOL, .as.b_val = g_cnc_state.program_running});
    }
    else if (strcmp(action_name, "spindle_enable") == 0) {
        g_cnc_state.spindle_on = value.as.b_val;
        printf("  -> Spindle set to: %s\n", g_cnc_state.spindle_on ? "ON" : "OFF");
        data_binding_notify_state_changed("spindle_on", value);
    }
    else if (strcmp(action_name, "feed_override") == 0) {
        g_cnc_state.feed_override = value.as.i_val;
        printf("  -> Feed override set to: %d%%\n", g_cnc_state.feed_override);
        data_binding_notify_state_changed("feed_override", value);
    }
    else if (strcmp(action_name, "position::home") == 0) {
        printf("  -> Homing axes.\n");
        g_cnc_state.x_pos = 0.0f;
        g_cnc_state.y_pos = 0.0f;
        g_cnc_state.z_pos = 25.0f;
        g_cnc_state.sim_angle = 0.0f;
        g_cnc_state.sim_radius = 0.0f;
        // Notifications will be sent by the next tick
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
        g_cnc_state.spindle_rpm = 8000.0f + 200.0f * sinf(g_cnc_state.sim_angle * 0.5f);
    } else {
        g_cnc_state.spindle_rpm = 0.0f;
    }

    // Notify the UI about changes in observed state
    data_binding_notify_state_changed("position::absolute::x", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.x_pos});
    data_binding_notify_state_changed("position::absolute::y", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.y_pos});
    data_binding_notify_state_changed("spindle::rpm", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.spindle_rpm});
}

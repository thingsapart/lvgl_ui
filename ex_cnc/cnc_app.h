#ifndef CNC_APP_H
#define CNC_APP_H

#include "data_binding.h"
#include <stdbool.h>

// Represents the internal state of our CNC machine
typedef struct {
    float x_pos;
    float y_pos;
    float z_pos;
    float spindle_rpm;
    bool spindle_on;
    bool program_running;
    int feed_override; // Percentage
    float jog_step;    // The distance for manual jog moves

    // Internal state for simulation
    float sim_angle;
    float sim_radius;

} CNC_State;

// Initializes the CNC application state and registers its action handler.
void cnc_app_init(void);

// The action handler that receives commands from the UI.
void cnc_action_handler(const char* action_name, binding_value_t value);

// Simulates one tick of the CNC machine; call this from a timer.
void cnc_app_tick(void);

void cnc_app_notify_all(void);

#endif // CNC_APP_H

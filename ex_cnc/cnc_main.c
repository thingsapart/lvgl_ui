#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <cJSON.h>

#include "utils.h"
#include "api_spec.h"
#include "generator.h"
#include "ir.h"
#include "debug_log.h"
#include "viewer/sdl_viewer.h"
#include "data_binding.h"
#include "lvgl_renderer.h"
#include "registry.h"
#include "yaml_parser.h"

#include <SDL2/SDL.h>

// --- Global Configuration ---
bool g_strict_mode = false;
bool g_strict_registry_mode = false;


// --- CNC Machine Simulation ---

typedef enum {
    STATUS_IDLE,
    STATUS_RUNNING,
    STATUS_PAUSED
} ProgramStatus;

typedef struct {
    float x, y, z;
    ProgramStatus status;
    bool spindle_on;
    int feed_override_pct;
} CNCState;

// The global instance of our application's state model
static CNCState g_cnc_state;

/**
 * @brief The single handler for all actions coming from the UI.
 */
static void cnc_action_handler(const char* action_name, binding_value_t value) {
    printf("ACTION: name='%s', type=%d, ", action_name, value.type);
    switch(value.type) {
        case BINDING_TYPE_INT: printf("value=%d\n", (int)value.as.i_val); break;
        case BINDING_TYPE_BOOL: printf("value=%s\n", value.as.b_val ? "true" : "false"); break;
        default: printf("value=(N/A)\n"); break;
    }

    if (strcmp(action_name, "program|run") == 0) {
        g_cnc_state.status = STATUS_RUNNING;
        data_binding_notify_state_changed("program|status", (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = "RUNNING"});
    } else if (strcmp(action_name, "program|pause") == 0) {
        g_cnc_state.status = STATUS_PAUSED;
        data_binding_notify_state_changed("program|status", (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = "PAUSED"});
    } else if (strcmp(action_name, "program|stop") == 0) {
        g_cnc_state.status = STATUS_IDLE;
        g_cnc_state.x = 0; g_cnc_state.y = 0; g_cnc_state.z = 0; // Reset position
        data_binding_notify_state_changed("program|status", (binding_value_t){.type = BINDING_TYPE_STRING, .as.s_val = "IDLE"});
        data_binding_notify_state_changed("position|x", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.x});
        data_binding_notify_state_changed("position|y", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.y});
        data_binding_notify_state_changed("position|z", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.z});
    } else if (strcmp(action_name, "spindle|toggle") == 0 && value.type == BINDING_TYPE_BOOL) {
        g_cnc_state.spindle_on = value.as.b_val;
    } else if (strcmp(action_name, "feedrate|override") == 0 && value.type == BINDING_TYPE_INT) {
        g_cnc_state.feed_override_pct = value.as.i_val;
        data_binding_notify_state_changed("feedrate|override", (binding_value_t){.type = BINDING_TYPE_INT, .as.i_val = g_cnc_state.feed_override_pct});
    }
}

/**
 * @brief Simulates the CNC machine running and updates its state periodically.
 */
size_t g_ticks = 0;
static void cnc_tick(void) {
    if (g_cnc_state.status != STATUS_RUNNING) return;

    // Simple circle - 1 degree per tick.
    const int ticks_per_rev = 360;
    const float theta = (g_ticks++ % ticks_per_rev) / (ticks_per_rev / 360.0f) / 180.0f * M_PI;
    g_cnc_state.x = 50.0f * cosf(theta);
    g_cnc_state.y = 50.0f * sinf(theta);
    g_cnc_state.z = 50.f * (theta / (2.0 * M_PI));

    data_binding_notify_state_changed("position|x", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.x});
    data_binding_notify_state_changed("position|y", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.y});
    data_binding_notify_state_changed("position|z", (binding_value_t){.type = BINDING_TYPE_FLOAT, .as.f_val = g_cnc_state.z});
}

void render_abort(const char *msg) {
    fprintf(stderr, ANSI_BOLD_RED "\nFATAL ERROR: %s\n\n" ANSI_RESET, msg);
    fflush(stderr);
    exit(1);
}

int main(int argc, char* argv[]) {
    // --- Initialize Base Systems ---
    if (sdl_viewer_init() != 0) {
        render_abort("Failed to initialize SDL viewer.");
    }
    memset(&g_cnc_state, 0, sizeof(g_cnc_state));
    g_cnc_state.feed_override_pct = 100;
    
    data_binding_init();
    data_binding_register_action_handler(cnc_action_handler);

    lv_obj_t* screen = sdl_viewer_create_main_screen();

#if defined(CNC_LIVE_RENDER_MODE)
    // --- Option A: Live Rendering Mode ---
    printf("--- Running CNC Example in LIVE RENDER mode ---\n");
    debug_log_init();

    char* api_spec_content = read_file("api_spec.json");
    cJSON* api_spec_json = cJSON_Parse(api_spec_content);
    ApiSpec* api_spec = api_spec_parse(api_spec_json);

    char* ui_spec_content = read_file("ex_cnc/cnc_ui.yaml");
    char* error_msg = NULL;
    cJSON* ui_spec_json = yaml_to_cjson(ui_spec_content, &error_msg);
     if (error_msg) render_abort(error_msg);
    
    IRRoot* ir_root = generate_ir_from_ui_spec(ui_spec_json, api_spec);
    Registry* registry = registry_create();
    lvgl_render_backend(ir_root, api_spec, screen, registry);

#elif defined(CNC_STATIC_BUILD_MODE)
    // --- Option B: Static Compiled Mode ---
    printf("--- Running CNC Example in STATIC COMPILED mode ---\n");
    // This function is declared because it's in a separate, generated C file.

    create_ui(screen);
#else
    #error "No build mode defined. Please compile with -DCNC_LIVE_RENDER_MODE or -DCNC_STATIC_BUILD_MODE"
#endif

    // --- Main Simulation Loop (common to both modes) ---
    printf("Starting CNC simulation. Close the window to exit.\n");
    while(1) {
        uint32_t start_tick = lv_tick_get();
        
        lv_timer_handler();
        cnc_tick();

        uint32_t time_elapsed = lv_tick_elaps(start_tick);
        if(time_elapsed < 20) {
            SDL_Delay(20 - time_elapsed);
        }
    }

    return 0;
}

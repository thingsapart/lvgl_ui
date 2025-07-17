#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h> // For fcntl
#include <errno.h> // For errno
#include "lvgl.h"
#include "api_spec.h"
#include "generator.h"
#include "lvgl_renderer.h"
#include "registry.h"
#include "utils.h"
#include "cJSON.h"

// --- Global Configuration ---
bool g_strict_mode = false;
bool g_strict_registry_mode = false;

// --- Server State ---
static lv_display_t *disp;
static lv_indev_t *indev;
static lv_color_t *frame_buffer;
static lv_indev_data_t last_input_data;
static volatile bool frame_ready = false;
static int VSC_WIDTH = 480;
static int VSC_HEIGHT = 320;

// --- LVGL Driver Callbacks ---

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
    (void)display;
    (void)area;
    (void)px_map;
    frame_ready = true;
    lv_display_flush_ready(display);
}

static void read_cb(lv_indev_t *in_dev, lv_indev_data_t *data) {
    (void)in_dev;
    memcpy(data, &last_input_data, sizeof(lv_indev_data_t));
    last_input_data.state = LV_INDEV_STATE_RELEASED;
}

// --- Server Logic ---

void render_abort(const char *msg) {
    fprintf(stderr, "SERVER_FATAL_ERROR: %s\n", msg);
    fflush(stderr);
    exit(1);
}

static void send_frame() {
    if (!frame_buffer) return;
    fprintf(stderr, "SERVER_LOG: Frame sent to VSCode.\n");
    fprintf(stdout, "FRAME_DATA %d %d %zu\n", VSC_WIDTH, VSC_HEIGHT, (size_t)VSC_WIDTH * VSC_HEIGHT * sizeof(lv_color_t));
    fwrite(frame_buffer, sizeof(lv_color_t), (size_t)VSC_WIDTH * VSC_HEIGHT, stdout);
    fflush(stdout);
}

static void handle_render_command(cJSON* payload, ApiSpec* api_spec) {
    cJSON* width_item = cJSON_GetObjectItem(payload, "width");
    cJSON* height_item = cJSON_GetObjectItem(payload, "height");
    cJSON* source_item = cJSON_GetObjectItem(payload, "source");

    if (!cJSON_IsNumber(width_item) || !cJSON_IsNumber(height_item) || !cJSON_IsString(source_item)) {
        fprintf(stderr, "SERVER_LOG: Invalid render command payload.\n");
        return;
    }

    int new_width = width_item->valueint;
    int new_height = height_item->valueint;
    if (new_width != VSC_WIDTH || new_height != VSC_HEIGHT || frame_buffer == NULL) {
        VSC_WIDTH = new_width > 0 ? new_width : 1;
        VSC_HEIGHT = new_height > 0 ? new_height : 1;
        if (frame_buffer) free(frame_buffer);
        size_t buf_size = (size_t)VSC_WIDTH * VSC_HEIGHT * sizeof(lv_color_t);
        frame_buffer = malloc(buf_size);
        if (!frame_buffer) render_abort("Failed to allocate frame buffer.");
        // Use FULL render mode for maximum robustness, avoiding partial update bugs.
        lv_display_set_buffers(disp, frame_buffer, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);
        lv_display_set_resolution(disp, VSC_WIDTH, VSC_HEIGHT);
    }
    
    char tmp_filename[] = "/tmp/lvgl_vsc_ui_XXXXXX.yml";
    int fd = mkstemps(tmp_filename, 4);
    if (fd == -1) {
        render_abort("Failed to create temporary file for UI spec.");
    }
    write(fd, source_item->valuestring, strlen(source_item->valuestring));
    close(fd);

    lvgl_renderer_reload_ui(tmp_filename, api_spec, lv_screen_active(), NULL);

    unlink(tmp_filename);

    frame_ready = false;
    lv_refr_now(disp); // Force an initial render immediately
}

static void handle_input_command(cJSON* payload) {
    cJSON* type_item = cJSON_GetObjectItem(payload, "type");
    if (!cJSON_IsString(type_item)) return;

    if (strcmp(type_item->valuestring, "mouse") == 0) {
        cJSON* x_item = cJSON_GetObjectItem(payload, "x");
        cJSON* y_item = cJSON_GetObjectItem(payload, "y");
        cJSON* pressed_item = cJSON_GetObjectItem(payload, "pressed");

        if (cJSON_IsNumber(x_item) && cJSON_IsNumber(y_item) && cJSON_IsBool(pressed_item)) {
            last_input_data.point.x = (lv_coord_t)x_item->valueint;
            last_input_data.point.y = (lv_coord_t)y_item->valueint;
            last_input_data.state = cJSON_IsTrue(pressed_item) ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
            
            fprintf(stderr, "SERVER_LOG: Input received: { x: %d, y: %d, pressed: %s }\n",
                    last_input_data.point.x, last_input_data.point.y,
                    last_input_data.state == LV_INDEV_STATE_PRESSED ? "true" : "false");
        }
    }
}

static void process_command_line(char* line, ApiSpec* api_spec) {
    cJSON *json = cJSON_Parse(line);
    if (!json) {
        fprintf(stderr, "SERVER_LOG: Received invalid JSON: %s\n", line);
        return;
    }

    cJSON *command_item = cJSON_GetObjectItem(json, "command");
    if (cJSON_IsString(command_item)) {
        if (strcmp(command_item->valuestring, "render") == 0) {
            handle_render_command(json, api_spec);
        } else if (strcmp(command_item->valuestring, "input") == 0) {
            handle_input_command(json);
        }
    }
    cJSON_Delete(json);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        fprintf(stderr, "SERVER_FATAL_ERROR: API spec path not provided as a command-line argument.\n");
        return 1;
    }
    const char* api_spec_path = argv[1];

    setvbuf(stdout, NULL, _IONBF, 0);

    lv_init();

    disp = lv_display_create(VSC_WIDTH, VSC_HEIGHT);
    lv_display_set_flush_cb(disp, flush_cb);
    size_t buf_size = (size_t)VSC_WIDTH * VSC_HEIGHT * sizeof(lv_color_t);
    frame_buffer = malloc(buf_size);
    if (!frame_buffer) render_abort("Failed to allocate initial frame buffer.");
    // Use FULL render mode for maximum robustness, avoiding partial update bugs.
    lv_display_set_buffers(disp, frame_buffer, NULL, buf_size, LV_DISPLAY_RENDER_MODE_FULL);

    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    lv_indev_set_display(indev, disp);
    memset(&last_input_data, 0, sizeof(lv_indev_data_t));

    char* api_spec_content = read_file(api_spec_path);
    if (!api_spec_content) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "Could not read API spec file at '%s'", api_spec_path);
        render_abort(err_msg);
    }

    cJSON* api_spec_json = cJSON_Parse(api_spec_content);
    if (!api_spec_json) render_abort("Could not parse api_spec.json");
    free(api_spec_content);

    ApiSpec* api_spec = api_spec_parse(api_spec_json);
    if (!api_spec) render_abort("Failed to parse API spec into internal structures.");
    
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    static char stdin_buffer[8192];
    static size_t stdin_buffer_len = 0;

    fprintf(stderr, "SERVER_LOG: LVGL VSCode Server started successfully.\n");

    while(1) {
        // --- 1. Read and process all available commands from stdin ---
        ssize_t bytes_read = read(STDIN_FILENO, stdin_buffer + stdin_buffer_len, sizeof(stdin_buffer) - stdin_buffer_len - 1);
        if (bytes_read > 0) {
            stdin_buffer_len += bytes_read;
            stdin_buffer[stdin_buffer_len] = '\0'; 

            char* line_end;
            while ((line_end = strchr(stdin_buffer, '\n')) != NULL) {
                *line_end = '\0';
                process_command_line(stdin_buffer, api_spec);
                
                size_t remaining_len = stdin_buffer_len - (line_end - stdin_buffer + 1);
                memmove(stdin_buffer, line_end + 1, remaining_len);
                stdin_buffer_len = remaining_len;
                stdin_buffer[stdin_buffer_len] = '\0';
            }
        }

        // --- 2. Drive LVGL's internal state forward ---
        const int loop_period_ms = 100;
        lv_tick_inc(loop_period_ms);
        lv_timer_handler();

        // --- 3. Force a redraw and send the frame ---
        lv_refr_now(disp); // This synchronously calls flush_cb, which sets frame_ready
        
        if (frame_ready) {
            send_frame();
            frame_ready = false;
        }

        // --- 4. Sleep for the remainder of the cycle ---
        usleep(loop_period_ms * 1000);
    }
    
    // Cleanup
    free(frame_buffer);
    api_spec_free(api_spec);
    cJSON_Delete(api_spec_json);
    lv_deinit();

    fprintf(stderr, "SERVER_LOG: Server shutting down.\n");
    return 0;
}

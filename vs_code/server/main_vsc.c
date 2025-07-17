#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h> // For fcntl
#include <errno.h> // For errno
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h> // For htons, htonl
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
bool g_logging_enabled = false;

 int mkstemps(char *template, int suffixlen);

// --- Server State ---
static lv_display_t *disp;
static lv_indev_t *indev;
static lv_indev_data_t last_input_data;
static int VSC_WIDTH = 480;
static int VSC_HEIGHT = 320;
static lv_color_t* lvgl_draw_buffer = NULL;
static uint8_t* rgba_buffer = NULL; // Buffer for RGBA8888 conversion

// --- Helper for color conversion ---
/**
 * @brief Converts a buffer from LVGL's ARGB8888 to the webview's expected RGBA8888.
 * On little-endian systems, LVGL's lv_color_t (A,R,G,B) is stored as [B,G,R,A].
 * This function converts it to [R,G,B,A].
 * @param src The source pixel buffer from LVGL.
 * @param dest The destination buffer for the webview.
 * @param pixel_count The number of pixels to convert.
 */
static void convert_argb8888_to_rgba8888(const uint8_t* src, uint8_t* dest, uint32_t pixel_count) {
    for (uint32_t i = 0; i < pixel_count; ++i) {
        const uint8_t* p_src = &src[i * 4];
        uint8_t* p_dest = &dest[i * 4];

        // LVGL ARGB8888 (in memory) -> RGBA8888
        // src[0] -> Blue
        // src[1] -> Green
        // src[2] -> Red
        // src[3] -> Alpha
        p_dest[0] = p_src[2]; // R
        p_dest[1] = p_src[1]; // G
        p_dest[2] = p_src[0]; // B
        p_dest[3] = p_src[3]; // A
    }
}

// --- LVGL Driver Callbacks ---

static void flush_cb(lv_display_t *display, const lv_area_t *area, uint8_t *px_map) {
    // This callback is now the main engine for sending frames. It's called by LVGL
    // whenever a part of the screen needs to be updated. With PARTIAL render mode,
    // this can be a small portion of the screen.

    int32_t x = area->x1;
    int32_t y = area->y1;
    int32_t width = lv_area_get_width(area);
    int32_t height = lv_area_get_height(area);

    if (!rgba_buffer) {
        lv_display_flush_ready(display);
        return;
    }

    size_t pixel_count = (size_t)width * height;
    size_t rgba_buf_size = pixel_count * 4;

    // Convert the rendered LVGL buffer (ARGB8888) to RGBA8888 for the webview.
    convert_argb8888_to_rgba8888(px_map, rgba_buffer, pixel_count);

    // Prepare header for the new length-prefixed protocol
    char header[24];
    memcpy(header, "DATA:|FRAME|", 12);
    uint16_t net_x = htons(x);
    uint16_t net_y = htons(y);
    uint16_t net_w = htons(width);
    uint16_t net_h = htons(height);
    uint32_t net_size = htonl(rgba_buf_size);

    memcpy(header + 12, &net_x, 2);
    memcpy(header + 14, &net_y, 2);
    memcpy(header + 16, &net_w, 2);
    memcpy(header + 18, &net_h, 2);
    memcpy(header + 20, &net_size, 4);

    // Send header and payload
    fwrite(header, 1, sizeof(header), stdout);
    fwrite(rgba_buffer, 1, rgba_buf_size, stdout);
    fflush(stdout);

    if (g_logging_enabled) {
        fprintf(stderr, "SERVER_LOG: Sent frame rect {x:%d, y:%d, w:%d, h:%d, bytes:%zu}\n", x, y, width, height, rgba_buf_size);
    }

    // Tell LVGL that we are done with the flushing.
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

static void handle_render_command(cJSON* payload, ApiSpec* api_spec) {
    cJSON* width_item = cJSON_GetObjectItem(payload, "width");
    cJSON* height_item = cJSON_GetObjectItem(payload, "height");
    cJSON* source_item = cJSON_GetObjectItem(payload, "source");

    if (!cJSON_IsNumber(width_item) || !cJSON_IsNumber(height_item) || !cJSON_IsString(source_item)) {
        if (g_logging_enabled) {
            fprintf(stderr, "SERVER_LOG: Invalid render command payload.\n");
        }
        return;
    }

    int new_width = width_item->valueint;
    int new_height = height_item->valueint;
    if (new_width != VSC_WIDTH || new_height != VSC_HEIGHT || lvgl_draw_buffer == NULL) {
        VSC_WIDTH = new_width > 0 ? new_width : 1;
        VSC_HEIGHT = new_height > 0 ? new_height : 1;

        // Reallocate the LVGL draw buffer
        if (lvgl_draw_buffer) free(lvgl_draw_buffer);
        size_t lv_buf_size = (size_t)VSC_WIDTH * VSC_HEIGHT * sizeof(lv_color_t);
        lvgl_draw_buffer = malloc(lv_buf_size);
        if (!lvgl_draw_buffer) render_abort("Failed to allocate LVGL draw buffer.");

        // Reallocate the RGBA conversion buffer
        if (rgba_buffer) free(rgba_buffer);
        size_t rgba_buf_size = (size_t)VSC_WIDTH * VSC_HEIGHT * 4;
        rgba_buffer = malloc(rgba_buf_size);
        if(!rgba_buffer) render_abort("Failed to allocate RGBA conversion buffer.");

        // Use PARTIAL render mode for efficiency.
        lv_display_set_buffers(disp, lvgl_draw_buffer, NULL, lv_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_resolution(disp, VSC_WIDTH, VSC_HEIGHT);
    }

    char tmp_filename[] = "/tmp/lvgl_vsc_ui_XXXXXX.yml";
    int fd = mkstemps(tmp_filename, 4);
    if (fd == -1) {
        render_abort("Failed to create temporary file for UI spec.");
    }
    write(fd, source_item->valuestring, strlen(source_item->valuestring));
    close(fd);

    lv_obj_clean(lv_screen_active());
    lvgl_renderer_reload_ui(tmp_filename, api_spec, lv_screen_active(), NULL);

    unlink(tmp_filename);

    // Invalidate the screen to force LVGL to redraw it in the next lv_timer_handler call.
    lv_obj_invalidate(lv_screen_active());
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

            if (g_logging_enabled) {
                fprintf(stderr, "SERVER_LOG: Input received: { x: %d, y: %d, pressed: %s }\n",
                        last_input_data.point.x, last_input_data.point.y,
                        last_input_data.state == LV_INDEV_STATE_PRESSED ? "true" : "false");
            }
        }
    }
}

static void process_command_line(char* line, ApiSpec* api_spec) {
    cJSON *json = cJSON_Parse(line);
    if (!json) {
        if (g_logging_enabled) {
            fprintf(stderr, "SERVER_LOG: Received invalid JSON: %s\n", line);
        }
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
    const char* api_spec_path = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0) {
            g_logging_enabled = true;
        } else if (api_spec_path == NULL) {
            api_spec_path = argv[i];
        }
    }

    if (api_spec_path == NULL) {
        fprintf(stderr, "SERVER_FATAL_ERROR: API spec path not provided as a command-line argument.\n");
        return 1;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    lv_init();

    disp = lv_display_create(VSC_WIDTH, VSC_HEIGHT);
    lv_display_set_flush_cb(disp, flush_cb);

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

    // Set stdin to non-blocking mode
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    static char stdin_buffer[8192];
    static size_t stdin_buffer_len = 0;

    if (g_logging_enabled) {
        fprintf(stderr, "SERVER_LOG: LVGL VSCode Server started successfully.\n");
    }

    struct timeval last_tick_tv;
    gettimeofday(&last_tick_tv, NULL);

    while(1) {
        // --- 1. Determine wait time and use select() for efficient waiting ---
        uint32_t wait_ms = 100;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        struct timeval timeout;
        if (wait_ms >= 1000) {
            timeout.tv_sec = 1; // Wait a reasonable max time
            timeout.tv_usec = 0;
        } else {
            timeout.tv_sec = wait_ms / 1000;
            timeout.tv_usec = (wait_ms % 1000) * 1000;
        }

        int retval = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);

        if (retval == -1) {
            perror("select()");
            break; // Exit on select error
        }

        // --- 2. Increment LVGL tick based on actual elapsed time ---
        struct timeval now_tv;
        gettimeofday(&now_tv, NULL);
        long elapsed_ms = (now_tv.tv_sec - last_tick_tv.tv_sec) * 1000 +
                          (now_tv.tv_usec - last_tick_tv.tv_usec) / 1000;
        if (elapsed_ms > 0) {
            lv_tick_inc(elapsed_ms);
        }
        last_tick_tv = now_tv;

        // --- 3. Process commands from stdin if available ---
        if (retval > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
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
            } else if (bytes_read <= 0) {
                // stdin closed or error, exit loop
                break;
            }
        }

        // --- 4. Drive LVGL's internal state forward ---
        // This will call flush_cb if a redraw is needed.
        lv_timer_handler();
    }

    // Cleanup
    if (lvgl_draw_buffer) free(lvgl_draw_buffer);
    if (rgba_buffer) free(rgba_buffer);
    api_spec_free(api_spec);
    cJSON_Delete(api_spec_json);
    lv_deinit();

    if (g_logging_enabled) {
        fprintf(stderr, "SERVER_LOG: Server shutting down.\n");
    }
    return 0;
}

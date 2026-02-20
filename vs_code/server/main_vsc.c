#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h> // For fcntl
#include <errno.h> // For errno
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h> // For htons, htonl
#include <ctype.h>
#include "lvgl.h"
#include "api_spec.h"
#include "generator.h"
#include "lvgl_renderer.h"
#include "registry.h"
#include "utils.h"
#include "cJSON.h"
#include "yaml_parser.h"
#include "lvgl_assert_handler.h"
#include "ui_sim.h" // ADDED: For UI-Sim

// --- Configuration Defines ---
#define STDIN_BUFFER_SIZE 65536 // 64KB buffer for input commands
#define RENDER_FPS 10

// --- Global Configuration ---
bool g_strict_mode = false;
bool g_strict_registry_mode = false;
bool g_logging_enabled = false;
// For the VSCode server, UI-Sim trace messages must be sent to stderr to avoid
// corrupting the binary stdout stream. This can be enabled by the extension.
bool g_ui_sim_trace_enabled = false;
bool g_ui_sim_trace_no_time_enabled = false; // ADDED: To suppress time-only trace messages

// Define the global function pointer that LVGL's assert handler will see.
void (*g_lvgl_assert_abort_cb)(const char *msg) = NULL;

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

    // Prepare header for the new length-prefixed protocol.
    // The header now includes the TOTAL display dimensions with every frame.
    char header[28]; // Increased size for total_w, total_h
    memcpy(header, "DATA:|FRAME|", 12);

    // Add total display dimensions
    uint16_t net_total_w = htons(lv_display_get_horizontal_resolution(display));
    uint16_t net_total_h = htons(lv_display_get_vertical_resolution(display));
    memcpy(header + 12, &net_total_w, 2);
    memcpy(header + 14, &net_total_h, 2);

    // Add partial frame dimensions
    uint16_t net_x = htons(x);
    uint16_t net_y = htons(y);
    uint16_t net_w = htons(width);
    uint16_t net_h = htons(height);
    uint32_t net_size = htonl(rgba_buf_size);

    memcpy(header + 16, &net_x, 2);
    memcpy(header + 18, &net_y, 2);
    memcpy(header + 20, &net_w, 2);
    memcpy(header + 22, &net_h, 2);
    memcpy(header + 24, &net_size, 4);

    // Send header and payload
    fwrite(header, 1, sizeof(header), stdout);
    fwrite(rgba_buffer, 1, rgba_buf_size, stdout);
    fflush(stdout);

    if (g_logging_enabled) {
        fprintf(stderr, "SERVER_LOG: Sent frame total_dim{%dx%d} rect {x:%d, y:%d, w:%d, h:%d, bytes:%zu}\n",
                lv_display_get_horizontal_resolution(display), lv_display_get_vertical_resolution(display),
                x, y, width, height, rgba_buf_size);
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
    // This function no longer aborts the server. It sends a formatted error
    // message to stderr, which the VSCode extension will display in the
    // webview's console. The server continues to run.
    fprintf(stderr, "[ERROR] %s\n", msg);
    fflush(stderr);
}

/**
 * @brief The callback function that LVGL will invoke when an LV_ASSERT fails.
 * This function reports an unrecoverable error and exits the server, allowing the
 * VSCode extension to restart it on the next render attempt.
 */
static void lvgl_assert_handler_cb(const char *msg) {
  const char *fmt = "LVGL ASSERT FAILED! Likely invalid argument, like NULL or object of invalid passed to LVGL function. The server will now exit and be restarted on the next change.\n    %s";

  int len = snprintf(NULL, 0, fmt, msg);
  char *buffer = (char *)malloc(len + 1);
  snprintf(buffer, 0, fmt, msg);
  render_abort(buffer);

  // An LVGL assert means the library's internal state is corrupt.
  // The most robust action is to exit immediately.
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

        if (g_logging_enabled) {
            fprintf(stderr, "SERVER_LOG: Resolution changing to %dx%d.\n", VSC_WIDTH, VSC_HEIGHT);
        }

        // Reallocate the LVGL draw buffer
        if (lvgl_draw_buffer) free(lvgl_draw_buffer);
        size_t lv_buf_size = (size_t)VSC_WIDTH * VSC_HEIGHT * sizeof(lv_color_t);
        lvgl_draw_buffer = malloc(lv_buf_size);
        if (!lvgl_draw_buffer) {
            render_abort("Failed to allocate LVGL draw buffer.");
            return;
        }

        // Reallocate the RGBA conversion buffer
        if (rgba_buffer) free(rgba_buffer);
        size_t rgba_buf_size = (size_t)VSC_WIDTH * VSC_HEIGHT * 4;
        rgba_buffer = malloc(rgba_buf_size);
        if(!rgba_buffer) {
            render_abort("Failed to allocate RGBA conversion buffer.");
            return;
        }
        // Use PARTIAL render mode for efficiency.
        lv_display_set_buffers(disp, lvgl_draw_buffer, NULL, lv_buf_size, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_resolution(disp, VSC_WIDTH, VSC_HEIGHT);
    }

    // lvgl_renderer_reload_ui_from_string will handle parsing and rendering.
    // It also handles cleaning the screen internally, both on success and failure.
    lvgl_renderer_reload_ui_from_string(source_item->valuestring, api_spec, lv_screen_active(), NULL);

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

static void send_cjson_reply(const cJSON* reply_json) {
    if (!reply_json) return;
    char* payload = cJSON_PrintUnformatted((cJSON*)reply_json);
    if (!payload) return;

    // Header: "DATA:|CMPLT |" (12 bytes) + payload_size (4 bytes)
    char header[16];
    memcpy(header, "DATA:|CMPLT |", 12);
    uint32_t net_size = htonl((uint32_t)strlen(payload));
    memcpy(header + 12, &net_size, 4);

    fwrite(header, 1, sizeof(header), stdout);
    fwrite(payload, 1, strlen(payload), stdout);
    fflush(stdout);

    free(payload);
}

// Collect context variable names recursively for a component, following use-view subcomponents.
static void collect_context_vars_recursive(const cJSON* node, const cJSON* root_array, cJSON* collector, cJSON* visited) {
    if (!node) return;
    if (cJSON_IsObject(node)) {
        cJSON* item = NULL;
        cJSON_ArrayForEach(item, (cJSON*)node) {
            // If key starts with '$', add var name and optional doc (after first '/')
            if (item->string && item->string[0] == '$') {
                const char* raw = item->string + 1;
                const char* slash = strchr(raw, '/');
                size_t namelen = slash ? (size_t)(slash - raw) : strlen(raw);
                char namebuf[256]; if (namelen >= sizeof(namebuf)) namelen = sizeof(namebuf)-1;
                memcpy(namebuf, raw, namelen); namebuf[namelen] = '\0';
                const char* docstr = (slash && slash[1]) ? slash + 1 : NULL;

                // Add only unique by name
                bool found = false;
                cJSON* it = NULL;
                cJSON_ArrayForEach(it, collector) {
                    if (cJSON_IsObject(it)) {
                        cJSON* nm = cJSON_GetObjectItemCaseSensitive(it, "name");
                        if (nm && cJSON_IsString(nm) && strcmp(nm->valuestring, namebuf) == 0) { found = true; break; }
                    } else if (cJSON_IsString(it) && strcmp(it->valuestring, namebuf) == 0) { found = true; break; }
                }
                if (!found) {
                    cJSON* obj = cJSON_CreateObject();
                    cJSON_AddStringToObject(obj, "name", namebuf);
                    if (docstr) cJSON_AddStringToObject(obj, "doc", docstr);
                    cJSON_AddItemToArray(collector, obj);
                }
            }
            // If this is a use-view, resolve subcomponent and recurse
            if (item->string && strcmp(item->string, "type") == 0 && cJSON_IsString(item) && strcmp(item->valuestring, "use-view") == 0) {
                // find sibling id
                cJSON* id_item = cJSON_GetObjectItemCaseSensitive(node, "id");
                if (id_item && cJSON_IsString(id_item)) {
                    const char* cid = id_item->valuestring;
                    // search root array for component with matching id
                    cJSON* top = NULL;
                    cJSON_ArrayForEach(top, (cJSON*)root_array) {
                        cJSON* t_type = cJSON_GetObjectItemCaseSensitive(top, "type");
                        cJSON* t_id = cJSON_GetObjectItemCaseSensitive(top, "id");
                        if (t_type && cJSON_IsString(t_type) && strcmp(t_type->valuestring, "component") == 0 && t_id && cJSON_IsString(t_id)) {
                            if (strcmp(t_id->valuestring, cid) == 0) {
                                // avoid infinite recursion: check visited list
                                bool already = false;
                                if (visited) {
                                    cJSON* vit = NULL;
                                    cJSON_ArrayForEach(vit, visited) {
                                        if (cJSON_IsString(vit) && strcmp(vit->valuestring, t_id->valuestring) == 0) { already = true; break; }
                                    }
                                }
                                if (!already) {
                                    if (visited) cJSON_AddItemToArray(visited, cJSON_CreateString(t_id->valuestring));
                                // found component content: try 'root' or 'content'
                                cJSON* content = cJSON_GetObjectItemCaseSensitive(top, "root");
                                if (!content) content = cJSON_GetObjectItemCaseSensitive(top, "content");
                                if (content) collect_context_vars_recursive(content, root_array, collector, visited);
                                }
                                break;
                            }
                        }
                    }
                }
            }

            // Recurse into value
            collect_context_vars_recursive(item, root_array, collector, visited);
        }
        return;
    }
    if (cJSON_IsArray(node)) {
        cJSON* it = NULL;
        cJSON_ArrayForEach(it, (cJSON*)node) {
            collect_context_vars_recursive(it, root_array, collector, visited);
        }
        return;
    }
    // strings/numbers: optionally scan for '$var' in strings
    if (cJSON_IsString(node)) {
        const char* s = node->valuestring;
        const char* p = s;
        while ((p = strchr(p, '$')) != NULL) {
            p++;
            // read identifier
            char buf[128]; int bi = 0;
            while (*p && (isalnum((unsigned char)*p) || *p == '_' || *p == '-')) {
                if (bi < (int)sizeof(buf)-1) buf[bi++] = *p;
                p++;
            }
            if (bi > 0) {
                buf[bi] = '\0';
                const char* doc = NULL;
                // if next char is '/', the rest of the string is the doc-info
                if (*p == '/') {
                    doc = p + 1; // rest of string
                }
                // add unique (by name)
                bool found = false;
                cJSON* it = NULL;
                cJSON_ArrayForEach(it, collector) {
                    if (cJSON_IsObject(it)) {
                        cJSON* nm = cJSON_GetObjectItemCaseSensitive(it, "name");
                        if (nm && cJSON_IsString(nm) && strcmp(nm->valuestring, buf) == 0) { found = true; break; }
                    } else if (cJSON_IsString(it) && strcmp(it->valuestring, buf) == 0) { found = true; break; }
                }
                if (!found) {
                    cJSON* obj = cJSON_CreateObject();
                    cJSON_AddStringToObject(obj, "name", buf);
                    if (doc && *doc) cJSON_AddStringToObject(obj, "doc", doc);
                    cJSON_AddItemToArray(collector, obj);
                }
            }
        }
    }
}

static void handle_completions_command(cJSON* payload, ApiSpec* api_spec) {
    cJSON* source_item = cJSON_GetObjectItem(payload, "source");
    if (!source_item || !cJSON_IsString(source_item)) return;

    char* err = NULL;
    cJSON* ui_json = yaml_to_cjson(source_item->valuestring, &err);
    if (!ui_json) {
        if (err) {
            fprintf(stderr, "[ERROR] YAML parse failed: %s\n", err);
            free(err);
        } else {
            fprintf(stderr, "[ERROR] YAML parse failed (unknown)\n");
        }
        return;
    }

    // Build response: components array with id, info, context_vars
    cJSON* response = cJSON_CreateObject();
    cJSON* comps = cJSON_CreateArray();
    cJSON_AddItemToObject(response, "components", comps);

    cJSON* item = NULL;
    cJSON_ArrayForEach(item, ui_json) {
        if (!cJSON_IsObject(item)) continue;
        cJSON* type_item = cJSON_GetObjectItemCaseSensitive(item, "type");
        cJSON* id_item = cJSON_GetObjectItemCaseSensitive(item, "id");
        if (!type_item || !cJSON_IsString(type_item) || !id_item || !cJSON_IsString(id_item)) continue;
        if (strcmp(type_item->valuestring, "component") != 0) continue;

        cJSON* comp_obj = cJSON_CreateObject();
        cJSON_AddStringToObject(comp_obj, "id", id_item->valuestring);
        cJSON* info_item = cJSON_GetObjectItemCaseSensitive(item, "info");
        if (info_item && cJSON_IsString(info_item)) cJSON_AddStringToObject(comp_obj, "info", info_item->valuestring);

        // find content (root or content)
        cJSON* content = cJSON_GetObjectItemCaseSensitive(item, "root");
        if (!content) content = cJSON_GetObjectItemCaseSensitive(item, "content");
        cJSON* vars = cJSON_CreateArray();
        if (content) {
            cJSON* visited = cJSON_CreateArray();
            // mark this component id as visited to avoid self-recursion
            if (id_item && cJSON_IsString(id_item)) cJSON_AddItemToArray(visited, cJSON_CreateString(id_item->valuestring));
            collect_context_vars_recursive(content, ui_json, vars, visited);
            cJSON_Delete(visited);
        }
        cJSON_AddItemToObject(comp_obj, "context_vars", vars);

        // build context snippet
        if (cJSON_GetArraySize(vars) > 0) {
            // build simple YAML snippet (no indent) and prefill values with doc if present
            size_t buf_sz = 1024;
            char* buf = malloc(buf_sz);
            if (buf) {
                buf[0] = '\0';
                strcat(buf, "context:\n");
                cJSON* it = NULL;
                cJSON_ArrayForEach(it, vars) {
                    const char* name = NULL;
                    const char* doc = NULL;
                    if (cJSON_IsObject(it)) {
                        cJSON* nm = cJSON_GetObjectItemCaseSensitive(it, "name");
                        cJSON* dc = cJSON_GetObjectItemCaseSensitive(it, "doc");
                        if (nm && cJSON_IsString(nm)) name = nm->valuestring;
                        if (dc && cJSON_IsString(dc)) doc = dc->valuestring;
                    } else if (cJSON_IsString(it)) {
                        name = it->valuestring;
                    }
                    if (!name) continue;
                    // Prepare line: either "  name: doc\n" or "  name:\n"
                    const char* value_part = doc ? doc : "";
                    size_t need = strlen(buf) + strlen("  :\n") + strlen(name) + strlen(value_part) + 4;
                    if (need > buf_sz) { buf_sz = need + 256; buf = realloc(buf, buf_sz); }
                    strcat(buf, "  "); strcat(buf, name); strcat(buf, ":");
                    if (doc) { strcat(buf, " "); strcat(buf, doc); }
                    strcat(buf, "\n");
                }
                cJSON_AddStringToObject(comp_obj, "context_snippet", buf);
                free(buf);
            }
        }

        cJSON_AddItemToArray(comps, comp_obj);
    }

    // include requestId if present so the client can correlate
    cJSON* reqId = cJSON_GetObjectItemCaseSensitive(payload, "requestId");
    if (reqId && cJSON_IsString(reqId)) {
        cJSON_AddStringToObject(response, "requestId", reqId->valuestring);
    }
    // send reply and cleanup
    send_cjson_reply(response);
    cJSON_Delete(response);
    cJSON_Delete(ui_json);
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
        } else if (strcmp(command_item->valuestring, "completions") == 0) {
            handle_completions_command(json, api_spec);
        }
    }
    cJSON_Delete(json);
}

int main(int argc, char* argv[]) {
    const char* api_spec_path = NULL;
    bool background_mode = false;
    bool no_timers_mode = false;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--log") == 0) {
            g_logging_enabled = true;
        } else if (strcmp(argv[i], "--background") == 0) {
            background_mode = true;
        } else if (strcmp(argv[i], "--no-timers") == 0) {
            no_timers_mode = true;
        } else if (strcmp(argv[i], "--trace-sim-no-time") == 0) {
            g_ui_sim_trace_enabled = true;
            g_ui_sim_trace_no_time_enabled = true;
        } else if (api_spec_path == NULL) {
            api_spec_path = argv[i];
        }
    }

    if (api_spec_path == NULL) {
        fprintf(stderr, "[ERROR] API spec path not provided as a command-line argument.\n");
        return 1;
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    lv_init();

    // Set our custom assert handler callback *before* any other LVGL operations.
    g_lvgl_assert_abort_cb = lvgl_assert_handler_cb;

    disp = lv_display_create(VSC_WIDTH, VSC_HEIGHT);
    lv_display_set_flush_cb(disp, flush_cb);

    indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev, read_cb);
    lv_indev_set_display(indev, disp);
    memset(&last_input_data, 0, sizeof(lv_indev_data_t));

    // Send initial handshake message with the starting resolution
    // Using a specific command "INIT" to distinguish from "FRAME"
    char init_header[16]; // DATA:|INIT |wwww_hhhh
    memcpy(init_header, "DATA:|INIT |", 12); // Note the space after INIT
    uint16_t net_w = htons(VSC_WIDTH);
    uint16_t net_h = htons(VSC_HEIGHT);
    memcpy(init_header + 12, &net_w, 2);
    memcpy(init_header + 14, &net_h, 2);
    fwrite(init_header, 1, sizeof(init_header), stdout);
    fflush(stdout);
    if (g_logging_enabled) {
        fprintf(stderr, "SERVER_LOG: Sent INIT handshake with resolution %dx%d\n", VSC_WIDTH, VSC_HEIGHT);
    }

    char* api_spec_content = read_file(api_spec_path);
    if (!api_spec_content) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "Could not read API spec file at '%s'", api_spec_path);
        render_abort(err_msg);
        exit(1); // Still exit here, server can't start.
    }

    cJSON* api_spec_json = cJSON_Parse(api_spec_content);
    if (!api_spec_json) {
        render_abort("Could not parse api_spec.json");
        exit(1); // Still exit here, server can't start.
    }
    free(api_spec_content);

    ApiSpec* api_spec = api_spec_parse(api_spec_json);
    if (!api_spec) {
        render_abort("Failed to parse API spec into internal structures.");
        exit(1); // Still exit here, server can't start.
    }

    // Set stdin to non-blocking mode
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    static char stdin_buffer[STDIN_BUFFER_SIZE];
    static size_t stdin_buffer_len = 0;

    if (g_logging_enabled) {
        fprintf(stderr, "SERVER_LOG: LVGL VSCode Server started successfully.\n");
    } else if (background_mode) {
        // Minimal startup message for background mode
        fprintf(stderr, "SERVER_LOG: LVGL VSCode Server started in background mode.\n");
    }

    struct timeval last_tick_tv;
    gettimeofday(&last_tick_tv, NULL);

    while(1) {
        // --- 1. Determine wait time and use select() for efficient waiting ---
        // This might run continuously, better to cap FPS as below.
        //uint32_t wait_ms = lv_timer_get_idle();

        // Cap FPS. In no-timers/background mode use a long idle to save CPU.
        uint32_t wait_ms;
        if (no_timers_mode) {
            wait_ms = 5000; // 5s
        } else {
            wait_ms = (1000 / RENDER_FPS);
        }
        if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: Loop start. Waiting for input or timeout (idle: %u ms)...\n", wait_ms);

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(STDIN_FILENO, &readfds);

        struct timeval timeout;
        if (wait_ms > 500) { // Cap wait time to stay responsive
            timeout.tv_sec = 0;
            timeout.tv_usec = 500 * 1000;
        } else {
            timeout.tv_sec = wait_ms / 1000;
            timeout.tv_usec = (wait_ms % 1000) * 1000;
        }

        int retval = select(STDIN_FILENO + 1, &readfds, NULL, NULL, &timeout);
        if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: select() returned %d.\n", retval);


        if (retval == -1) {
            if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: select() error, shutting down.\n");
            perror("select()");
            break; // Exit on select error
        }

        // --- 2. Increment system ticks based on actual elapsed time ---
        struct timeval now_tv;
        gettimeofday(&now_tv, NULL);
        long elapsed_ms = (now_tv.tv_sec - last_tick_tv.tv_sec) * 1000 +
                          (now_tv.tv_usec - last_tick_tv.tv_usec) / 1000;
        if (elapsed_ms > 0) {
            if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: Elapsed time: %ld ms. Advancing ticks.\n", elapsed_ms);
            // Always advance LVGL ticks even in no-timers mode so timers don't stall
            lv_tick_inc(elapsed_ms);
            if (!no_timers_mode) {
                // Drive the UI simulator forward when timers are active
                ui_sim_tick((float)elapsed_ms / 1000.0f);
            }
        }
        last_tick_tv = now_tv;

        // --- 3. Process commands from stdin if available ---
        if (retval > 0 && FD_ISSET(STDIN_FILENO, &readfds)) {
            if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: stdin is readable, attempting to read...\n");

            // Check for buffer overrun BEFORE reading
            size_t remaining_space = sizeof(stdin_buffer) - stdin_buffer_len - 1;
            if (remaining_space == 0) {
                char err_msg[256];
                snprintf(err_msg, sizeof(err_msg), "Input command buffer overrun. The UI spec is likely too large (max: %d bytes).", STDIN_BUFFER_SIZE);
                render_abort(err_msg);
                // We must exit, as we can't process the truncated command.
                break;
            }

            ssize_t bytes_read = read(STDIN_FILENO, stdin_buffer + stdin_buffer_len, remaining_space);

            if (bytes_read > 0) {
                 if (g_logging_enabled) {
                    fprintf(stderr, "SERVER_LOG: Read %zd bytes from stdin. Current buffer content: '%.*s'\n", bytes_read, (int)(stdin_buffer_len + bytes_read), stdin_buffer);
                }
                stdin_buffer_len += bytes_read;
                stdin_buffer[stdin_buffer_len] = '\0';

                char* line_end;
                while ((line_end = strchr(stdin_buffer, '\n')) != NULL) {
                    *line_end = '\0';
                    if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: Processing command line: '%s'\n", stdin_buffer);
                    process_command_line(stdin_buffer, api_spec);

                    size_t remaining_len = stdin_buffer_len - (line_end - stdin_buffer + 1);
                    memmove(stdin_buffer, line_end + 1, remaining_len);
                    stdin_buffer_len = remaining_len;
                    stdin_buffer[stdin_buffer_len] = '\0';
                }
            } else if (bytes_read == 0) {
                // This means stdin was closed by the parent process.
                if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: stdin closed (EOF detected). Shutting down.\n");
                break; // This is a clean way for the extension to terminate the server.
            } else { // bytes_read < 0
                // An actual read error occurred (not just EAGAIN, which select prevents)
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: stdin read error (%s). Shutting down.\n", strerror(errno));
                    break;
                }
                 if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: Read returned EAGAIN/EWOULDBLOCK despite select(). Ignoring.\n");
            }
        }

        // --- 4. Drive LVGL's internal state forward ---
        if (!no_timers_mode) {
            if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: Loop end. Calling lv_timer_handler().\n");
            // This will call flush_cb if a redraw is needed.
            lv_timer_handler();
        } else {
            if (g_logging_enabled) fprintf(stderr, "SERVER_LOG: Loop end. Skipping lv_timer_handler() because no-timers mode is active.\n");
        }
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

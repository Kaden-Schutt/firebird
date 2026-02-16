// Headless MCP server for Firebird TI-Nspire emulator
// JSON-RPC 2.0 over stdin/stdout (MCP stdio transport)
// Single-threaded: polled from gui_do_stuff() at ~100Hz

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "mcp_headless.h"
#include "mcp_keymap.h"

extern "C" {
#include "vendor/cJSON.h"
}

// Firebird core headers
#include "core/emu.h"
#include "core/cpu.h"
#include "core/mem.h"
#include "core/mmu.h"
#include "core/keypad.h"
#include "core/lcd.h"
#include "core/debug.h"
#include "core/usblink.h"
#include "core/usblink_queue.h"

// stb_image_write implementation
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "vendor/stb_image_write.h"

struct mcp_state mcp;

// Debug output goes to stderr when MCP is active
static FILE *debug_out = NULL;

// ---- Utility ----

uint64_t mcp_time_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

// Base64 encoding for screenshot data
static const char b64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *base64_encode(const uint8_t *data, int len)
{
    int out_len = 4 * ((len + 2) / 3);
    char *out = (char *)malloc(out_len + 1);
    if (!out) return NULL;

    int i, j;
    for (i = 0, j = 0; i < len - 2; i += 3) {
        out[j++] = b64_table[(data[i] >> 2) & 0x3F];
        out[j++] = b64_table[((data[i] & 0x3) << 4) | (data[i+1] >> 4)];
        out[j++] = b64_table[((data[i+1] & 0xF) << 2) | (data[i+2] >> 6)];
        out[j++] = b64_table[data[i+2] & 0x3F];
    }
    if (i < len) {
        out[j++] = b64_table[(data[i] >> 2) & 0x3F];
        if (i == len - 1) {
            out[j++] = b64_table[((data[i] & 0x3) << 4)];
            out[j++] = '=';
        } else {
            out[j++] = b64_table[((data[i] & 0x3) << 4) | (data[i+1] >> 4)];
            out[j++] = b64_table[((data[i+1] & 0xF) << 2)];
        }
        out[j++] = '=';
    }
    out[j] = '\0';
    return out;
}

// Send a raw JSON line to the client (stdout or TCP)
static void mcp_send_raw(const char *json)
{
    if (mcp.tcp_port > 0) {
        // TCP mode
        if (mcp.client_fd < 0) return; // no client connected
        int len = strlen(json);
        // Write JSON + newline, ignoring partial writes for simplicity
        write(mcp.client_fd, json, len);
        write(mcp.client_fd, "\n", 1);
    } else {
        // stdin/stdout mode
        fprintf(stdout, "%s\n", json);
        fflush(stdout);
    }
}

static void mcp_send_json(cJSON *root)
{
    char *s = cJSON_PrintUnformatted(root);
    if (s) {
        mcp_send_raw(s);
        free(s);
    }
    cJSON_Delete(root);
}

// Send a JSON-RPC result response
static void mcp_respond(int id, cJSON *result)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(resp, "id", id);
    cJSON_AddItemToObject(resp, "result", result);
    mcp_send_json(resp);
}

// Send a JSON-RPC error response
static void mcp_respond_error(int id, int code, const char *msg)
{
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
    cJSON_AddNumberToObject(resp, "id", id);
    cJSON *err = cJSON_CreateObject();
    cJSON_AddNumberToObject(err, "code", code);
    cJSON_AddStringToObject(err, "message", msg);
    cJSON_AddItemToObject(resp, "error", err);
    mcp_send_json(resp);
}

// ---- MCP Protocol Helpers ----

static cJSON *make_tool(const char *name, const char *description, cJSON *schema)
{
    cJSON *tool = cJSON_CreateObject();
    cJSON_AddStringToObject(tool, "name", name);
    cJSON_AddStringToObject(tool, "description", description);
    cJSON_AddItemToObject(tool, "inputSchema", schema);
    return tool;
}

static cJSON *make_schema(const char *props_json)
{
    cJSON *schema = cJSON_CreateObject();
    cJSON_AddStringToObject(schema, "type", "object");
    if (props_json && props_json[0]) {
        cJSON *props = cJSON_Parse(props_json);
        if (props)
            cJSON_AddItemToObject(schema, "properties", props);
    }
    return schema;
}

// Convenience: get string param or return default
static const char *param_str(cJSON *params, const char *key, const char *def)
{
    cJSON *v = cJSON_GetObjectItem(params, key);
    if (v && cJSON_IsString(v)) return v->valuestring;
    return def;
}

static int param_int(cJSON *params, const char *key, int def)
{
    cJSON *v = cJSON_GetObjectItem(params, key);
    if (v && cJSON_IsNumber(v)) return v->valueint;
    return def;
}

static bool param_bool(cJSON *params, const char *key, bool def)
{
    cJSON *v = cJSON_GetObjectItem(params, key);
    if (v && cJSON_IsBool(v)) return cJSON_IsTrue(v);
    return def;
}

// ---- USB Callbacks ----

static void usb_progress_cb(int progress, void *user_data)
{
    (void)user_data;
    mcp.usb_progress = progress;
    if (progress < 0) {
        mcp.usb_error = true;
        mcp.usb_done = true;
    } else if (progress >= 100) {
        mcp.usb_done = true;
    }
}

static void usb_dirlist_cb(struct usblink_file *f, bool is_error, void *user_data)
{
    (void)user_data;
    if (is_error) {
        mcp.dirlist_error = true;
        mcp.dirlist_done = true;
        return;
    }
    if (!f) {
        // End of listing
        mcp.dirlist_done = true;
        return;
    }
    if (mcp.dirlist_count < MCP_DIRLIST_MAX) {
        struct mcp_dirlist_entry *e = &mcp.dirlist[mcp.dirlist_count++];
        strncpy(e->filename, f->filename, sizeof(e->filename) - 1);
        e->filename[sizeof(e->filename) - 1] = '\0';
        e->size = f->size;
        e->is_dir = f->is_dir;
    }
}

// ---- Tool Implementations ----

static void tool_emulator_status(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "is_running", !exiting);
    cJSON_AddBoolToObject(r, "turbo_mode", turbo_mode);
    cJSON_AddBoolToObject(r, "paused", (cpu_events & EVENT_WAITING) != 0);
    mcp_respond(id, r);
}

static void tool_emulator_pause(int id, cJSON *params)
{
    bool paused = param_bool(params, "paused", true);
    if (paused)
        cpu_events |= EVENT_WAITING;
    else
        cpu_events &= ~EVENT_WAITING;

    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "paused", paused);
    mcp_respond(id, r);
}

static void tool_emulator_set_turbo(int id, cJSON *params)
{
    turbo_mode = param_bool(params, "enabled", true);
    cJSON *r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "turbo_mode", turbo_mode);
    mcp_respond(id, r);
}

// stb_image_write callback to collect PNG bytes
struct png_write_ctx {
    uint8_t *data;
    int size;
    int capacity;
};

static void png_write_func(void *context, void *data, int size)
{
    struct png_write_ctx *ctx = (struct png_write_ctx *)context;
    if (ctx->size + size > ctx->capacity) {
        ctx->capacity = (ctx->size + size) * 2;
        ctx->data = (uint8_t *)realloc(ctx->data, ctx->capacity);
    }
    memcpy(ctx->data + ctx->size, data, size);
    ctx->size += size;
}

static void tool_emulator_screenshot(int id, cJSON *params)
{
    const char *output_path = param_str(params, "output_path", NULL);

    // Capture LCD
    uint16_t *framebuf = (uint16_t *)malloc(320 * 240 * 2);
    if (!framebuf) {
        mcp_respond_error(id, -1, "malloc failed");
        return;
    }
    lcd_cx_draw_frame(framebuf);

    // Convert RGB565 to RGB888
    uint8_t *rgb = (uint8_t *)malloc(320 * 240 * 3);
    if (!rgb) {
        free(framebuf);
        mcp_respond_error(id, -1, "malloc failed");
        return;
    }
    for (int i = 0; i < 320 * 240; i++) {
        uint16_t px = framebuf[i];
        rgb[i*3+0] = ((px >> 11) & 0x1F) * 255 / 31;
        rgb[i*3+1] = ((px >> 5) & 0x3F) * 255 / 63;
        rgb[i*3+2] = (px & 0x1F) * 255 / 31;
    }
    free(framebuf);

    if (output_path) {
        // Write to file
        int ok = stbi_write_png(output_path, 320, 240, 3, rgb, 320 * 3);
        free(rgb);
        if (!ok) {
            mcp_respond_error(id, -1, "Failed to write PNG");
            return;
        }
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "path", output_path);
        cJSON_AddNumberToObject(r, "width", 320);
        cJSON_AddNumberToObject(r, "height", 240);
        mcp_respond(id, r);
    } else {
        // Encode to memory, return base64 in response
        struct png_write_ctx ctx = {NULL, 0, 0};
        stbi_write_png_to_func(png_write_func, &ctx, 320, 240, 3, rgb, 320 * 3);
        free(rgb);

        if (!ctx.data || ctx.size == 0) {
            free(ctx.data);
            mcp_respond_error(id, -1, "PNG encode failed");
            return;
        }

        char *b64 = base64_encode(ctx.data, ctx.size);
        free(ctx.data);

        if (!b64) {
            mcp_respond_error(id, -1, "base64 encode failed");
            return;
        }

        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "png_base64", b64);
        cJSON_AddNumberToObject(r, "width", 320);
        cJSON_AddNumberToObject(r, "height", 240);
        mcp_respond(id, r);
        free(b64);
    }
}

static void tool_emulator_press_key(int id, cJSON *params)
{
    const char *key = param_str(params, "key", NULL);
    int duration_ms = param_int(params, "duration_ms", 50);

    if (!key) {
        mcp_respond_error(id, -32602, "Missing 'key' parameter");
        return;
    }
    if (mcp.pending != MCP_PENDING_NONE) {
        mcp_respond_error(id, -32000, "Another operation is pending");
        return;
    }

    int row, col;
    if (!mcp_lookup_key(key, &row, &col)) {
        mcp_respond_error(id, -32602, "Unknown key name");
        return;
    }

    // Press key
    keypad_set_key(row, col, true);
    if (row == 0 && col == 9) keypad_on_pressed(); // "on" key

    // Set up pending release
    mcp.pending = MCP_PENDING_KEY_PRESS;
    mcp.pending_id = id;
    mcp.key_row = row;
    mcp.key_col = col;
    mcp.key_deadline_us = mcp_time_us() + (uint64_t)duration_ms * 1000;
}

static void tool_emulator_type_text(int id, cJSON *params)
{
    const char *text = param_str(params, "text", NULL);
    int delay_ms = param_int(params, "delay_ms", 50);

    if (!text || !text[0]) {
        mcp_respond_error(id, -32602, "Missing 'text' parameter");
        return;
    }
    if (mcp.pending != MCP_PENDING_NONE) {
        mcp_respond_error(id, -32000, "Another operation is pending");
        return;
    }

    mcp.type_text_alloc = strdup(text);
    mcp.type_text = mcp.type_text_alloc;
    mcp.type_delay_ms = delay_ms;
    mcp.type_next_us = mcp_time_us();
    mcp.type_key_down = false;
    mcp.pending = MCP_PENDING_TYPE_TEXT;
    mcp.pending_id = id;
}

static void tool_emulator_read_memory(int id, cJSON *params)
{
    const char *addr_s = param_str(params, "address", NULL);
    int size = param_int(params, "size", 0);

    if (!addr_s || size <= 0) {
        mcp_respond_error(id, -32602, "Missing 'address' or 'size'");
        return;
    }
    if (size > 4096) {
        mcp_respond_error(id, -32602, "Size exceeds 4096");
        return;
    }

    uint32_t addr = strtoul(addr_s, NULL, 0);
    void *ptr = virt_mem_ptr(addr, size);
    if (!ptr) {
        ptr = phys_mem_ptr(addr, size);
    }
    if (!ptr) {
        mcp_respond_error(id, -32000, "Invalid memory address");
        return;
    }

    // Format as hex string
    char *hex = (char *)malloc(size * 2 + 1);
    for (int i = 0; i < size; i++)
        sprintf(hex + i * 2, "%02x", ((uint8_t *)ptr)[i]);
    hex[size * 2] = '\0';

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "address", addr_s);
    cJSON_AddNumberToObject(r, "size", size);
    cJSON_AddStringToObject(r, "hex", hex);
    mcp_respond(id, r);
    free(hex);
}

static void tool_emulator_write_memory(int id, cJSON *params)
{
    const char *addr_s = param_str(params, "address", NULL);
    const char *data_hex = param_str(params, "data", NULL);

    if (!addr_s || !data_hex) {
        mcp_respond_error(id, -32602, "Missing 'address' or 'data'");
        return;
    }

    int len = strlen(data_hex);
    if (len % 2 != 0 || len == 0) {
        mcp_respond_error(id, -32602, "Data must be even-length hex string");
        return;
    }
    int size = len / 2;

    uint32_t addr = strtoul(addr_s, NULL, 0);
    void *ptr = virt_mem_ptr(addr, size);
    if (!ptr) {
        ptr = phys_mem_ptr(addr, size);
    }
    if (!ptr) {
        mcp_respond_error(id, -32000, "Invalid memory address");
        return;
    }

    // Parse hex
    uint8_t *dest = (uint8_t *)ptr;
    for (int i = 0; i < size; i++) {
        unsigned int byte;
        sscanf(data_hex + i * 2, "%2x", &byte);
        dest[i] = (uint8_t)byte;
    }

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "address", addr_s);
    cJSON_AddNumberToObject(r, "bytes_written", size);
    mcp_respond(id, r);
}

static void tool_emulator_get_registers(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON *regs = cJSON_CreateObject();

    char name[8];
    for (int i = 0; i < 16; i++) {
        snprintf(name, sizeof(name), "r%d", i);
        char val[16];
        snprintf(val, sizeof(val), "0x%08x", arm.reg[i]);
        cJSON_AddStringToObject(regs, name, val);
    }
    char cpsr_s[16];
    snprintf(cpsr_s, sizeof(cpsr_s), "0x%08x", get_cpsr());
    cJSON_AddStringToObject(regs, "cpsr", cpsr_s);

    cJSON_AddItemToObject(r, "registers", regs);
    mcp_respond(id, r);
}

static void tool_emulator_upload_file(int id, cJSON *params)
{
    const char *local = param_str(params, "local_path", NULL);
    const char *remote = param_str(params, "remote_path", NULL);

    if (!local || !remote) {
        mcp_respond_error(id, -32602, "Missing 'local_path' or 'remote_path'");
        return;
    }
    if (mcp.pending != MCP_PENDING_NONE) {
        mcp_respond_error(id, -32000, "Another operation is pending");
        return;
    }

    mcp.usb_progress = 0;
    mcp.usb_done = false;
    mcp.usb_error = false;
    mcp.pending = MCP_PENDING_UPLOAD;
    mcp.pending_id = id;

    usblink_queue_put_file(std::string(local), std::string(remote), usb_progress_cb, NULL);
}

static void tool_emulator_download_file(int id, cJSON *params)
{
    const char *remote = param_str(params, "remote_path", NULL);
    const char *local = param_str(params, "local_path", NULL);

    if (!remote || !local) {
        mcp_respond_error(id, -32602, "Missing 'remote_path' or 'local_path'");
        return;
    }
    if (mcp.pending != MCP_PENDING_NONE) {
        mcp_respond_error(id, -32000, "Another operation is pending");
        return;
    }

    mcp.usb_progress = 0;
    mcp.usb_done = false;
    mcp.usb_error = false;
    mcp.pending = MCP_PENDING_DOWNLOAD;
    mcp.pending_id = id;

    usblink_queue_download(std::string(remote), std::string(local), usb_progress_cb, NULL);
}

static void tool_emulator_list_files(int id, cJSON *params)
{
    const char *path = param_str(params, "path", "/");

    if (mcp.pending != MCP_PENDING_NONE) {
        mcp_respond_error(id, -32000, "Another operation is pending");
        return;
    }

    mcp.dirlist_count = 0;
    mcp.dirlist_done = false;
    mcp.dirlist_error = false;
    mcp.pending = MCP_PENDING_DIRLIST;
    mcp.pending_id = id;

    usblink_queue_dirlist(std::string(path), usb_dirlist_cb, NULL);
}

// ---- Pending Operation Processing ----

static void mcp_process_pending(void)
{
    uint64_t now = mcp_time_us();

    switch (mcp.pending) {
    case MCP_PENDING_NONE:
        break;

    case MCP_PENDING_KEY_PRESS:
        if (now >= mcp.key_deadline_us) {
            keypad_set_key(mcp.key_row, mcp.key_col, false);
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "success", true);
            mcp_respond(mcp.pending_id, r);
            mcp.pending = MCP_PENDING_NONE;
        }
        break;

    case MCP_PENDING_TYPE_TEXT:
    {
        if (now < mcp.type_next_us) break;

        if (mcp.type_key_down) {
            // Release current key
            keypad_set_key(mcp.type_cur_row, mcp.type_cur_col, false);
            mcp.type_key_down = false;
            mcp.type_text++;
            mcp.type_next_us = now + (uint64_t)mcp.type_delay_ms * 500; // half-delay between release and next press
            break;
        }

        if (!mcp.type_text || !*mcp.type_text) {
            // Done typing
            free(mcp.type_text_alloc);
            mcp.type_text_alloc = NULL;
            mcp.type_text = NULL;
            cJSON *r = cJSON_CreateObject();
            cJSON_AddBoolToObject(r, "success", true);
            mcp_respond(mcp.pending_id, r);
            mcp.pending = MCP_PENDING_NONE;
            break;
        }

        // Look up this character
        char keyname[2] = { *mcp.type_text, '\0' };
        int row, col;

        // Handle space specially
        if (*mcp.type_text == ' ') {
            row = 0; col = 4; // space key
        } else if (!mcp_lookup_key(keyname, &row, &col)) {
            // Skip unknown characters
            mcp.type_text++;
            break;
        }

        keypad_set_key(row, col, true);
        mcp.type_key_down = true;
        mcp.type_cur_row = row;
        mcp.type_cur_col = col;
        mcp.type_next_us = now + (uint64_t)mcp.type_delay_ms * 500; // half-delay for press duration
        break;
    }

    case MCP_PENDING_UPLOAD:
    case MCP_PENDING_DOWNLOAD:
        if (mcp.usb_done) {
            if (mcp.usb_error) {
                mcp_respond_error(mcp.pending_id, -32000, "USB file operation failed");
            } else {
                cJSON *r = cJSON_CreateObject();
                cJSON_AddBoolToObject(r, "success", true);
                mcp_respond(mcp.pending_id, r);
            }
            mcp.pending = MCP_PENDING_NONE;
        }
        break;

    case MCP_PENDING_DIRLIST:
        if (mcp.dirlist_done) {
            if (mcp.dirlist_error) {
                mcp_respond_error(mcp.pending_id, -32000, "Directory listing failed");
            } else {
                cJSON *r = cJSON_CreateObject();
                cJSON *files = cJSON_CreateArray();
                for (int i = 0; i < mcp.dirlist_count; i++) {
                    cJSON *entry = cJSON_CreateObject();
                    cJSON_AddStringToObject(entry, "filename", mcp.dirlist[i].filename);
                    cJSON_AddNumberToObject(entry, "size", mcp.dirlist[i].size);
                    cJSON_AddBoolToObject(entry, "is_dir", mcp.dirlist[i].is_dir);
                    cJSON_AddItemToArray(files, entry);
                }
                cJSON_AddItemToObject(r, "files", files);
                mcp_respond(mcp.pending_id, r);
            }
            mcp.pending = MCP_PENDING_NONE;
        }
        break;
    }
}

// ---- MCP Protocol Handlers ----

static void handle_initialize(int id, cJSON *params)
{
    (void)params;
    mcp.initialized = true;

    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "protocolVersion", "2024-11-05");

    cJSON *caps = cJSON_CreateObject();
    cJSON *tools_cap = cJSON_CreateObject();
    cJSON_AddItemToObject(caps, "tools", tools_cap);
    cJSON_AddItemToObject(r, "capabilities", caps);

    cJSON *info = cJSON_CreateObject();
    cJSON_AddStringToObject(info, "name", "firebird-headless");
    cJSON_AddStringToObject(info, "version", "1.0.0");
    cJSON_AddItemToObject(r, "serverInfo", info);

    mcp_respond(id, r);
}

static void handle_initialized(void)
{
    // No-op notification, client confirms init
}

static void handle_tools_list(int id, cJSON *params)
{
    (void)params;
    cJSON *r = cJSON_CreateObject();
    cJSON *tools = cJSON_CreateArray();

    // emulator_status
    cJSON_AddItemToArray(tools, make_tool("emulator_status",
        "Get current emulator status (running, turbo, paused)",
        make_schema("")));

    // emulator_pause
    cJSON_AddItemToArray(tools, make_tool("emulator_pause",
        "Pause or resume emulation",
        make_schema("{\"paused\":{\"type\":\"boolean\",\"description\":\"True to pause, false to resume\"}}")));

    // emulator_set_turbo
    cJSON_AddItemToArray(tools, make_tool("emulator_set_turbo",
        "Enable or disable turbo mode (no speed throttling)",
        make_schema("{\"enabled\":{\"type\":\"boolean\",\"description\":\"True to enable turbo mode\"}}")));

    // emulator_screenshot
    cJSON_AddItemToArray(tools, make_tool("emulator_screenshot",
        "Capture the calculator LCD display to a PNG file",
        make_schema("{\"output_path\":{\"type\":\"string\",\"description\":\"Path to save screenshot (default: temp file)\"}}")));

    // emulator_press_key
    cJSON_AddItemToArray(tools, make_tool("emulator_press_key",
        "Press and release a calculator key",
        make_schema("{\"key\":{\"type\":\"string\",\"description\":\"Key name (e.g., 'enter', '0'-'9', 'a'-'z', 'plus', 'esc')\"},\"duration_ms\":{\"type\":\"integer\",\"description\":\"Duration to hold key in ms (default: 50)\"}}")));

    // emulator_type_text
    cJSON_AddItemToArray(tools, make_tool("emulator_type_text",
        "Type text by simulating keypresses",
        make_schema("{\"text\":{\"type\":\"string\",\"description\":\"Text to type\"},\"delay_ms\":{\"type\":\"integer\",\"description\":\"Delay between keys in ms (default: 50)\"}}")));

    // emulator_read_memory
    cJSON_AddItemToArray(tools, make_tool("emulator_read_memory",
        "Read memory from the emulated calculator",
        make_schema("{\"address\":{\"type\":\"string\",\"description\":\"Hex address (e.g., '0x10000000')\"},\"size\":{\"type\":\"integer\",\"description\":\"Number of bytes to read (max 4096)\"}}")));

    // emulator_write_memory
    cJSON_AddItemToArray(tools, make_tool("emulator_write_memory",
        "Write memory to the emulated calculator",
        make_schema("{\"address\":{\"type\":\"string\",\"description\":\"Hex address\"},\"data\":{\"type\":\"string\",\"description\":\"Hex string of data to write\"}}")));

    // emulator_get_registers
    cJSON_AddItemToArray(tools, make_tool("emulator_get_registers",
        "Get CPU register values",
        make_schema("")));

    // emulator_upload_file
    cJSON_AddItemToArray(tools, make_tool("emulator_upload_file",
        "Upload a file from host to calculator",
        make_schema("{\"local_path\":{\"type\":\"string\",\"description\":\"Path on host machine\"},\"remote_path\":{\"type\":\"string\",\"description\":\"Path on calculator\"}}")));

    // emulator_download_file
    cJSON_AddItemToArray(tools, make_tool("emulator_download_file",
        "Download a file from calculator to host",
        make_schema("{\"remote_path\":{\"type\":\"string\",\"description\":\"Path on calculator\"},\"local_path\":{\"type\":\"string\",\"description\":\"Path on host machine\"}}")));

    // emulator_list_files
    cJSON_AddItemToArray(tools, make_tool("emulator_list_files",
        "List files in a directory on the calculator",
        make_schema("{\"path\":{\"type\":\"string\",\"description\":\"Directory path on calculator (e.g., '/documents')\"}}")));

    cJSON_AddItemToObject(r, "tools", tools);
    mcp_respond(id, r);
}

// Unified tool dispatch — returns true if tool was found
static bool dispatch_tool(int id, const char *name, cJSON *args)
{
    if (strcmp(name, "emulator_status") == 0)
        tool_emulator_status(id, args);
    else if (strcmp(name, "emulator_pause") == 0)
        tool_emulator_pause(id, args);
    else if (strcmp(name, "emulator_set_turbo") == 0)
        tool_emulator_set_turbo(id, args);
    else if (strcmp(name, "emulator_screenshot") == 0)
        tool_emulator_screenshot(id, args);
    else if (strcmp(name, "emulator_press_key") == 0)
        tool_emulator_press_key(id, args);
    else if (strcmp(name, "emulator_type_text") == 0)
        tool_emulator_type_text(id, args);
    else if (strcmp(name, "emulator_read_memory") == 0)
        tool_emulator_read_memory(id, args);
    else if (strcmp(name, "emulator_write_memory") == 0)
        tool_emulator_write_memory(id, args);
    else if (strcmp(name, "emulator_get_registers") == 0)
        tool_emulator_get_registers(id, args);
    else if (strcmp(name, "emulator_upload_file") == 0)
        tool_emulator_upload_file(id, args);
    else if (strcmp(name, "emulator_download_file") == 0)
        tool_emulator_download_file(id, args);
    else if (strcmp(name, "emulator_list_files") == 0)
        tool_emulator_list_files(id, args);
    else
        return false;
    return true;
}

static void handle_tools_call(int id, cJSON *params)
{
    const char *name = param_str(params, "name", NULL);
    cJSON *arguments = cJSON_GetObjectItem(params, "arguments");

    if (!name) {
        mcp_respond_error(id, -32602, "Missing tool name");
        return;
    }
    if (!dispatch_tool(id, name, arguments))
        mcp_respond_error(id, -32601, "Unknown tool");
}

// ---- MCP Message Router ----

static void mcp_handle_message(const char *line)
{
    cJSON *msg = cJSON_Parse(line);
    if (!msg) {
        fprintf(debug_out, "[MCP] Invalid JSON: %.100s...\n", line);
        return;
    }

    const char *method = NULL;
    cJSON *method_item = cJSON_GetObjectItem(msg, "method");
    if (method_item && cJSON_IsString(method_item))
        method = method_item->valuestring;

    cJSON *id_item = cJSON_GetObjectItem(msg, "id");
    int id = -1;
    if (id_item && cJSON_IsNumber(id_item))
        id = id_item->valueint;

    cJSON *params = cJSON_GetObjectItem(msg, "params");

    if (!method) {
        // Not a request or notification
        cJSON_Delete(msg);
        return;
    }

    if (strcmp(method, "initialize") == 0) {
        handle_initialize(id, params);
    } else if (strcmp(method, "notifications/initialized") == 0) {
        handle_initialized();
    } else if (strcmp(method, "tools/list") == 0) {
        handle_tools_list(id, params);
    } else if (strcmp(method, "tools/call") == 0) {
        handle_tools_call(id, params);
    } else if (strcmp(method, "ping") == 0) {
        cJSON *r = cJSON_CreateObject();
        mcp_respond(id, r);
    } else {
        // Try direct tool dispatch (method name = tool name)
        if (!dispatch_tool(id, method, params)) {
            if (id >= 0)
                mcp_respond_error(id, -32601, "Method not found");
        }
    }

    cJSON_Delete(msg);
}

// ---- Init & Poll ----

void mcp_init(void)
{
    memset(&mcp, 0, sizeof(mcp));
    mcp.enabled = true;
    mcp.tcp_port = 0;
    mcp.listen_fd = -1;
    mcp.client_fd = -1;
    debug_out = stderr;

    // Set stdin non-blocking
    int flags = fcntl(STDIN_FILENO, F_GETFL, 0);
    fcntl(STDIN_FILENO, F_SETFL, flags | O_NONBLOCK);

    // Ensure stdout is line-buffered for JSON-RPC
    setvbuf(stdout, NULL, _IOLBF, 0);

    fprintf(debug_out, "[MCP] Headless MCP server initialized (stdin mode)\n");
}

void mcp_init_tcp(int port)
{
    memset(&mcp, 0, sizeof(mcp));
    mcp.enabled = true;
    mcp.initialized = true; // skip MCP handshake for TCP mode
    mcp.tcp_port = port;
    mcp.client_fd = -1;
    debug_out = stderr;

    // Ignore SIGPIPE so writes to closed sockets don't kill us
    signal(SIGPIPE, SIG_IGN);

    // Create listening socket
    mcp.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (mcp.listen_fd < 0) {
        fprintf(debug_out, "[MCP] Failed to create TCP socket: %s\n", strerror(errno));
        return;
    }

    int opt = 1;
    setsockopt(mcp.listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(mcp.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(debug_out, "[MCP] Failed to bind port %d: %s\n", port, strerror(errno));
        close(mcp.listen_fd);
        mcp.listen_fd = -1;
        return;
    }

    if (listen(mcp.listen_fd, 2) < 0) {
        fprintf(debug_out, "[MCP] Failed to listen: %s\n", strerror(errno));
        close(mcp.listen_fd);
        mcp.listen_fd = -1;
        return;
    }

    // Set non-blocking
    int flags = fcntl(mcp.listen_fd, F_GETFL, 0);
    fcntl(mcp.listen_fd, F_SETFL, flags | O_NONBLOCK);

    fprintf(debug_out, "[MCP] TCP server listening on 0.0.0.0:%d\n", port);
}

static void mcp_tcp_disconnect_client(void)
{
    if (mcp.client_fd >= 0) {
        close(mcp.client_fd);
        mcp.client_fd = -1;
        mcp.line_pos = 0;

        // Cancel any pending operation (client won't receive response)
        if (mcp.pending != MCP_PENDING_NONE) {
            // Clean up pending key press
            if (mcp.pending == MCP_PENDING_KEY_PRESS)
                keypad_set_key(mcp.key_row, mcp.key_col, false);
            // Clean up pending type
            if (mcp.pending == MCP_PENDING_TYPE_TEXT) {
                if (mcp.type_key_down)
                    keypad_set_key(mcp.type_cur_row, mcp.type_cur_col, false);
                free(mcp.type_text_alloc);
                mcp.type_text_alloc = NULL;
                mcp.type_text = NULL;
            }
            mcp.pending = MCP_PENDING_NONE;
            fprintf(debug_out, "[MCP] Client disconnected, cancelled pending op\n");
        }
    }
}

static void mcp_poll_tcp(void)
{
    // Accept new connection if none active
    if (mcp.client_fd < 0 && mcp.listen_fd >= 0) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int fd = accept(mcp.listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (fd >= 0) {
            // Set non-blocking
            int flags = fcntl(fd, F_GETFL, 0);
            fcntl(fd, F_SETFL, flags | O_NONBLOCK);
            mcp.client_fd = fd;
            mcp.line_pos = 0;
            fprintf(debug_out, "[MCP] Client connected from %s:%d\n",
                    inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
        }
    }

    // Read from active client
    if (mcp.client_fd < 0) return;

    for (;;) {
        struct pollfd pfd = { mcp.client_fd, POLLIN, 0 };
        int ret = poll(&pfd, 1, 0);
        if (ret <= 0) break;
        if (!(pfd.revents & POLLIN)) break;

        char buf[4096];
        ssize_t n = read(mcp.client_fd, buf, sizeof(buf));
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                fprintf(debug_out, "[MCP] Client disconnected\n");
                mcp_tcp_disconnect_client();
            }
            break;
        }

        // Append to line buffer, process complete lines
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                mcp.line_buf[mcp.line_pos] = '\0';
                if (mcp.line_pos > 0)
                    mcp_handle_message(mcp.line_buf);
                mcp.line_pos = 0;
            } else if (mcp.line_pos < MCP_LINE_BUF_SIZE - 1) {
                mcp.line_buf[mcp.line_pos++] = buf[i];
            }
        }
    }
}

static void mcp_poll_stdin(void)
{
    for (;;) {
        struct pollfd pfd = { STDIN_FILENO, POLLIN, 0 };
        int ret = poll(&pfd, 1, 0);
        if (ret <= 0) break;
        if (!(pfd.revents & POLLIN)) break;

        char buf[4096];
        ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n <= 0) {
            if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) {
                fprintf(debug_out, "[MCP] stdin closed, exiting\n");
                exiting = true;
            }
            break;
        }

        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                mcp.line_buf[mcp.line_pos] = '\0';
                if (mcp.line_pos > 0)
                    mcp_handle_message(mcp.line_buf);
                mcp.line_pos = 0;
            } else if (mcp.line_pos < MCP_LINE_BUF_SIZE - 1) {
                mcp.line_buf[mcp.line_pos++] = buf[i];
            }
        }
    }
}

void mcp_poll(void)
{
    if (!mcp.enabled) return;

    mcp_process_pending();

    if (mcp.tcp_port > 0)
        mcp_poll_tcp();
    else
        mcp_poll_stdin();
}

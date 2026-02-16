// Headless MCP server for Firebird TI-Nspire emulator
// Single-threaded, polled from gui_do_stuff()
#ifndef MCP_HEADLESS_H
#define MCP_HEADLESS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pending operation types
enum mcp_pending_type {
    MCP_PENDING_NONE = 0,
    MCP_PENDING_KEY_PRESS,    // key down, waiting for release deadline
    MCP_PENDING_TYPE_TEXT,    // typing characters one by one
    MCP_PENDING_UPLOAD,       // waiting for usblink upload callback
    MCP_PENDING_DOWNLOAD,     // waiting for usblink download callback
    MCP_PENDING_DIRLIST,      // waiting for usblink dirlist callback
};

// Max line buffer for JSON-RPC
#define MCP_LINE_BUF_SIZE 65536

// Default TCP port
#define MCP_DEFAULT_PORT 3334

// Max dirlist entries
#define MCP_DIRLIST_MAX 512

struct mcp_dirlist_entry {
    char filename[256];
    uint32_t size;
    bool is_dir;
};

struct mcp_state {
    bool enabled;
    bool initialized;

    // Transport mode
    int tcp_port;       // 0 = stdin mode, >0 = TCP mode
    int listen_fd;      // TCP listening socket (-1 if not TCP)
    int client_fd;      // Current TCP client (-1 if none)

    // Line buffer
    char line_buf[MCP_LINE_BUF_SIZE];
    int line_pos;

    // Pending operation
    enum mcp_pending_type pending;
    int pending_id;  // JSON-RPC id for response

    // Key press state
    int key_row, key_col;
    uint64_t key_deadline_us;

    // Type text state
    const char *type_text;    // current position in text (malloc'd copy)
    char *type_text_alloc;    // pointer to free
    int type_delay_ms;
    uint64_t type_next_us;
    bool type_key_down;       // true = key currently pressed, waiting to release
    int type_cur_row, type_cur_col;

    // USB file op state
    int usb_progress;        // last progress value from callback
    bool usb_done;
    bool usb_error;

    // Dirlist state
    struct mcp_dirlist_entry dirlist[MCP_DIRLIST_MAX];
    int dirlist_count;
    bool dirlist_done;
    bool dirlist_error;
};

extern struct mcp_state mcp;

// Initialize MCP in stdin mode (JSON-RPC over stdin/stdout)
void mcp_init(void);

// Initialize MCP in TCP mode (JSON-RPC over TCP socket)
void mcp_init_tcp(int port);

// Poll for incoming JSON-RPC, process pending ops
// Called from gui_do_stuff()
void mcp_poll(void);

// Get monotonic time in microseconds
uint64_t mcp_time_us(void);

#ifdef __cplusplus
}
#endif

#endif // MCP_HEADLESS_H

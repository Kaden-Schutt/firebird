// Key name to (row, col) mapping for TI-Nspire CX keypad
// Used by headless MCP server
#ifndef MCP_KEYMAP_H
#define MCP_KEYMAP_H

#include <string.h>

struct KeyPos { int row; int col; };

// Returns true if key found, fills row/col
static inline bool mcp_lookup_key(const char *name, int *row, int *col)
{
    struct { const char *name; int row; int col; } static const keymap[] = {
        // Numbers
        {"0",           0, 7},
        {"1",           1, 7},
        {"2",           6, 4},
        {"3",           1, 3},
        {"4",           2, 7},
        {"5",           5, 6},
        {"6",           2, 3},
        {"7",           3, 7},
        {"8",           6, 6},
        {"9",           3, 3},

        // Letters
        {"a",           4, 6},
        {"b",           4, 5},
        {"c",           4, 4},
        {"d",           4, 2},
        {"e",           4, 1},
        {"f",           4, 0},
        {"g",           3, 6},
        {"h",           3, 5},
        {"i",           3, 4},
        {"j",           3, 2},
        {"k",           3, 1},
        {"l",           3, 0},
        {"m",           2, 6},
        {"n",           2, 5},
        {"o",           2, 4},
        {"p",           2, 2},
        {"q",           2, 1},
        {"r",           2, 0},
        {"s",           1, 6},
        {"t",           1, 5},
        {"u",           1, 4},
        {"v",           1, 2},
        {"w",           1, 1},
        {"x",           1, 0},
        {"y",           0, 6},
        {"z",           0, 5},

        // Operators
        {"plus",        6, 2},
        {"+",           6, 2},
        {"minus",       5, 2},
        {"-",           5, 2},
        {"multiply",    4, 8},
        {"mult",        4, 8},
        {"*",           4, 8},
        {"divide",      3, 8},
        {"div",         3, 8},
        {"/",           3, 8},
        {"equals",      4, 7},
        {"=",           4, 7},
        {"power",       4, 9},
        {"pow",         4, 9},
        {"^",           3, 9},
        {"exponent",    3, 9},
        {"exp",         3, 9},
        {"10x",         1, 10},
        {"pow10",       1, 10},

        // Control
        {"enter",       0, 1},
        {"ret",         0, 0},
        {"esc",         6, 7},
        {"escape",      6, 7},
        {"tab",         6, 9},
        {"del",         5, 9},
        {"delete",      5, 9},
        {"backspace",   5, 9},
        {"space",       0, 4},
        {"on",          0, 9},

        // Navigation/UI
        {"leftparen",   5, 5},
        {"(",           5, 5},
        {"rightparen",  5, 3},
        {")",           5, 3},
        {"comma",       7, 10},
        {",",           7, 10},
        {"dot",         5, 4},
        {"decimal",     5, 4},
        {"neg",         0, 3},
        {"negative",    0, 3},
        {"punct",       0, 8},
        {"menu",        6, 5},
        {"doc",         6, 3},
        {"documents",   6, 3},
        {"var",         5, 1},
        {"cat",         5, 7},
        {"catalog",     5, 7},
        {"matrix",      5, 8},
        {"pad",         5, 10},
        {"scratchpad",  5, 10},
        {"flag",        6, 0},

        // Math
        {"pi",          1, 8},
        {"ee",          2, 8},
        {"square",      2, 9},
        {"squ",         2, 9},
        {"trig",        1, 9},

        // Modifiers
        {"shift",       7, 8},
        {"ctrl",        7, 9},
        {"control",     7, 9},
    };

    for (size_t i = 0; i < sizeof(keymap) / sizeof(keymap[0]); i++) {
        if (strcmp(name, keymap[i].name) == 0) {
            *row = keymap[i].row;
            *col = keymap[i].col;
            return true;
        }
    }
    return false;
}

#endif // MCP_KEYMAP_H

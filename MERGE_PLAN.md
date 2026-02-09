# Firebird MCP Server Merge Plan

## Overview
Merge 3 tools from MacBook version into k9lin version (which is the superset).
k9lin version is on branch `mcp-merge` with commit 676790d as base.

## What k9lin already has that MacBook doesn't
- emulator_arrow (direct keypad struct manipulation)
- emulator_wait_stable (screen change detection)
- emulator_input_expr (math expression input with exponent handling)
- emulator_search_memory (hex pattern scanning)
- emulator_read_string (null-terminated string from memory)
- emulator_run_until (breakpoint + resume + capture)
- emulator_trace (execution trace log)
- emulator_find_functions (ARM/Thumb prologue detection)
- emulator_deploy_and_run (one-shot build-test)
- Thread-local capture buffer (beginCapture/endCapture in emuthread.cpp/h)
- pressKeyAndWaitForTransition / waitForStableScreen / simulateArrow helpers

## Tools to port FROM MacBook → k9lin

### 1. emulator_touchpad
Raw touchpad x/y/contact/down control. Different from emulator_arrow (which is directional).

### 2. emulator_get_screen_info
Returns screen dimensions, pause state, turbo mode.

### 3. emulator_run_macro
Sequence executor: array of operations (key, nav, wait, type). This is valuable
because it composes with existing tools and reduces round-trips.

## Implementation Instructions

### In mcpserver.h, add these 3 method declarations in the "Tool implementations" section:
```cpp
QJsonObject toolEmulatorTouchpad(const QJsonObject &args);
QJsonObject toolEmulatorGetScreenInfo();
QJsonObject toolEmulatorRunMacro(const QJsonObject &args);
```

### In mcpserver.cpp handleToolsList(), add these tool registrations:

#### emulator_touchpad (add near other input tools):
```cpp
// Raw touchpad control
{
    QJsonObject props;
    QJsonObject xProp;
    xProp[QStringLiteral("type")] = QStringLiteral("number");
    xProp[QStringLiteral("description")] = QStringLiteral("X position (0.0-1.0)");
    props[QStringLiteral("x")] = xProp;
    QJsonObject yProp;
    yProp[QStringLiteral("type")] = QStringLiteral("number");
    yProp[QStringLiteral("description")] = QStringLiteral("Y position (0.0-1.0)");
    props[QStringLiteral("y")] = yProp;
    QJsonObject contactProp;
    contactProp[QStringLiteral("type")] = QStringLiteral("boolean");
    contactProp[QStringLiteral("description")] = QStringLiteral("Finger touching touchpad");
    props[QStringLiteral("contact")] = contactProp;
    QJsonObject downProp;
    downProp[QStringLiteral("type")] = QStringLiteral("boolean");
    downProp[QStringLiteral("description")] = QStringLiteral("Touchpad clicked/pressed");
    props[QStringLiteral("down")] = downProp;
    QJsonArray required;
    required.append(QStringLiteral("x"));
    required.append(QStringLiteral("y"));
    required.append(QStringLiteral("contact"));
    required.append(QStringLiteral("down"));
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");
    schema[QStringLiteral("properties")] = props;
    schema[QStringLiteral("required")] = required;
    addTool(QStringLiteral("emulator_touchpad"), QStringLiteral("Raw touchpad control (x/y position, contact, press). For directional nav, prefer emulator_arrow."), schema);
}
```

#### emulator_get_screen_info:
```cpp
addTool(QStringLiteral("emulator_get_screen_info"), QStringLiteral("Get screen dimensions and basic state info"), emptySchema);
```

#### emulator_run_macro:
```cpp
{
    QJsonObject props;
    QJsonObject opsProp;
    opsProp[QStringLiteral("type")] = QStringLiteral("array");
    opsProp[QStringLiteral("description")] = QStringLiteral("Array of operations: [{\"op\":\"key\",\"key\":\"enter\"}, {\"op\":\"nav\",\"dir\":\"down\"}, {\"op\":\"wait\",\"ms\":100}, {\"op\":\"type\",\"text\":\"hello\"}]");
    props[QStringLiteral("operations")] = opsProp;
    QJsonArray required;
    required.append(QStringLiteral("operations"));
    QJsonObject schema;
    schema[QStringLiteral("type")] = QStringLiteral("object");
    schema[QStringLiteral("properties")] = props;
    schema[QStringLiteral("required")] = required;
    addTool(QStringLiteral("emulator_run_macro"), QStringLiteral("Run a sequence of operations (key presses, arrow navigation, waits, text input). Reduces round-trips for multi-step interactions."), schema);
}
```

### In handleToolsCall(), add the dispatch cases:
```cpp
} else if (name == QStringLiteral("emulator_touchpad")) {
    result = toolEmulatorTouchpad(args);
} else if (name == QStringLiteral("emulator_get_screen_info")) {
    result = toolEmulatorGetScreenInfo();
} else if (name == QStringLiteral("emulator_run_macro")) {
    result = toolEmulatorRunMacro(args);
```

### Tool implementations to add to mcpserver.cpp:

```cpp
QJsonObject MCPServer::toolEmulatorTouchpad(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    double x = args.value(QStringLiteral("x")).toDouble();
    double y = args.value(QStringLiteral("y")).toDouble();
    bool contact = args.value(QStringLiteral("contact")).toBool();
    bool down = args.value(QStringLiteral("down")).toBool();

    keypad.touchpad_x = static_cast<uint16_t>(x * TOUCHPAD_X_MAX);
    keypad.touchpad_y = static_cast<uint16_t>(y * TOUCHPAD_Y_MAX);
    keypad.touchpad_contact = contact;
    keypad.touchpad_down = down;
    keypad.kpc.gpio_int_active |= 0x800;
    keypad_int_check();

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("x")] = x;
    result[QStringLiteral("y")] = y;
    result[QStringLiteral("contact")] = contact;
    result[QStringLiteral("down")] = down;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorGetScreenInfo()
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QJsonObject result;
    result[QStringLiteral("running")] = true;
    result[QStringLiteral("width")] = 320;
    result[QStringLiteral("height")] = 240;
    result[QStringLiteral("paused")] = emu_thread.isPaused();
    result[QStringLiteral("turbo")] = turbo_mode;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorRunMacro(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QJsonArray operations = args.value(QStringLiteral("operations")).toArray();
    if (operations.isEmpty()) {
        return makeToolError(QStringLiteral("Operations array is required"));
    }

    int executed = 0;
    for (const QJsonValue &opVal : operations) {
        QJsonObject op = opVal.toObject();
        QString opType = op.value(QStringLiteral("op")).toString().toLower();

        if (opType == QStringLiteral("key")) {
            QString key = op.value(QStringLiteral("key")).toString();
            int duration = op.value(QStringLiteral("duration")).toInt(50);
            QJsonObject keyArgs;
            keyArgs[QStringLiteral("key")] = key;
            keyArgs[QStringLiteral("duration_ms")] = duration;
            toolEmulatorPressKey(keyArgs);

        } else if (opType == QStringLiteral("nav") || opType == QStringLiteral("arrow")) {
            QString dir = op.value(QStringLiteral("dir")).toString();
            // Use the k9lin arrow implementation (direct keypad struct)
            simulateArrow(dir);

        } else if (opType == QStringLiteral("wait")) {
            int ms = op.value(QStringLiteral("ms")).toInt(100);
            QThread::msleep(ms);

        } else if (opType == QStringLiteral("type")) {
            QString text = op.value(QStringLiteral("text")).toString();
            QJsonObject typeArgs;
            typeArgs[QStringLiteral("text")] = text;
            toolEmulatorTypeText(typeArgs);

        } else if (opType == QStringLiteral("wait_stable")) {
            int timeout = op.value(QStringLiteral("timeout_ms")).toInt(5000);
            int stable = op.value(QStringLiteral("stable_ms")).toInt(300);
            waitForStableScreen(timeout, stable, 50);
        }

        executed++;
        QThread::msleep(30);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("executed")] = executed;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}
```

## IMPORTANT: run_macro enhancement
The k9lin version's run_macro adds `wait_stable` as a macro op type, which the MacBook
version didn't have. This lets macros include "wait for screen to settle" steps inline,
which is critical for reliability. The `nav`/`arrow` op uses k9lin's `simulateArrow()`
(direct keypad struct) instead of MacBook's broken touchpad approach.

## After merge, rebuild:
```bash
cd /home/kaden/ClaudeCode/nspire/firebird
make -j$(nproc) 2>&1 | tail -20
```

## Then commit:
```bash
git add -A mcp/ && git commit -m "Merge MacBook tools: touchpad, get_screen_info, run_macro"
```

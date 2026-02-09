#include "mcpserver.h"
#include "stdioreader.h"
#include "keymap_mcp.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QImage>
#include <QDir>
#include <QThread>
#include <QMetaObject>

#include <iostream>
#include <cstring>
#include <algorithm>

#include "emuthread.h"
#include "qmlbridge.h"
#include "qtframebuffer.h"
#include "core/keypad.h"
#include "core/debug.h"
#include "core/cpu.h"
#include "core/mem.h"
#include "core/disasm.h"
#include "core/mmu.h"
#include "core/usblink_queue.h"

MCPServer *the_mcp_server = nullptr;

MCPServer::MCPServer(QObject *parent)
    : QObject(parent)
    , m_reader(new StdioReader(this))
    , m_initialized(false)
    , m_asyncComplete(false)
    , m_asyncProgress(0)
{
    the_mcp_server = this;

    connect(m_reader, &StdioReader::lineReceived,
            this, &MCPServer::handleLine, Qt::QueuedConnection);
}

MCPServer::~MCPServer()
{
    stop();
    the_mcp_server = nullptr;
}

void MCPServer::start()
{
    m_reader->start();
}

void MCPServer::stop()
{
    m_reader->stop();
    m_reader->wait();
}

void MCPServer::sendResponse(const QJsonObject &response)
{
    QJsonDocument doc(response);
    std::cout << doc.toJson(QJsonDocument::Compact).toStdString() << std::endl;
    std::cout.flush();
}

void MCPServer::sendError(const QJsonValue &id, int code, const QString &message)
{
    QJsonObject error;
    error[QStringLiteral("code")] = code;
    error[QStringLiteral("message")] = message;

    QJsonObject response;
    response[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    response[QStringLiteral("id")] = id;
    response[QStringLiteral("error")] = error;

    sendResponse(response);
}

void MCPServer::sendResult(const QJsonValue &id, const QJsonObject &result)
{
    QJsonObject response;
    response[QStringLiteral("jsonrpc")] = QStringLiteral("2.0");
    response[QStringLiteral("id")] = id;
    response[QStringLiteral("result")] = result;

    sendResponse(response);
}

QJsonObject MCPServer::makeToolResult(const QString &text)
{
    QJsonObject textContent;
    textContent[QStringLiteral("type")] = QStringLiteral("text");
    textContent[QStringLiteral("text")] = text;

    QJsonArray content;
    content.append(textContent);

    QJsonObject result;
    result[QStringLiteral("content")] = content;
    return result;
}

QJsonObject MCPServer::makeToolError(const QString &text)
{
    QJsonObject textContent;
    textContent[QStringLiteral("type")] = QStringLiteral("text");
    textContent[QStringLiteral("text")] = text;

    QJsonArray content;
    content.append(textContent);

    QJsonObject result;
    result[QStringLiteral("content")] = content;
    result[QStringLiteral("isError")] = true;
    return result;
}

void MCPServer::handleLine(const QString &line)
{
    QJsonParseError parseError;
    QJsonDocument doc = QJsonDocument::fromJson(line.toUtf8(), &parseError);

    if (parseError.error != QJsonParseError::NoError) {
        sendError(QJsonValue::Null, -32700, QStringLiteral("Parse error: ") + parseError.errorString());
        return;
    }

    if (!doc.isObject()) {
        sendError(QJsonValue::Null, -32600, QStringLiteral("Invalid Request: not an object"));
        return;
    }

    QJsonObject request = doc.object();
    QJsonValue id = request.value(QStringLiteral("id"));
    QString method = request.value(QStringLiteral("method")).toString();
    QJsonObject params = request.value(QStringLiteral("params")).toObject();

    if (method == QStringLiteral("initialize")) {
        handleInitialize(id, params);
    } else if (method == QStringLiteral("notifications/initialized") || method == QStringLiteral("initialized")) {
        handleInitialized(id);
    } else if (method == QStringLiteral("tools/list")) {
        handleToolsList(id);
    } else if (method == QStringLiteral("tools/call")) {
        handleToolsCall(id, params);
    } else {
        sendError(id, -32601, QStringLiteral("Method not found: ") + method);
    }
}

void MCPServer::handleInitialize(const QJsonValue &id, const QJsonObject &params)
{
    Q_UNUSED(params);

    QJsonObject serverInfo;
    serverInfo[QStringLiteral("name")] = QStringLiteral("firebird-mcp");
    serverInfo[QStringLiteral("version")] = QStringLiteral("1.0.0");

    QJsonObject toolsCapability;
    QJsonObject capabilities;
    capabilities[QStringLiteral("tools")] = toolsCapability;

    QJsonObject result;
    result[QStringLiteral("protocolVersion")] = QStringLiteral("2024-11-05");
    result[QStringLiteral("serverInfo")] = serverInfo;
    result[QStringLiteral("capabilities")] = capabilities;

    sendResult(id, result);
    m_initialized = true;
}

void MCPServer::handleInitialized(const QJsonValue &id)
{
    Q_UNUSED(id);
    // No response needed for notification
}

void MCPServer::handleToolsList(const QJsonValue &id)
{
    QJsonArray tools;

    // Helper lambda to create tool definitions
    auto addTool = [&tools](const QString &name, const QString &description, const QJsonObject &inputSchema) {
        QJsonObject tool;
        tool[QStringLiteral("name")] = name;
        tool[QStringLiteral("description")] = description;
        tool[QStringLiteral("inputSchema")] = inputSchema;
        tools.append(tool);
    };

    QJsonObject emptySchema;
    emptySchema[QStringLiteral("type")] = QStringLiteral("object");
    emptySchema[QStringLiteral("properties")] = QJsonObject();

    // Emulator control tools
    addTool(QStringLiteral("emulator_status"), QStringLiteral("Get current emulator status (running, paused, speed)"), emptySchema);

    {
        QJsonObject props;
        QJsonObject snapProp;
        snapProp[QStringLiteral("type")] = QStringLiteral("string");
        snapProp[QStringLiteral("description")] = QStringLiteral("Path to snapshot file to load");
        props[QStringLiteral("snapshot_path")] = snapProp;
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        addTool(QStringLiteral("emulator_start"), QStringLiteral("Start the emulator (optionally from a snapshot)"), schema);
    }

    addTool(QStringLiteral("emulator_stop"), QStringLiteral("Stop the emulator"), emptySchema);
    addTool(QStringLiteral("emulator_reset"), QStringLiteral("Reset the emulator CPU"), emptySchema);

    {
        QJsonObject props;
        QJsonObject pausedProp;
        pausedProp[QStringLiteral("type")] = QStringLiteral("boolean");
        pausedProp[QStringLiteral("description")] = QStringLiteral("True to pause, false to resume");
        props[QStringLiteral("paused")] = pausedProp;
        QJsonArray required;
        required.append(QStringLiteral("paused"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_pause"), QStringLiteral("Pause or resume emulation"), schema);
    }

    {
        QJsonObject props;
        QJsonObject enabledProp;
        enabledProp[QStringLiteral("type")] = QStringLiteral("boolean");
        enabledProp[QStringLiteral("description")] = QStringLiteral("True to enable turbo mode");
        props[QStringLiteral("enabled")] = enabledProp;
        QJsonArray required;
        required.append(QStringLiteral("enabled"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_set_turbo"), QStringLiteral("Enable or disable turbo mode (no speed throttling)"), schema);
    }

    {
        QJsonObject props;
        QJsonObject pathProp;
        pathProp[QStringLiteral("type")] = QStringLiteral("string");
        pathProp[QStringLiteral("description")] = QStringLiteral("Path to save screenshot (default: temp file)");
        props[QStringLiteral("output_path")] = pathProp;
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        addTool(QStringLiteral("emulator_screenshot"), QStringLiteral("Capture the calculator LCD display to a PNG file"), schema);
    }

    {
        QJsonObject props;
        QJsonObject keyProp;
        keyProp[QStringLiteral("type")] = QStringLiteral("string");
        keyProp[QStringLiteral("description")] = QStringLiteral("Key name (e.g., 'enter', '0'-'9', 'a'-'z', 'plus', 'esc')");
        props[QStringLiteral("key")] = keyProp;
        QJsonObject durProp;
        durProp[QStringLiteral("type")] = QStringLiteral("integer");
        durProp[QStringLiteral("description")] = QStringLiteral("Duration to hold key in ms (default: 50)");
        props[QStringLiteral("duration_ms")] = durProp;
        QJsonArray required;
        required.append(QStringLiteral("key"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_press_key"), QStringLiteral("Press and release a calculator key"), schema);
    }

    {
        QJsonObject props;
        QJsonObject keyProp;
        keyProp[QStringLiteral("type")] = QStringLiteral("string");
        keyProp[QStringLiteral("description")] = QStringLiteral("Key name");
        props[QStringLiteral("key")] = keyProp;
        QJsonArray required;
        required.append(QStringLiteral("key"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_key_down"), QStringLiteral("Hold a key down (for key combinations)"), schema);
    }

    {
        QJsonObject props;
        QJsonObject keyProp;
        keyProp[QStringLiteral("type")] = QStringLiteral("string");
        keyProp[QStringLiteral("description")] = QStringLiteral("Key name");
        props[QStringLiteral("key")] = keyProp;
        QJsonArray required;
        required.append(QStringLiteral("key"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_key_up"), QStringLiteral("Release a held key"), schema);
    }

    {
        QJsonObject props;
        QJsonObject textProp;
        textProp[QStringLiteral("type")] = QStringLiteral("string");
        textProp[QStringLiteral("description")] = QStringLiteral("Text to type");
        props[QStringLiteral("text")] = textProp;
        QJsonObject delayProp;
        delayProp[QStringLiteral("type")] = QStringLiteral("integer");
        delayProp[QStringLiteral("description")] = QStringLiteral("Delay between keys in ms (default: 50)");
        props[QStringLiteral("delay_ms")] = delayProp;
        QJsonArray required;
        required.append(QStringLiteral("text"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_type_text"), QStringLiteral("Type a string of text by simulating keypresses"), schema);
    }

    {
        QJsonObject props;
        QJsonObject pathProp;
        pathProp[QStringLiteral("type")] = QStringLiteral("string");
        pathProp[QStringLiteral("description")] = QStringLiteral("Directory path on calculator (e.g., '/documents')");
        props[QStringLiteral("path")] = pathProp;
        QJsonArray required;
        required.append(QStringLiteral("path"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_list_files"), QStringLiteral("List files in a directory on the calculator"), schema);
    }

    {
        QJsonObject props;
        QJsonObject localProp;
        localProp[QStringLiteral("type")] = QStringLiteral("string");
        localProp[QStringLiteral("description")] = QStringLiteral("Path on host machine");
        props[QStringLiteral("local_path")] = localProp;
        QJsonObject remoteProp;
        remoteProp[QStringLiteral("type")] = QStringLiteral("string");
        remoteProp[QStringLiteral("description")] = QStringLiteral("Path on calculator");
        props[QStringLiteral("remote_path")] = remoteProp;
        QJsonArray required;
        required.append(QStringLiteral("local_path"));
        required.append(QStringLiteral("remote_path"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_upload_file"), QStringLiteral("Upload a file from host to calculator"), schema);
    }

    {
        QJsonObject props;
        QJsonObject pathProp;
        pathProp[QStringLiteral("type")] = QStringLiteral("string");
        pathProp[QStringLiteral("description")] = QStringLiteral("Path to OS file (.tco/.tcc/.tnc) on host");
        props[QStringLiteral("os_path")] = pathProp;
        QJsonArray required;
        required.append(QStringLiteral("os_path"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_send_os"), QStringLiteral("Send OS file via USB link (triggers Boot2 file parsing)"), schema);
    }

    {
        QJsonObject props;
        QJsonObject remoteProp;
        remoteProp[QStringLiteral("type")] = QStringLiteral("string");
        remoteProp[QStringLiteral("description")] = QStringLiteral("Path on calculator");
        props[QStringLiteral("remote_path")] = remoteProp;
        QJsonObject localProp;
        localProp[QStringLiteral("type")] = QStringLiteral("string");
        localProp[QStringLiteral("description")] = QStringLiteral("Path on host machine");
        props[QStringLiteral("local_path")] = localProp;
        QJsonArray required;
        required.append(QStringLiteral("remote_path"));
        required.append(QStringLiteral("local_path"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_download_file"), QStringLiteral("Download a file from calculator to host"), schema);
    }

    {
        QJsonObject props;
        QJsonObject pathProp;
        pathProp[QStringLiteral("type")] = QStringLiteral("string");
        pathProp[QStringLiteral("description")] = QStringLiteral("Path on calculator to delete");
        props[QStringLiteral("path")] = pathProp;
        QJsonObject isDirProp;
        isDirProp[QStringLiteral("type")] = QStringLiteral("boolean");
        isDirProp[QStringLiteral("description")] = QStringLiteral("True if deleting a directory");
        props[QStringLiteral("is_dir")] = isDirProp;
        QJsonArray required;
        required.append(QStringLiteral("path"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_delete_file"), QStringLiteral("Delete a file or directory on the calculator"), schema);
    }

    {
        QJsonObject props;
        QJsonObject pathProp;
        pathProp[QStringLiteral("type")] = QStringLiteral("string");
        pathProp[QStringLiteral("description")] = QStringLiteral("Directory path to create");
        props[QStringLiteral("path")] = pathProp;
        QJsonArray required;
        required.append(QStringLiteral("path"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_create_dir"), QStringLiteral("Create a directory on the calculator"), schema);
    }

    {
        QJsonObject props;
        QJsonObject pathProp;
        pathProp[QStringLiteral("type")] = QStringLiteral("string");
        pathProp[QStringLiteral("description")] = QStringLiteral("Path to .tns program on calculator");
        props[QStringLiteral("path")] = pathProp;
        QJsonArray required;
        required.append(QStringLiteral("path"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_execute_program"), QStringLiteral("Execute an Ndless program on the calculator"), schema);
    }

    {
        QJsonObject props;
        QJsonObject addrProp;
        addrProp[QStringLiteral("type")] = QStringLiteral("string");
        addrProp[QStringLiteral("description")] = QStringLiteral("Hex address (e.g., '0x10000000')");
        props[QStringLiteral("address")] = addrProp;
        QJsonObject sizeProp;
        sizeProp[QStringLiteral("type")] = QStringLiteral("integer");
        sizeProp[QStringLiteral("description")] = QStringLiteral("Number of bytes to read (max 4096)");
        props[QStringLiteral("size")] = sizeProp;
        QJsonArray required;
        required.append(QStringLiteral("address"));
        required.append(QStringLiteral("size"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_read_memory"), QStringLiteral("Read memory from the emulated calculator"), schema);
    }

    {
        QJsonObject props;
        QJsonObject addrProp;
        addrProp[QStringLiteral("type")] = QStringLiteral("string");
        addrProp[QStringLiteral("description")] = QStringLiteral("Hex address");
        props[QStringLiteral("address")] = addrProp;
        QJsonObject dataProp;
        dataProp[QStringLiteral("type")] = QStringLiteral("string");
        dataProp[QStringLiteral("description")] = QStringLiteral("Hex string of data to write");
        props[QStringLiteral("data")] = dataProp;
        QJsonArray required;
        required.append(QStringLiteral("address"));
        required.append(QStringLiteral("data"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_write_memory"), QStringLiteral("Write memory to the emulated calculator"), schema);
    }

    {
        QJsonObject props;
        QJsonObject addrProp;
        addrProp[QStringLiteral("type")] = QStringLiteral("string");
        addrProp[QStringLiteral("description")] = QStringLiteral("Hex address (default: current PC)");
        props[QStringLiteral("address")] = addrProp;
        QJsonObject countProp;
        countProp[QStringLiteral("type")] = QStringLiteral("integer");
        countProp[QStringLiteral("description")] = QStringLiteral("Number of instructions (default: 10)");
        props[QStringLiteral("count")] = countProp;
        QJsonObject thumbProp;
        thumbProp[QStringLiteral("type")] = QStringLiteral("boolean");
        thumbProp[QStringLiteral("description")] = QStringLiteral("Use Thumb mode (default: ARM)");
        props[QStringLiteral("thumb")] = thumbProp;
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        addTool(QStringLiteral("emulator_disassemble"), QStringLiteral("Disassemble instructions at an address"), schema);
    }

    addTool(QStringLiteral("emulator_get_registers"), QStringLiteral("Get CPU register values"), emptySchema);

    {
        QJsonObject props;
        QJsonObject regProp;
        regProp[QStringLiteral("type")] = QStringLiteral("string");
        regProp[QStringLiteral("description")] = QStringLiteral("Register name (r0-r15)");
        props[QStringLiteral("register")] = regProp;
        QJsonObject valProp;
        valProp[QStringLiteral("type")] = QStringLiteral("string");
        valProp[QStringLiteral("description")] = QStringLiteral("Hex value");
        props[QStringLiteral("value")] = valProp;
        QJsonArray required;
        required.append(QStringLiteral("register"));
        required.append(QStringLiteral("value"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_set_register"), QStringLiteral("Set a CPU register value"), schema);
    }

    {
        QJsonObject props;
        QJsonObject addrProp;
        addrProp[QStringLiteral("type")] = QStringLiteral("string");
        addrProp[QStringLiteral("description")] = QStringLiteral("Hex address");
        props[QStringLiteral("address")] = addrProp;
        QJsonObject typeProp;
        typeProp[QStringLiteral("type")] = QStringLiteral("string");
        typeProp[QStringLiteral("description")] = QStringLiteral("Breakpoint type: 'exec', 'read', 'write'");
        props[QStringLiteral("type")] = typeProp;
        QJsonArray required;
        required.append(QStringLiteral("address"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_set_breakpoint"), QStringLiteral("Set a breakpoint at an address"), schema);
    }

    {
        QJsonObject props;
        QJsonObject addrProp;
        addrProp[QStringLiteral("type")] = QStringLiteral("string");
        addrProp[QStringLiteral("description")] = QStringLiteral("Hex address");
        props[QStringLiteral("address")] = addrProp;
        QJsonArray required;
        required.append(QStringLiteral("address"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_clear_breakpoint"), QStringLiteral("Clear a breakpoint at an address"), schema);
    }

    addTool(QStringLiteral("emulator_step"), QStringLiteral("Single step one instruction"), emptySchema);
    addTool(QStringLiteral("emulator_continue"), QStringLiteral("Continue execution after a breakpoint"), emptySchema);

    {
        QJsonObject props;
        QJsonObject pathProp;
        pathProp[QStringLiteral("type")] = QStringLiteral("string");
        pathProp[QStringLiteral("description")] = QStringLiteral("Path to save snapshot");
        props[QStringLiteral("path")] = pathProp;
        QJsonArray required;
        required.append(QStringLiteral("path"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_save_snapshot"), QStringLiteral("Save emulator state to a snapshot file"), schema);
    }

    {
        QJsonObject props;
        QJsonObject pathProp;
        pathProp[QStringLiteral("type")] = QStringLiteral("string");
        pathProp[QStringLiteral("description")] = QStringLiteral("Path to snapshot file");
        props[QStringLiteral("path")] = pathProp;
        QJsonArray required;
        required.append(QStringLiteral("path"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_load_snapshot"), QStringLiteral("Load emulator state from a snapshot file"), schema);
    }

    // === New high-level tools ===

    {
        QJsonObject props;
        QJsonObject dirProp;
        dirProp[QStringLiteral("type")] = QStringLiteral("string");
        dirProp[QStringLiteral("description")] = QStringLiteral("Direction: 'up', 'down', 'left', 'right'");
        props[QStringLiteral("direction")] = dirProp;
        QJsonObject countProp;
        countProp[QStringLiteral("type")] = QStringLiteral("integer");
        countProp[QStringLiteral("description")] = QStringLiteral("Number of times to repeat (default: 1)");
        props[QStringLiteral("count")] = countProp;
        QJsonArray required;
        required.append(QStringLiteral("direction"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_arrow"), QStringLiteral("Simulate arrow key via touchpad swipe (up/down/left/right). Essential for navigating menus, exiting exponent mode, and cursor movement."), schema);
    }

    {
        QJsonObject props;
        QJsonObject timeoutProp;
        timeoutProp[QStringLiteral("type")] = QStringLiteral("integer");
        timeoutProp[QStringLiteral("description")] = QStringLiteral("Max time to wait in ms (default: 3000)");
        props[QStringLiteral("timeout_ms")] = timeoutProp;
        QJsonObject stableProp;
        stableProp[QStringLiteral("type")] = QStringLiteral("integer");
        stableProp[QStringLiteral("description")] = QStringLiteral("Screen must be unchanged for this many ms (default: 300)");
        props[QStringLiteral("stable_ms")] = stableProp;
        QJsonObject intervalProp;
        intervalProp[QStringLiteral("type")] = QStringLiteral("integer");
        intervalProp[QStringLiteral("description")] = QStringLiteral("Check interval in ms (default: 50)");
        props[QStringLiteral("interval_ms")] = intervalProp;
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        addTool(QStringLiteral("emulator_wait_stable"), QStringLiteral("Wait until the screen stops changing (stable for stable_ms). Returns a screenshot of the final stable frame. Use this after key presses or navigation to ensure the screen has finished updating."), schema);
    }

    {
        QJsonObject props;
        QJsonObject targetProp;
        targetProp[QStringLiteral("type")] = QStringLiteral("string");
        targetProp[QStringLiteral("description")] = QStringLiteral("Target: 'home', 'scratchpad_calc', 'scratchpad_graph', 'my_documents', 'back'");
        props[QStringLiteral("target")] = targetProp;
        QJsonArray required;
        required.append(QStringLiteral("target"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_navigate"), QStringLiteral("Navigate to a well-known screen (home, scratchpad_calc, scratchpad_graph, my_documents, back). Uses wait_stable internally."), schema);
    }

    {
        QJsonObject props;
        QJsonObject nameProp;
        nameProp[QStringLiteral("type")] = QStringLiteral("string");
        nameProp[QStringLiteral("description")] = QStringLiteral("Filename to open (e.g. 'polycalc'). Types first letter to jump, then enter to open.");
        props[QStringLiteral("filename")] = nameProp;
        QJsonArray required;
        required.append(QStringLiteral("filename"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_open_document"), QStringLiteral("Navigate to My Documents and open a file by name. Types the first letter to jump to it, then presses enter."), schema);
    }

    {
        QJsonObject props;
        QJsonObject exprProp;
        exprProp[QStringLiteral("type")] = QStringLiteral("string");
        exprProp[QStringLiteral("description")] = QStringLiteral("Expression to input (e.g. '(2x+3)(4x-1)')");
        props[QStringLiteral("expression")] = exprProp;
        QJsonObject ctxProp;
        ctxProp[QStringLiteral("type")] = QStringLiteral("string");
        ctxProp[QStringLiteral("description")] = QStringLiteral("Input context: 'text' (console/polycalc) or 'math2d' (OS 2D editor). Default: 'text'");
        props[QStringLiteral("context")] = ctxProp;
        QJsonArray required;
        required.append(QStringLiteral("expression"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_input_expr"), QStringLiteral("Input a math expression. In 'text' mode, types character-by-character with proper key mapping. In 'math2d' mode, handles ^{exponent} with arrow-right to exit superscript."), schema);
    }

    {
        QJsonObject props;
        QJsonObject patternProp;
        patternProp[QStringLiteral("type")] = QStringLiteral("string");
        patternProp[QStringLiteral("description")] = QStringLiteral("Hex byte pattern to search for (e.g. 'E92D4' for STMFD SP!, {…,LR})");
        props[QStringLiteral("pattern")] = patternProp;
        QJsonObject startProp;
        startProp[QStringLiteral("type")] = QStringLiteral("string");
        startProp[QStringLiteral("description")] = QStringLiteral("Start address in hex (e.g. '0x10000000')");
        props[QStringLiteral("start_address")] = startProp;
        QJsonObject endProp;
        endProp[QStringLiteral("type")] = QStringLiteral("string");
        endProp[QStringLiteral("description")] = QStringLiteral("End address in hex (e.g. '0x10100000')");
        props[QStringLiteral("end_address")] = endProp;
        QJsonObject maxProp;
        maxProp[QStringLiteral("type")] = QStringLiteral("integer");
        maxProp[QStringLiteral("description")] = QStringLiteral("Max results to return (default: 20)");
        props[QStringLiteral("max_results")] = maxProp;
        QJsonArray required;
        required.append(QStringLiteral("pattern"));
        required.append(QStringLiteral("start_address"));
        required.append(QStringLiteral("end_address"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_search_memory"), QStringLiteral("Search memory for a hex byte pattern. Scans in 4096-byte chunks. Useful for finding function prologues, string references, or hook trampolines."), schema);
    }

    {
        QJsonObject props;
        QJsonObject addrProp;
        addrProp[QStringLiteral("type")] = QStringLiteral("string");
        addrProp[QStringLiteral("description")] = QStringLiteral("Hex address to read from");
        props[QStringLiteral("address")] = addrProp;
        QJsonObject encProp;
        encProp[QStringLiteral("type")] = QStringLiteral("string");
        encProp[QStringLiteral("description")] = QStringLiteral("Encoding: 'ascii' or 'utf16' (default: 'ascii')");
        props[QStringLiteral("encoding")] = encProp;
        QJsonObject maxProp;
        maxProp[QStringLiteral("type")] = QStringLiteral("integer");
        maxProp[QStringLiteral("description")] = QStringLiteral("Max length in bytes (default: 256)");
        props[QStringLiteral("max_length")] = maxProp;
        QJsonArray required;
        required.append(QStringLiteral("address"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_read_string"), QStringLiteral("Read a null-terminated string from memory. Supports ASCII and UTF-16 encodings."), schema);
    }

    {
        QJsonObject props;
        QJsonObject addrProp;
        addrProp[QStringLiteral("type")] = QStringLiteral("string");
        addrProp[QStringLiteral("description")] = QStringLiteral("Hex address to break at");
        props[QStringLiteral("address")] = addrProp;
        QJsonObject timeoutProp;
        timeoutProp[QStringLiteral("type")] = QStringLiteral("integer");
        timeoutProp[QStringLiteral("description")] = QStringLiteral("Timeout in ms (default: 5000)");
        props[QStringLiteral("timeout_ms")] = timeoutProp;
        QJsonObject typeProp;
        typeProp[QStringLiteral("type")] = QStringLiteral("string");
        typeProp[QStringLiteral("description")] = QStringLiteral("Breakpoint type: 'exec', 'read', 'write' (default: 'exec')");
        props[QStringLiteral("type")] = typeProp;
        QJsonArray required;
        required.append(QStringLiteral("address"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_run_until"), QStringLiteral("Set a breakpoint, resume execution, and wait for it to hit (or timeout). Returns registers + disassembly at PC on hit. Cleans up breakpoint afterward."), schema);
    }

    {
        QJsonObject props;
        QJsonObject countProp;
        countProp[QStringLiteral("type")] = QStringLiteral("integer");
        countProp[QStringLiteral("description")] = QStringLiteral("Number of instructions to trace (default: 10, max: 1000)");
        props[QStringLiteral("count")] = countProp;
        QJsonObject thumbProp;
        thumbProp[QStringLiteral("type")] = QStringLiteral("boolean");
        thumbProp[QStringLiteral("description")] = QStringLiteral("Disassemble in Thumb mode (default: auto-detect from CPSR)");
        props[QStringLiteral("thumb")] = thumbProp;
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        addTool(QStringLiteral("emulator_trace"), QStringLiteral("Single-step N instructions, recording PC + disassembly for each. Returns an execution trace log. The emulator must be paused first."), schema);
    }

    {
        QJsonObject props;
        QJsonObject startProp;
        startProp[QStringLiteral("type")] = QStringLiteral("string");
        startProp[QStringLiteral("description")] = QStringLiteral("Start address in hex");
        props[QStringLiteral("start_address")] = startProp;
        QJsonObject endProp;
        endProp[QStringLiteral("type")] = QStringLiteral("string");
        endProp[QStringLiteral("description")] = QStringLiteral("End address in hex");
        props[QStringLiteral("end_address")] = endProp;
        QJsonArray required;
        required.append(QStringLiteral("start_address"));
        required.append(QStringLiteral("end_address"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_find_functions"), QStringLiteral("Scan memory for ARM function prologues (STMFD SP!, {…,LR} and Thumb PUSH {…,LR}). Returns probable function start addresses."), schema);
    }

    {
        QJsonObject props;
        QJsonObject localProp;
        localProp[QStringLiteral("type")] = QStringLiteral("string");
        localProp[QStringLiteral("description")] = QStringLiteral("Path to .tns file on host");
        props[QStringLiteral("local_path")] = localProp;
        QJsonObject remoteProp;
        remoteProp[QStringLiteral("type")] = QStringLiteral("string");
        remoteProp[QStringLiteral("description")] = QStringLiteral("Path on calculator (e.g. '/polycalc.tns')");
        props[QStringLiteral("calc_path")] = remoteProp;
        QJsonObject waitProp;
        waitProp[QStringLiteral("type")] = QStringLiteral("boolean");
        waitProp[QStringLiteral("description")] = QStringLiteral("Wait for program to finish (screen stable) (default: true)");
        props[QStringLiteral("wait_for_exit")] = waitProp;
        QJsonArray required;
        required.append(QStringLiteral("local_path"));
        required.append(QStringLiteral("calc_path"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_deploy_and_run"), QStringLiteral("Upload a .tns file to the calculator, navigate to it in My Documents, and open it. Returns a screenshot of the result. One-shot build-test workflow."), schema);
    }

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

    addTool(QStringLiteral("emulator_get_screen_info"), QStringLiteral("Get screen dimensions and basic state info"), emptySchema);

    {
        QJsonObject props;
        QJsonObject opsProp;
        opsProp[QStringLiteral("type")] = QStringLiteral("array");
        opsProp[QStringLiteral("description")] = QStringLiteral("Array of operations: [{\"op\":\"key\",\"key\":\"enter\"}, {\"op\":\"nav\",\"dir\":\"down\"}, {\"op\":\"wait\",\"ms\":100}, {\"op\":\"type\",\"text\":\"hello\"}, {\"op\":\"wait_stable\",\"timeout_ms\":5000,\"stable_ms\":300}]");
        props[QStringLiteral("operations")] = opsProp;
        QJsonArray required;
        required.append(QStringLiteral("operations"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_run_macro"), QStringLiteral("Run a sequence of operations (key presses, arrow navigation, waits, text input, wait_stable). Reduces round-trips for multi-step interactions."), schema);
    }

    // OCR tools
    {
        QJsonObject props;
        QJsonObject lineStartProp;
        lineStartProp[QStringLiteral("type")] = QStringLiteral("integer");
        lineStartProp[QStringLiteral("description")] = QStringLiteral("First text line to read (0-based, default: 0)");
        props[QStringLiteral("line_start")] = lineStartProp;
        QJsonObject lineCountProp;
        lineCountProp[QStringLiteral("type")] = QStringLiteral("integer");
        lineCountProp[QStringLiteral("description")] = QStringLiteral("Number of text lines to read (default: all)");
        props[QStringLiteral("line_count")] = lineCountProp;
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        addTool(QStringLiteral("emulator_ocr"), QStringLiteral("Read text from the calculator screen using glyph-based OCR. Run emulator_ocr_calibrate first for best results."), schema);
    }

    {
        QJsonObject props;
        QJsonObject knownTextProp;
        knownTextProp[QStringLiteral("type")] = QStringLiteral("string");
        knownTextProp[QStringLiteral("description")] = QStringLiteral("Known text matching the first visible text line on screen. Include ALL characters exactly as displayed.");
        props[QStringLiteral("known_text")] = knownTextProp;
        QJsonArray required;
        required.append(QStringLiteral("known_text"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_ocr_calibrate"), QStringLiteral("Calibrate OCR by providing known text that matches the first text line on screen. Detects font metrics and builds glyph table."), schema);
    }

    // GDB client tools
    {
        QJsonObject props;
        QJsonObject portProp;
        portProp[QStringLiteral("type")] = QStringLiteral("integer");
        portProp[QStringLiteral("description")] = QStringLiteral("GDB stub port (default: 3333)");
        props[QStringLiteral("port")] = portProp;
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        addTool(QStringLiteral("emulator_gdb_connect"), QStringLiteral("Connect to the emulator's GDB stub for fast register/memory access and proper debugging"), schema);
    }

    {
        QJsonObject props;
        QJsonObject addrProp;
        addrProp[QStringLiteral("type")] = QStringLiteral("string");
        addrProp[QStringLiteral("description")] = QStringLiteral("Hex address (e.g., '0x10000000')");
        props[QStringLiteral("address")] = addrProp;
        QJsonObject sizeProp;
        sizeProp[QStringLiteral("type")] = QStringLiteral("integer");
        sizeProp[QStringLiteral("description")] = QStringLiteral("Number of bytes to read (max 1023)");
        props[QStringLiteral("size")] = sizeProp;
        QJsonArray required;
        required.append(QStringLiteral("address"));
        required.append(QStringLiteral("size"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_gdb_read_memory"), QStringLiteral("Read memory via GDB (halts CPU briefly, then resumes)"), schema);
    }

    {
        QJsonObject props;
        QJsonObject addrProp;
        addrProp[QStringLiteral("type")] = QStringLiteral("string");
        addrProp[QStringLiteral("description")] = QStringLiteral("Hex address (e.g., '0x10000000')");
        props[QStringLiteral("address")] = addrProp;
        QJsonObject dataProp;
        dataProp[QStringLiteral("type")] = QStringLiteral("string");
        dataProp[QStringLiteral("description")] = QStringLiteral("Hex data to write (e.g., '41424344')");
        props[QStringLiteral("data")] = dataProp;
        QJsonArray required;
        required.append(QStringLiteral("address"));
        required.append(QStringLiteral("data"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_gdb_write_memory"), QStringLiteral("Write memory via GDB (halts CPU briefly, then resumes)"), schema);
    }

    addTool(QStringLiteral("emulator_gdb_read_registers"), QStringLiteral("Read all ARM registers via GDB in one shot (r0-r15, cpsr)"), emptySchema);

    {
        QJsonObject props;
        QJsonObject regProp;
        regProp[QStringLiteral("type")] = QStringLiteral("string");
        regProp[QStringLiteral("description")] = QStringLiteral("Register name (r0-r15, sp, lr, pc, cpsr)");
        props[QStringLiteral("register")] = regProp;
        QJsonObject valProp;
        valProp[QStringLiteral("type")] = QStringLiteral("string");
        valProp[QStringLiteral("description")] = QStringLiteral("Hex value (e.g., '0x10000000')");
        props[QStringLiteral("value")] = valProp;
        QJsonArray required;
        required.append(QStringLiteral("register"));
        required.append(QStringLiteral("value"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_gdb_set_register"), QStringLiteral("Set a CPU register via GDB"), schema);
    }

    {
        QJsonObject props;
        QJsonObject addrProp;
        addrProp[QStringLiteral("type")] = QStringLiteral("string");
        addrProp[QStringLiteral("description")] = QStringLiteral("Hex address for breakpoint");
        props[QStringLiteral("address")] = addrProp;
        QJsonObject setProp;
        setProp[QStringLiteral("type")] = QStringLiteral("boolean");
        setProp[QStringLiteral("description")] = QStringLiteral("true to set, false to clear");
        props[QStringLiteral("set")] = setProp;
        QJsonArray required;
        required.append(QStringLiteral("address"));
        required.append(QStringLiteral("set"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_gdb_breakpoint"), QStringLiteral("Set or clear an execution breakpoint via GDB"), schema);
    }

    addTool(QStringLiteral("emulator_gdb_continue"), QStringLiteral("Resume execution via GDB (after step or breakpoint hit)"), emptySchema);
    addTool(QStringLiteral("emulator_gdb_step"), QStringLiteral("Single step one instruction via GDB. Returns registers at new PC. CPU stays halted."), emptySchema);

    {
        QJsonObject props;
        QJsonObject pktProp;
        pktProp[QStringLiteral("type")] = QStringLiteral("string");
        pktProp[QStringLiteral("description")] = QStringLiteral("Raw GDB packet payload (without $...#XX framing)");
        props[QStringLiteral("packet")] = pktProp;
        QJsonArray required;
        required.append(QStringLiteral("packet"));
        QJsonObject schema;
        schema[QStringLiteral("type")] = QStringLiteral("object");
        schema[QStringLiteral("properties")] = props;
        schema[QStringLiteral("required")] = required;
        addTool(QStringLiteral("emulator_gdb_raw"), QStringLiteral("Send a raw GDB RSP packet and return the response"), schema);
    }

    QJsonObject result;
    result[QStringLiteral("tools")] = tools;

    sendResult(id, result);
}

void MCPServer::handleToolsCall(const QJsonValue &id, const QJsonObject &params)
{
    QString name = params.value(QStringLiteral("name")).toString();
    QJsonObject args = params.value(QStringLiteral("arguments")).toObject();

    QJsonObject result;

    if (name == QStringLiteral("emulator_status")) {
        result = toolEmulatorStatus();
    } else if (name == QStringLiteral("emulator_start")) {
        result = toolEmulatorStart(args);
    } else if (name == QStringLiteral("emulator_stop")) {
        result = toolEmulatorStop();
    } else if (name == QStringLiteral("emulator_reset")) {
        result = toolEmulatorReset();
    } else if (name == QStringLiteral("emulator_pause")) {
        result = toolEmulatorPause(args);
    } else if (name == QStringLiteral("emulator_set_turbo")) {
        result = toolEmulatorSetTurbo(args);
    } else if (name == QStringLiteral("emulator_screenshot")) {
        result = toolEmulatorScreenshot(args);
    } else if (name == QStringLiteral("emulator_press_key")) {
        result = toolEmulatorPressKey(args);
    } else if (name == QStringLiteral("emulator_key_down")) {
        result = toolEmulatorKeyDown(args);
    } else if (name == QStringLiteral("emulator_key_up")) {
        result = toolEmulatorKeyUp(args);
    } else if (name == QStringLiteral("emulator_type_text")) {
        result = toolEmulatorTypeText(args);
    } else if (name == QStringLiteral("emulator_touchpad")) {
        result = toolEmulatorTouchpad(args);
    } else if (name == QStringLiteral("emulator_list_files")) {
        result = toolEmulatorListFiles(args);
    } else if (name == QStringLiteral("emulator_upload_file")) {
        result = toolEmulatorUploadFile(args);
    } else if (name == QStringLiteral("emulator_send_os")) {
        result = toolEmulatorSendOs(args);
    } else if (name == QStringLiteral("emulator_download_file")) {
        result = toolEmulatorDownloadFile(args);
    } else if (name == QStringLiteral("emulator_delete_file")) {
        result = toolEmulatorDeleteFile(args);
    } else if (name == QStringLiteral("emulator_create_dir")) {
        result = toolEmulatorCreateDir(args);
    } else if (name == QStringLiteral("emulator_read_memory")) {
        result = toolEmulatorReadMemory(args);
    } else if (name == QStringLiteral("emulator_write_memory")) {
        result = toolEmulatorWriteMemory(args);
    } else if (name == QStringLiteral("emulator_disassemble")) {
        result = toolEmulatorDisassemble(args);
    } else if (name == QStringLiteral("emulator_get_registers")) {
        result = toolEmulatorGetRegisters();
    } else if (name == QStringLiteral("emulator_set_register")) {
        result = toolEmulatorSetRegister(args);
    } else if (name == QStringLiteral("emulator_set_breakpoint")) {
        result = toolEmulatorSetBreakpoint(args);
    } else if (name == QStringLiteral("emulator_clear_breakpoint")) {
        result = toolEmulatorClearBreakpoint(args);
    } else if (name == QStringLiteral("emulator_step")) {
        result = toolEmulatorStep();
    } else if (name == QStringLiteral("emulator_continue")) {
        result = toolEmulatorContinue();
    } else if (name == QStringLiteral("emulator_execute_program")) {
        result = toolEmulatorExecuteProgram(args);
    } else if (name == QStringLiteral("emulator_save_snapshot")) {
        result = toolEmulatorSaveSnapshot(args);
    } else if (name == QStringLiteral("emulator_load_snapshot")) {
        result = toolEmulatorLoadSnapshot(args);
    } else if (name == QStringLiteral("emulator_arrow")) {
        result = toolEmulatorArrow(args);
    } else if (name == QStringLiteral("emulator_wait_stable")) {
        result = toolEmulatorWaitStable(args);
    } else if (name == QStringLiteral("emulator_navigate")) {
        result = toolEmulatorNavigate(args);
    } else if (name == QStringLiteral("emulator_open_document")) {
        result = toolEmulatorOpenDocument(args);
    } else if (name == QStringLiteral("emulator_input_expr")) {
        result = toolEmulatorInputExpr(args);
    } else if (name == QStringLiteral("emulator_search_memory")) {
        result = toolEmulatorSearchMemory(args);
    } else if (name == QStringLiteral("emulator_read_string")) {
        result = toolEmulatorReadString(args);
    } else if (name == QStringLiteral("emulator_run_until")) {
        result = toolEmulatorRunUntil(args);
    } else if (name == QStringLiteral("emulator_trace")) {
        result = toolEmulatorTrace(args);
    } else if (name == QStringLiteral("emulator_find_functions")) {
        result = toolEmulatorFindFunctions(args);
    } else if (name == QStringLiteral("emulator_deploy_and_run")) {
        result = toolEmulatorDeployAndRun(args);
    } else if (name == QStringLiteral("emulator_get_screen_info")) {
        result = toolEmulatorGetScreenInfo();
    } else if (name == QStringLiteral("emulator_run_macro")) {
        result = toolEmulatorRunMacro(args);
    } else if (name == QStringLiteral("emulator_ocr")) {
        result = toolEmulatorOcr(args);
    } else if (name == QStringLiteral("emulator_ocr_calibrate")) {
        result = toolEmulatorOcrCalibrate(args);
    } else if (name == QStringLiteral("emulator_gdb_connect")) {
        result = toolEmulatorGdbConnect(args);
    } else if (name == QStringLiteral("emulator_gdb_read_memory")) {
        result = toolEmulatorGdbReadMemory(args);
    } else if (name == QStringLiteral("emulator_gdb_write_memory")) {
        result = toolEmulatorGdbWriteMemory(args);
    } else if (name == QStringLiteral("emulator_gdb_read_registers")) {
        result = toolEmulatorGdbReadRegisters();
    } else if (name == QStringLiteral("emulator_gdb_set_register")) {
        result = toolEmulatorGdbSetRegister(args);
    } else if (name == QStringLiteral("emulator_gdb_breakpoint")) {
        result = toolEmulatorGdbBreakpoint(args);
    } else if (name == QStringLiteral("emulator_gdb_continue")) {
        result = toolEmulatorGdbContinue();
    } else if (name == QStringLiteral("emulator_gdb_step")) {
        result = toolEmulatorGdbStep();
    } else if (name == QStringLiteral("emulator_gdb_raw")) {
        result = toolEmulatorGdbRaw(args);
    } else {
        sendError(id, -32602, QStringLiteral("Unknown tool: ") + name);
        return;
    }

    sendResult(id, result);
}

// OCR helpers

static QVector<bool> framebufferToBinary(const QImage &image, int threshold = 128)
{
    int w = image.width();
    int h = image.height();
    QVector<bool> binary(w * h, false);

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            QRgb pixel = image.pixel(x, y);
            int gray = qGray(pixel);
            binary[y * w + x] = (gray < threshold);
        }
    }
    return binary;
}

struct FontMetrics {
    int charWidth;
    int charHeight;
    int startX;
    int startY;
    int cols;
    int rows;
    bool valid;
};

static FontMetrics detectFontMetrics(const QVector<bool> &binary, int imgW, int imgH)
{
    FontMetrics fm = {0, 0, 0, 0, 0, 0, false};

    QVector<int> rowDensity(imgH, 0);
    for (int y = 0; y < imgH; ++y) {
        for (int x = 0; x < imgW; ++x) {
            if (binary[y * imgW + x]) rowDensity[y]++;
        }
    }

    QVector<QPair<int,int>> textBands;
    bool inBand = false;
    int bandStart = 0;
    int minDensity = imgW / 100;

    for (int y = 0; y < imgH; ++y) {
        if (rowDensity[y] > minDensity) {
            if (!inBand) { bandStart = y; inBand = true; }
        } else {
            if (inBand) {
                textBands.append({bandStart, y});
                inBand = false;
            }
        }
    }
    if (inBand) textBands.append({bandStart, imgH});

    if (textBands.size() < 2) {
        minDensity = 1;
        textBands.clear();
        inBand = false;
        for (int y = 0; y < imgH; ++y) {
            if (rowDensity[y] > minDensity) {
                if (!inBand) { bandStart = y; inBand = true; }
            } else {
                if (inBand) { textBands.append({bandStart, y}); inBand = false; }
            }
        }
        if (inBand) textBands.append({bandStart, imgH});
    }

    if (textBands.size() < 2) return fm;

    QMap<int, int> heightCounts;
    for (const auto &band : textBands) {
        int h = band.second - band.first;
        if (h >= 6 && h <= 20) heightCounts[h]++;
    }

    int bestHeight = 0, bestCount = 0;
    for (auto it = heightCounts.begin(); it != heightCounts.end(); ++it) {
        if (it.value() > bestCount) {
            bestCount = it.value();
            bestHeight = it.key();
        }
    }

    if (bestHeight == 0) return fm;

    QVector<int> spacings;
    for (int i = 1; i < textBands.size(); ++i) {
        int spacing = textBands[i].first - textBands[i-1].first;
        if (spacing >= bestHeight && spacing <= bestHeight * 2)
            spacings.append(spacing);
    }

    int lineHeight = bestHeight;
    if (!spacings.isEmpty()) {
        std::sort(spacings.begin(), spacings.end());
        lineHeight = spacings[spacings.size() / 2];
    }

    int bestBand = 0;
    int bestBandDensity = 0;
    for (int i = 0; i < textBands.size(); ++i) {
        int h = textBands[i].second - textBands[i].first;
        if (qAbs(h - bestHeight) <= 2) {
            int density = 0;
            for (int y = textBands[i].first; y < textBands[i].second; ++y)
                density += rowDensity[y];
            if (density > bestBandDensity) {
                bestBandDensity = density;
                bestBand = i;
            }
        }
    }

    int by0 = textBands[bestBand].first;
    int by1 = textBands[bestBand].second;
    QVector<int> colDensity(imgW, 0);
    for (int x = 0; x < imgW; ++x) {
        for (int y = by0; y < by1; ++y) {
            if (binary[y * imgW + x]) colDensity[x]++;
        }
    }

    QVector<int> gapPositions;
    for (int x = 1; x < imgW - 1; ++x) {
        if (colDensity[x] == 0 && (colDensity[x-1] > 0 || colDensity[x+1] > 0))
            gapPositions.append(x);
    }

    int bestWidth = 0;
    int bestWidthScore = 0;
    for (int tryWidth = 5; tryWidth <= 10; ++tryWidth) {
        int score = 0;
        for (int gap : gapPositions) {
            for (int offset = 0; offset < tryWidth; ++offset) {
                if ((gap - offset) % tryWidth == 0) {
                    score++;
                    break;
                }
            }
        }
        int chars = imgW / tryWidth;
        if (chars >= 30 && chars <= 60) score += 5;
        if (score > bestWidthScore) {
            bestWidthScore = score;
            bestWidth = tryWidth;
        }
    }

    if (bestWidth == 0) bestWidth = 6;

    fm.charWidth = bestWidth;
    fm.charHeight = lineHeight;
    fm.startX = 0;
    fm.startY = textBands[0].first;
    fm.cols = imgW / bestWidth;
    fm.rows = (imgH - fm.startY) / lineHeight;
    fm.valid = true;

    for (int x = 0; x < imgW; ++x) {
        bool hasDark = false;
        for (int y = 0; y < imgH && !hasDark; ++y)
            hasDark = binary[y * imgW + x];
        if (hasDark) { fm.startX = x; break; }
    }
    fm.startX = (fm.startX / fm.charWidth) * fm.charWidth;
    fm.cols = (imgW - fm.startX) / fm.charWidth;

    return fm;
}

static QByteArray extractGlyph(const QVector<bool> &binary, int imgW,
                                int cellX, int cellY, int cellW, int cellH)
{
    QByteArray glyph;
    glyph.reserve((cellW * cellH + 7) / 8);

    uint8_t byte = 0;
    int bit = 0;
    for (int y = cellY; y < cellY + cellH && y < 240; ++y) {
        for (int x = cellX; x < cellX + cellW && x < 320; ++x) {
            if (binary[y * imgW + x])
                byte |= (1 << bit);
            bit++;
            if (bit == 8) {
                glyph.append(static_cast<char>(byte));
                byte = 0;
                bit = 0;
            }
        }
    }
    if (bit > 0) glyph.append(static_cast<char>(byte));

    return glyph;
}

// Tool implementations

QJsonObject MCPServer::toolEmulatorStatus()
{
    QJsonObject status;
    status[QStringLiteral("running")] = emu_thread.isRunning();
    status[QStringLiteral("paused")] = emu_thread.isPaused();
    status[QStringLiteral("turbo_mode")] = turbo_mode;

    return makeToolResult(QString::fromUtf8(QJsonDocument(status).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorStart(const QJsonObject &args)
{
    if (emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is already running"));
    }

    QString snapshotPath = args.value(QStringLiteral("snapshot_path")).toString();

    // Enable GDB stub by default for MCP GDB client tools
    if (emu_thread.port_gdb == 0)
        emu_thread.port_gdb = 3333;

    bool success = false;
    if (!snapshotPath.isEmpty()) {
        success = emu_thread.resume(snapshotPath);
    } else {
        // Start with current kit configuration
        if (the_qml_bridge) {
            success = the_qml_bridge->restart();
        }
    }

    if (success) {
        QJsonObject result;
        result[QStringLiteral("success")] = true;
        result[QStringLiteral("message")] = QStringLiteral("Emulator started");
        return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
    } else {
        return makeToolError(QStringLiteral("Failed to start emulator"));
    }
}

QJsonObject MCPServer::toolEmulatorStop()
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    bool success = emu_thread.stop();

    QJsonObject result;
    result[QStringLiteral("success")] = success;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorReset()
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    emu_thread.reset();

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorPause(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    bool paused = args.value(QStringLiteral("paused")).toBool();
    emu_thread.setPaused(paused);

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("paused")] = paused;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorSetTurbo(const QJsonObject &args)
{
    bool enabled = args.value(QStringLiteral("enabled")).toBool();
    emu_thread.setTurboMode(enabled);

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("turbo_mode")] = enabled;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorScreenshot(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString outputPath = args.value(QStringLiteral("output_path")).toString();
    if (outputPath.isEmpty()) {
        outputPath = QStringLiteral("/tmp/firebird_mcp_%1.png")
                        .arg(QDateTime::currentMSecsSinceEpoch());
    }

    // Capture the framebuffer
    QImage image = renderFramebuffer();
    if (image.isNull()) {
        return makeToolError(QStringLiteral("Failed to capture framebuffer"));
    }

    if (!image.save(outputPath, "PNG")) {
        return makeToolError(QStringLiteral("Failed to save screenshot to: ") + outputPath);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("path")] = outputPath;
    result[QStringLiteral("width")] = image.width();
    result[QStringLiteral("height")] = image.height();
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorPressKey(const QJsonObject &args)
{
    QString keyName = args.value(QStringLiteral("key")).toString();
    int durationMs = args.value(QStringLiteral("duration_ms")).toInt(50);

    KeyPosition pos = getKeyPosition(keyName);
    if (!pos.valid) {
        return makeToolError(QStringLiteral("Unknown key: ") + keyName + QStringLiteral(". Valid keys: ") + getAllKeyNames().join(QStringLiteral(", ")));
    }

    // Press key
    keypad_set_key(pos.row, pos.col, true);

    // Wait then release
    QThread::msleep(durationMs);
    keypad_set_key(pos.row, pos.col, false);

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("key")] = keyName;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorKeyDown(const QJsonObject &args)
{
    QString keyName = args.value(QStringLiteral("key")).toString();

    KeyPosition pos = getKeyPosition(keyName);
    if (!pos.valid) {
        return makeToolError(QStringLiteral("Unknown key: ") + keyName);
    }

    keypad_set_key(pos.row, pos.col, true);

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("key")] = keyName;
    result[QStringLiteral("state")] = QStringLiteral("down");
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorKeyUp(const QJsonObject &args)
{
    QString keyName = args.value(QStringLiteral("key")).toString();

    KeyPosition pos = getKeyPosition(keyName);
    if (!pos.valid) {
        return makeToolError(QStringLiteral("Unknown key: ") + keyName);
    }

    keypad_set_key(pos.row, pos.col, false);

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("key")] = keyName;
    result[QStringLiteral("state")] = QStringLiteral("up");
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorTypeText(const QJsonObject &args)
{
    QString text = args.value(QStringLiteral("text")).toString();
    int delayMs = args.value(QStringLiteral("delay_ms")).toInt(50);

    for (const QChar &ch : text) {
        QString keyName = ch.toLower();

        // Handle special characters
        if (ch == QLatin1Char(' ')) keyName = QStringLiteral("space");
        else if (ch == QLatin1Char('\n') || ch == QLatin1Char('\r')) keyName = QStringLiteral("enter");
        else if (ch == QLatin1Char('+')) keyName = QStringLiteral("plus");
        else if (ch == QLatin1Char('-')) keyName = QStringLiteral("minus");
        else if (ch == QLatin1Char('*')) keyName = QStringLiteral("multiply");
        else if (ch == QLatin1Char('/')) keyName = QStringLiteral("divide");
        else if (ch == QLatin1Char('=')) keyName = QStringLiteral("equals");
        else if (ch == QLatin1Char('(')) keyName = QStringLiteral("leftparen");
        else if (ch == QLatin1Char(')')) keyName = QStringLiteral("rightparen");
        else if (ch == QLatin1Char(',')) keyName = QStringLiteral("comma");
        else if (ch == QLatin1Char('.')) keyName = QStringLiteral("dot");

        KeyPosition pos = getKeyPosition(keyName);
        if (pos.valid) {
            keypad_set_key(pos.row, pos.col, true);
            QThread::msleep(delayMs / 2);
            keypad_set_key(pos.row, pos.col, false);
            QThread::msleep(delayMs / 2);
        }
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("typed")] = text;
    result[QStringLiteral("length")] = text.length();
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

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

// File transfer tools - these use async callbacks

static void dirlist_callback(struct usblink_file *file, bool is_error, void *user_data)
{
    MCPServer *server = static_cast<MCPServer*>(user_data);
    QMutexLocker locker(&server->m_asyncMutex);

    if (file == nullptr) {
        // End of listing
        server->m_asyncComplete = true;
        if (is_error) {
            server->m_asyncError = QStringLiteral("Directory listing failed");
        }
        server->m_asyncCondition.wakeAll();
    } else {
        QJsonObject entry;
        entry[QStringLiteral("name")] = QString::fromUtf8(file->filename);
        entry[QStringLiteral("size")] = (qint64)file->size;
        entry[QStringLiteral("is_dir")] = file->is_dir;
        server->m_dirListResult.append(entry);
    }
}

QJsonObject MCPServer::toolEmulatorListFiles(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString path = args.value(QStringLiteral("path")).toString();

    m_asyncMutex.lock();
    m_asyncComplete = false;
    m_asyncError.clear();
    m_dirListResult = QJsonArray();
    m_asyncMutex.unlock();

    usblink_queue_dirlist(path.toStdString(), dirlist_callback, this);

    // Wait for completion (with timeout)
    m_asyncMutex.lock();
    bool completed = m_asyncCondition.wait(&m_asyncMutex, 30000);
    m_asyncMutex.unlock();

    if (!completed) {
        return makeToolError(QStringLiteral("Directory listing timed out"));
    }

    if (!m_asyncError.isEmpty()) {
        return makeToolError(m_asyncError);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("path")] = path;
    result[QStringLiteral("files")] = m_dirListResult;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

static void progress_callback(int progress, void *user_data)
{
    MCPServer *server = static_cast<MCPServer*>(user_data);
    QMutexLocker locker(&server->m_asyncMutex);

    server->m_asyncProgress = progress;
    if (progress == 100 || progress < 0) {
        server->m_asyncComplete = true;
        if (progress < 0) {
            server->m_asyncError = QStringLiteral("File operation failed");
        }
        server->m_asyncCondition.wakeAll();
    }
}

QJsonObject MCPServer::toolEmulatorUploadFile(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString localPath = args.value(QStringLiteral("local_path")).toString();
    QString remotePath = args.value(QStringLiteral("remote_path")).toString();

    if (!QFile::exists(localPath)) {
        return makeToolError(QStringLiteral("Local file does not exist: ") + localPath);
    }

    m_asyncMutex.lock();
    m_asyncComplete = false;
    m_asyncError.clear();
    m_asyncProgress = 0;
    m_asyncMutex.unlock();

    usblink_queue_put_file(localPath.toStdString(), remotePath.toStdString(), progress_callback, this);

    // Wait for completion
    m_asyncMutex.lock();
    bool completed = m_asyncCondition.wait(&m_asyncMutex, 60000);
    m_asyncMutex.unlock();

    if (!completed) {
        return makeToolError(QStringLiteral("File upload timed out"));
    }

    if (!m_asyncError.isEmpty()) {
        return makeToolError(m_asyncError);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("local_path")] = localPath;
    result[QStringLiteral("remote_path")] = remotePath;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorSendOs(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString osPath = args.value(QStringLiteral("os_path")).toString();

    if (!QFile::exists(osPath)) {
        return makeToolError(QStringLiteral("OS file does not exist: ") + osPath);
    }

    m_asyncMutex.lock();
    m_asyncComplete = false;
    m_asyncError.clear();
    m_asyncProgress = 0;
    m_asyncMutex.unlock();

    usblink_queue_send_os(osPath.toStdString(), progress_callback, this);

    // Wait for completion (longer timeout for OS transfer)
    m_asyncMutex.lock();
    bool completed = m_asyncCondition.wait(&m_asyncMutex, 300000);
    m_asyncMutex.unlock();

    if (!completed) {
        return makeToolError(QStringLiteral("OS transfer timed out"));
    }

    if (!m_asyncError.isEmpty()) {
        return makeToolError(m_asyncError);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("os_path")] = osPath;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorDownloadFile(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString remotePath = args.value(QStringLiteral("remote_path")).toString();
    QString localPath = args.value(QStringLiteral("local_path")).toString();

    m_asyncMutex.lock();
    m_asyncComplete = false;
    m_asyncError.clear();
    m_asyncProgress = 0;
    m_asyncMutex.unlock();

    usblink_queue_download(remotePath.toStdString(), localPath.toStdString(), progress_callback, this);

    // Wait for completion
    m_asyncMutex.lock();
    bool completed = m_asyncCondition.wait(&m_asyncMutex, 60000);
    m_asyncMutex.unlock();

    if (!completed) {
        return makeToolError(QStringLiteral("File download timed out"));
    }

    if (!m_asyncError.isEmpty()) {
        return makeToolError(m_asyncError);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("remote_path")] = remotePath;
    result[QStringLiteral("local_path")] = localPath;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorDeleteFile(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString path = args.value(QStringLiteral("path")).toString();
    bool isDir = args.value(QStringLiteral("is_dir")).toBool(false);

    m_asyncMutex.lock();
    m_asyncComplete = false;
    m_asyncError.clear();
    m_asyncMutex.unlock();

    usblink_queue_delete(path.toStdString(), isDir, progress_callback, this);

    m_asyncMutex.lock();
    bool completed = m_asyncCondition.wait(&m_asyncMutex, 30000);
    m_asyncMutex.unlock();

    if (!completed) {
        return makeToolError(QStringLiteral("Delete operation timed out"));
    }

    if (!m_asyncError.isEmpty()) {
        return makeToolError(m_asyncError);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("path")] = path;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorCreateDir(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString path = args.value(QStringLiteral("path")).toString();

    m_asyncMutex.lock();
    m_asyncComplete = false;
    m_asyncError.clear();
    m_asyncMutex.unlock();

    usblink_queue_new_dir(path.toStdString(), progress_callback, this);

    m_asyncMutex.lock();
    bool completed = m_asyncCondition.wait(&m_asyncMutex, 30000);
    m_asyncMutex.unlock();

    if (!completed) {
        return makeToolError(QStringLiteral("Create directory timed out"));
    }

    if (!m_asyncError.isEmpty()) {
        return makeToolError(m_asyncError);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("path")] = path;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// Debug tools

QJsonObject MCPServer::toolEmulatorReadMemory(const QJsonObject &args)
{
    QString addrStr = args.value(QStringLiteral("address")).toString();
    int size = args.value(QStringLiteral("size")).toInt();

    if (size <= 0 || size > 4096) {
        return makeToolError(QStringLiteral("Size must be between 1 and 4096"));
    }

    bool ok;
    uint32_t addr = addrStr.toUInt(&ok, 0);
    if (!ok) {
        return makeToolError(QStringLiteral("Invalid address: ") + addrStr);
    }

    void *ptr = virt_mem_ptr(addr, size);
    if (!ptr) {
        return makeToolError(QStringLiteral("Address 0x%1 is not accessible").arg(addr, 8, 16, QLatin1Char('0')));
    }

    // Convert to hex string
    QByteArray data((const char*)ptr, size);
    QString hexStr = QString::fromLatin1(data.toHex());

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("address")] = QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0'));
    result[QStringLiteral("size")] = size;
    result[QStringLiteral("data")] = hexStr;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorWriteMemory(const QJsonObject &args)
{
    QString addrStr = args.value(QStringLiteral("address")).toString();
    QString dataStr = args.value(QStringLiteral("data")).toString();

    bool ok;
    uint32_t addr = addrStr.toUInt(&ok, 0);
    if (!ok) {
        return makeToolError(QStringLiteral("Invalid address: ") + addrStr);
    }

    QByteArray data = QByteArray::fromHex(dataStr.toUtf8());
    if (data.isEmpty() && !dataStr.isEmpty()) {
        return makeToolError(QStringLiteral("Invalid hex data"));
    }

    void *ptr = virt_mem_ptr(addr, data.size());
    if (!ptr) {
        return makeToolError(QStringLiteral("Address 0x%1 is not accessible").arg(addr, 8, 16, QLatin1Char('0')));
    }

    memcpy(ptr, data.constData(), data.size());

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("address")] = QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0'));
    result[QStringLiteral("bytes_written")] = data.size();
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorDisassemble(const QJsonObject &args)
{
    QString addrStr = args.value(QStringLiteral("address")).toString();
    int count = args.value(QStringLiteral("count")).toInt(10);
    bool thumb = args.value(QStringLiteral("thumb")).toBool(false);

    uint32_t addr;
    if (addrStr.isEmpty()) {
        addr = arm.reg[15];
    } else {
        bool ok;
        addr = addrStr.toUInt(&ok, 0);
        if (!ok) {
            return makeToolError(QStringLiteral("Invalid address: ") + addrStr);
        }
    }

    uint32_t startAddr = addr;

    // Capture disassembly output from gui_debug_printf
    QString captured;
    EmuThread::beginCapture(captured);

    for (int i = 0; i < count; i++) {
        uint32_t len = thumb ? disasm_thumb_insn(addr) : disasm_arm_insn(addr);
        if (!len) {
            captured += QStringLiteral("0x%1: (inaccessible)\n").arg(addr, 8, 16, QLatin1Char('0'));
            break;
        }
        addr += len;
    }

    EmuThread::endCapture();

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("start_address")] = QStringLiteral("0x%1").arg(startAddr, 8, 16, QLatin1Char('0'));
    result[QStringLiteral("end_address")] = QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0'));
    result[QStringLiteral("count")] = count;
    result[QStringLiteral("mode")] = thumb ? QStringLiteral("thumb") : QStringLiteral("arm");
    result[QStringLiteral("disassembly")] = captured;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorGetRegisters()
{
    QJsonObject regs;
    for (int i = 0; i < 16; i++) {
        regs[QStringLiteral("r%1").arg(i)] = QStringLiteral("0x%1").arg(arm.reg[i], 8, 16, QLatin1Char('0'));
    }
    regs[QStringLiteral("cpsr")] = QStringLiteral("0x%1").arg(get_cpsr(), 8, 16, QLatin1Char('0'));

    QJsonObject result;
    result[QStringLiteral("registers")] = regs;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorSetRegister(const QJsonObject &args)
{
    QString regName = args.value(QStringLiteral("register")).toString().toLower();
    QString valueStr = args.value(QStringLiteral("value")).toString();

    bool ok;
    uint32_t value = valueStr.toUInt(&ok, 0);
    if (!ok) {
        return makeToolError(QStringLiteral("Invalid value: ") + valueStr);
    }

    int regNum = -1;
    if (regName.startsWith(QLatin1Char('r'))) {
        regNum = regName.mid(1).toInt(&ok);
        if (!ok || regNum < 0 || regNum > 15) {
            return makeToolError(QStringLiteral("Invalid register: ") + regName);
        }
    } else {
        return makeToolError(QStringLiteral("Invalid register: ") + regName + QStringLiteral(". Use r0-r15"));
    }

    arm.reg[regNum] = value;

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("register")] = regName;
    result[QStringLiteral("value")] = QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorSetBreakpoint(const QJsonObject &args)
{
    QString addrStr = args.value(QStringLiteral("address")).toString();
    QString type = args.value(QStringLiteral("type")).toString(QStringLiteral("exec"));

    bool ok;
    uint32_t addr = addrStr.toUInt(&ok, 0);
    if (!ok) {
        return makeToolError(QStringLiteral("Invalid address: ") + addrStr);
    }

    QString flags;
    if (type == QStringLiteral("exec") || type == QStringLiteral("x")) flags = QStringLiteral("+x");
    else if (type == QStringLiteral("read") || type == QStringLiteral("r")) flags = QStringLiteral("+r");
    else if (type == QStringLiteral("write") || type == QStringLiteral("w")) flags = QStringLiteral("+w");
    else {
        return makeToolError(QStringLiteral("Invalid breakpoint type: ") + type + QStringLiteral(". Use 'exec', 'read', or 'write'"));
    }

    // Use the debug command processor
    QString cmd = QStringLiteral("k 0x%1 %2").arg(addr, 8, 16, QLatin1Char('0')).arg(flags);
    QByteArray cmdBytes = cmd.toUtf8();
    char cmdline[256];
    strncpy(cmdline, cmdBytes.constData(), sizeof(cmdline) - 1);
    cmdline[sizeof(cmdline) - 1] = '\0';

    process_debug_cmd(cmdline);

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("address")] = QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0'));
    result[QStringLiteral("type")] = type;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorClearBreakpoint(const QJsonObject &args)
{
    QString addrStr = args.value(QStringLiteral("address")).toString();

    bool ok;
    uint32_t addr = addrStr.toUInt(&ok, 0);
    if (!ok) {
        return makeToolError(QStringLiteral("Invalid address: ") + addrStr);
    }

    // Clear all breakpoint types
    QString cmd = QStringLiteral("k 0x%1 -x-r-w").arg(addr, 8, 16, QLatin1Char('0'));
    QByteArray cmdBytes = cmd.toUtf8();
    char cmdline[256];
    strncpy(cmdline, cmdBytes.constData(), sizeof(cmdline) - 1);
    cmdline[sizeof(cmdline) - 1] = '\0';

    process_debug_cmd(cmdline);

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("address")] = QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0'));
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorStep()
{
    // Note: EVENT_DEBUG_STEP triggers the native debugger which blocks for
    // interactive input, making it incompatible with MCP. Instead, return
    // the current PC and disassembly. Use emulator_trace for multi-instruction
    // disassembly, or emulator_run_until to run to a specific address.
    bool thumb = (get_cpsr() & 0x20) != 0;
    QString disasm;
    EmuThread::beginCapture(disasm);
    uint32_t pc = arm.reg[15];
    if (thumb) {
        disasm_thumb_insn(pc);
    } else {
        disasm_arm_insn(pc);
    }
    EmuThread::endCapture();

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("pc")] = QStringLiteral("0x%1").arg(arm.reg[15], 8, 16, QLatin1Char('0'));
    result[QStringLiteral("disassembly")] = disasm;
    result[QStringLiteral("note")] = QStringLiteral("Single-step execution is not available via MCP. Use emulator_run_until to run to a target address.");
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorContinue()
{
    if (emu_thread.isPaused()) {
        emu_thread.setPaused(false);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorExecuteProgram(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString path = args.value(QStringLiteral("path")).toString();

    QString cmd = QStringLiteral("exec %1").arg(path);
    QByteArray cmdBytes = cmd.toUtf8();
    char cmdline[512];
    strncpy(cmdline, cmdBytes.constData(), sizeof(cmdline) - 1);
    cmdline[sizeof(cmdline) - 1] = '\0';

    int ret = process_debug_cmd(cmdline);

    QJsonObject result;
    result[QStringLiteral("success")] = (ret == 1);
    result[QStringLiteral("path")] = path;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorSaveSnapshot(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString path = args.value(QStringLiteral("path")).toString();

    emu_thread.suspend(path);

    // Wait a bit for suspend to complete
    QThread::msleep(500);

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("path")] = path;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorLoadSnapshot(const QJsonObject &args)
{
    QString path = args.value(QStringLiteral("path")).toString();

    if (!QFile::exists(path)) {
        return makeToolError(QStringLiteral("Snapshot file does not exist: ") + path);
    }

    bool success = emu_thread.resume(path);

    QJsonObject result;
    result[QStringLiteral("success")] = success;
    result[QStringLiteral("path")] = path;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// ==================== New High-Level Tools ====================

// --- Arrow key simulation via touchpad swipe ---

void MCPServer::simulateArrow(const QString &direction)
{
    // Replicate the QtKeypadBridge approach: set touchpad position directly
    // at extreme + contact=down=true, then release.
    // This is how the GUI handles Qt arrow keys for the CX touchpad.

    if (direction == QStringLiteral("right")) {
        keypad.touchpad_x = TOUCHPAD_X_MAX;
        keypad.touchpad_y = TOUCHPAD_Y_MAX / 2;
    } else if (direction == QStringLiteral("left")) {
        keypad.touchpad_x = 0;
        keypad.touchpad_y = TOUCHPAD_Y_MAX / 2;
    } else if (direction == QStringLiteral("up")) {
        keypad.touchpad_x = TOUCHPAD_X_MAX / 2;
        keypad.touchpad_y = TOUCHPAD_Y_MAX;
    } else if (direction == QStringLiteral("down")) {
        keypad.touchpad_x = TOUCHPAD_X_MAX / 2;
        keypad.touchpad_y = 0;
    }

    // Press
    keypad.touchpad_contact = true;
    keypad.touchpad_down = true;
    keypad.kpc.gpio_int_active |= 0x800;
    keypad_int_check();

    QThread::msleep(30);

    // Release
    keypad.touchpad_contact = false;
    keypad.touchpad_down = false;
    keypad.kpc.gpio_int_active |= 0x800;
    keypad_int_check();

    QThread::msleep(50);
}

QJsonObject MCPServer::toolEmulatorArrow(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString direction = args.value(QStringLiteral("direction")).toString().toLower();
    int count = args.value(QStringLiteral("count")).toInt(1);

    if (direction != QStringLiteral("up") && direction != QStringLiteral("down") &&
        direction != QStringLiteral("left") && direction != QStringLiteral("right")) {
        return makeToolError(QStringLiteral("Invalid direction: ") + direction + QStringLiteral(". Use 'up', 'down', 'left', 'right'"));
    }

    if (count < 1) count = 1;
    if (count > 50) count = 50;

    for (int i = 0; i < count; i++) {
        simulateArrow(direction);
        if (i < count - 1) {
            QThread::msleep(30);
        }
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("direction")] = direction;
    result[QStringLiteral("count")] = count;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// --- Screen stability detection ---

bool MCPServer::waitForStableScreen(int timeoutMs, int stableMs, int intervalMs, QString *screenshotPath)
{
    QImage prevImage;
    int stableElapsed = 0;
    int totalElapsed = 0;

    while (totalElapsed < timeoutMs) {
        QImage currentImage = renderFramebuffer();
        if (currentImage.isNull()) {
            QThread::msleep(intervalMs);
            totalElapsed += intervalMs;
            continue;
        }

        if (!prevImage.isNull() && prevImage == currentImage) {
            stableElapsed += intervalMs;
            if (stableElapsed >= stableMs) {
                // Screen is stable — save screenshot if requested
                if (screenshotPath) {
                    *screenshotPath = QStringLiteral("/tmp/firebird_mcp_stable_%1.png")
                                        .arg(QDateTime::currentMSecsSinceEpoch());
                    currentImage.save(*screenshotPath, "PNG");
                }
                return true;
            }
        } else {
            stableElapsed = 0;
        }

        prevImage = currentImage;
        QThread::msleep(intervalMs);
        totalElapsed += intervalMs;
    }

    // Timed out — still save last screenshot if requested
    if (screenshotPath && !prevImage.isNull()) {
        *screenshotPath = QStringLiteral("/tmp/firebird_mcp_stable_%1.png")
                            .arg(QDateTime::currentMSecsSinceEpoch());
        prevImage.save(*screenshotPath, "PNG");
    }

    return false;
}

bool MCPServer::pressKeyAndWaitForTransition(const QString &keyName, int changeTimeoutMs, int stableMs, int intervalMs, QString *screenshotPath)
{
    // Capture pre-keypress screen
    QImage preImage = renderFramebuffer();

    // Press the key
    KeyPosition pos = getKeyPosition(keyName);
    if (pos.valid) {
        keypad_set_key(pos.row, pos.col, true);
        QThread::msleep(50);
        keypad_set_key(pos.row, pos.col, false);
    }

    // Wait for screen to change from pre-keypress state
    int elapsed = 0;
    while (elapsed < changeTimeoutMs) {
        QThread::msleep(intervalMs);
        elapsed += intervalMs;
        QImage currentImage = renderFramebuffer();
        if (!currentImage.isNull() && currentImage != preImage) {
            // Screen changed! Now wait for it to stabilize
            return waitForStableScreen(changeTimeoutMs - elapsed, stableMs, intervalMs, screenshotPath);
        }
    }

    // Screen never changed (maybe already on target screen) — save screenshot anyway
    if (screenshotPath && !preImage.isNull()) {
        *screenshotPath = QStringLiteral("/tmp/firebird_mcp_stable_%1.png")
                            .arg(QDateTime::currentMSecsSinceEpoch());
        preImage.save(*screenshotPath, "PNG");
    }
    return true; // Screen was stable the whole time
}

QJsonObject MCPServer::toolEmulatorWaitStable(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    int timeoutMs = args.value(QStringLiteral("timeout_ms")).toInt(3000);
    int stableMs = args.value(QStringLiteral("stable_ms")).toInt(300);
    int intervalMs = args.value(QStringLiteral("interval_ms")).toInt(50);

    QString screenshotPath;
    bool stable = waitForStableScreen(timeoutMs, stableMs, intervalMs, &screenshotPath);

    QJsonObject result;
    result[QStringLiteral("stable")] = stable;
    result[QStringLiteral("screenshot")] = screenshotPath;
    if (!stable) {
        result[QStringLiteral("warning")] = QStringLiteral("Screen did not stabilize within timeout");
    }
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// --- Navigation macros ---

QJsonObject MCPServer::toolEmulatorNavigate(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString target = args.value(QStringLiteral("target")).toString().toLower();
    QString screenshotPath;

    if (target == QStringLiteral("home")) {
        pressKeyAndWaitForTransition(QStringLiteral("on"), 3000, 300, 50, &screenshotPath);
    } else if (target == QStringLiteral("scratchpad_calc")) {
        pressKeyAndWaitForTransition(QStringLiteral("on"), 3000, 300, 50);
        pressKeyAndWaitForTransition(QStringLiteral("a"), 3000, 300, 50, &screenshotPath);
    } else if (target == QStringLiteral("scratchpad_graph")) {
        pressKeyAndWaitForTransition(QStringLiteral("on"), 3000, 300, 50);
        pressKeyAndWaitForTransition(QStringLiteral("b"), 3000, 300, 50, &screenshotPath);
    } else if (target == QStringLiteral("my_documents")) {
        pressKeyAndWaitForTransition(QStringLiteral("on"), 3000, 300, 50);
        // My Documents is the default selected item on the home screen — just press Enter
        pressKeyAndWaitForTransition(QStringLiteral("enter"), 3000, 300, 50, &screenshotPath);
    } else if (target == QStringLiteral("back")) {
        pressKeyAndWaitForTransition(QStringLiteral("esc"), 3000, 300, 50, &screenshotPath);
    } else {
        return makeToolError(QStringLiteral("Unknown target: ") + target + QStringLiteral(". Use 'home', 'scratchpad_calc', 'scratchpad_graph', 'my_documents', 'back'"));
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("target")] = target;
    result[QStringLiteral("screenshot")] = screenshotPath;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorOpenDocument(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString filename = args.value(QStringLiteral("filename")).toString();
    if (filename.isEmpty()) {
        return makeToolError(QStringLiteral("Filename is required"));
    }

    // Navigate to My Documents (Enter opens the default-selected My Documents item)
    pressKeyAndWaitForTransition(QStringLiteral("on"), 3000, 300, 50);
    pressKeyAndWaitForTransition(QStringLiteral("enter"), 3000, 300, 50);

    // Type first letter to jump to file, then wait for highlight to settle
    QString firstLetter = filename.left(1).toLower();
    pressKeyAndWaitForTransition(firstLetter, 2000, 200, 50);

    // Press enter to open — wait longer for document to load
    QString screenshotPath;
    pressKeyAndWaitForTransition(QStringLiteral("enter"), 5000, 500, 50, &screenshotPath);

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("filename")] = filename;
    result[QStringLiteral("screenshot")] = screenshotPath;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// --- Math expression input ---

QJsonObject MCPServer::toolEmulatorInputExpr(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString expression = args.value(QStringLiteral("expression")).toString();
    QString context = args.value(QStringLiteral("context")).toString(QStringLiteral("text"));

    auto pressKey = [](const QString &keyName, int durationMs = 30) {
        KeyPosition pos = getKeyPosition(keyName);
        if (pos.valid) {
            keypad_set_key(pos.row, pos.col, true);
            QThread::msleep(durationMs);
            keypad_set_key(pos.row, pos.col, false);
            QThread::msleep(20);
        }
    };

    if (context == QStringLiteral("text")) {
        // Character-by-character input for text contexts (console, polycalc)
        for (int i = 0; i < expression.length(); i++) {
            QChar ch = expression[i];
            if (ch == QLatin1Char('+'))        pressKey(QStringLiteral("plus"));
            else if (ch == QLatin1Char('-'))    pressKey(QStringLiteral("minus"));
            else if (ch == QLatin1Char('*'))    pressKey(QStringLiteral("multiply"));
            else if (ch == QLatin1Char('/'))    pressKey(QStringLiteral("divide"));
            else if (ch == QLatin1Char('^'))    pressKey(QStringLiteral("power"));
            else if (ch == QLatin1Char('('))    pressKey(QStringLiteral("leftparen"));
            else if (ch == QLatin1Char(')'))    pressKey(QStringLiteral("rightparen"));
            else if (ch == QLatin1Char(' '))    pressKey(QStringLiteral("space"));
            else if (ch == QLatin1Char('\n'))   pressKey(QStringLiteral("enter"));
            else if (ch == QLatin1Char(','))    pressKey(QStringLiteral("comma"));
            else if (ch == QLatin1Char('.'))    pressKey(QStringLiteral("dot"));
            else if (ch == QLatin1Char('='))    pressKey(QStringLiteral("equals"));
            else {
                // Letters and digits
                QString keyName = QString(ch.toLower());
                KeyPosition pos = getKeyPosition(keyName);
                if (pos.valid) {
                    keypad_set_key(pos.row, pos.col, true);
                    QThread::msleep(30);
                    keypad_set_key(pos.row, pos.col, false);
                    QThread::msleep(20);
                }
            }
        }
    } else if (context == QStringLiteral("math2d")) {
        // 2D editor input — handle ^{} for exponents with arrow-right to exit
        int i = 0;
        while (i < expression.length()) {
            QChar ch = expression[i];

            if (ch == QLatin1Char('^')) {
                // Press power key to enter superscript mode
                pressKey(QStringLiteral("power"));
                i++;

                if (i < expression.length() && expression[i] == QLatin1Char('{')) {
                    // ^{expr} — type everything until matching }
                    i++; // skip {
                    int depth = 1;
                    while (i < expression.length() && depth > 0) {
                        if (expression[i] == QLatin1Char('{')) depth++;
                        else if (expression[i] == QLatin1Char('}')) {
                            depth--;
                            if (depth == 0) { i++; break; }
                        }
                        // Type the character (recursive handling of nested ^)
                        QChar ic = expression[i];
                        if (ic == QLatin1Char('+'))        pressKey(QStringLiteral("plus"));
                        else if (ic == QLatin1Char('-'))    pressKey(QStringLiteral("minus"));
                        else if (ic == QLatin1Char('*'))    pressKey(QStringLiteral("multiply"));
                        else if (ic == QLatin1Char('/'))    pressKey(QStringLiteral("divide"));
                        else if (ic == QLatin1Char('('))    pressKey(QStringLiteral("leftparen"));
                        else if (ic == QLatin1Char(')'))    pressKey(QStringLiteral("rightparen"));
                        else {
                            QString keyName = QString(ic.toLower());
                            KeyPosition pos = getKeyPosition(keyName);
                            if (pos.valid) {
                                keypad_set_key(pos.row, pos.col, true);
                                QThread::msleep(30);
                                keypad_set_key(pos.row, pos.col, false);
                                QThread::msleep(20);
                            }
                        }
                        i++;
                    }
                } else if (i < expression.length()) {
                    // ^N — single character exponent
                    QChar ec = expression[i];
                    QString keyName = QString(ec.toLower());
                    KeyPosition pos = getKeyPosition(keyName);
                    if (pos.valid) {
                        keypad_set_key(pos.row, pos.col, true);
                        QThread::msleep(30);
                        keypad_set_key(pos.row, pos.col, false);
                        QThread::msleep(20);
                    }
                    i++;
                }
                // Arrow-right to exit superscript
                simulateArrow(QStringLiteral("right"));
            } else if (ch == QLatin1Char('+'))    { pressKey(QStringLiteral("plus")); i++; }
            else if (ch == QLatin1Char('-'))       { pressKey(QStringLiteral("minus")); i++; }
            else if (ch == QLatin1Char('*'))       { pressKey(QStringLiteral("multiply")); i++; }
            else if (ch == QLatin1Char('/'))       { pressKey(QStringLiteral("divide")); i++; }
            else if (ch == QLatin1Char('('))       { pressKey(QStringLiteral("leftparen")); i++; }
            else if (ch == QLatin1Char(')'))       { pressKey(QStringLiteral("rightparen")); i++; }
            else if (ch == QLatin1Char(' '))       { pressKey(QStringLiteral("space")); i++; }
            else if (ch == QLatin1Char(','))       { pressKey(QStringLiteral("comma")); i++; }
            else if (ch == QLatin1Char('.'))       { pressKey(QStringLiteral("dot")); i++; }
            else if (ch == QLatin1Char('='))       { pressKey(QStringLiteral("equals")); i++; }
            else {
                QString keyName = QString(ch.toLower());
                KeyPosition pos = getKeyPosition(keyName);
                if (pos.valid) {
                    keypad_set_key(pos.row, pos.col, true);
                    QThread::msleep(30);
                    keypad_set_key(pos.row, pos.col, false);
                    QThread::msleep(20);
                }
                i++;
            }
        }
    } else {
        return makeToolError(QStringLiteral("Invalid context: ") + context + QStringLiteral(". Use 'text' or 'math2d'"));
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("expression")] = expression;
    result[QStringLiteral("context")] = context;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// --- Memory search ---

QJsonObject MCPServer::toolEmulatorSearchMemory(const QJsonObject &args)
{
    QString patternStr = args.value(QStringLiteral("pattern")).toString();
    QString startStr = args.value(QStringLiteral("start_address")).toString();
    QString endStr = args.value(QStringLiteral("end_address")).toString();
    int maxResults = args.value(QStringLiteral("max_results")).toInt(20);

    bool ok;
    uint32_t startAddr = startStr.toUInt(&ok, 0);
    if (!ok) return makeToolError(QStringLiteral("Invalid start_address: ") + startStr);

    uint32_t endAddr = endStr.toUInt(&ok, 0);
    if (!ok) return makeToolError(QStringLiteral("Invalid end_address: ") + endStr);

    if (endAddr <= startAddr) return makeToolError(QStringLiteral("end_address must be greater than start_address"));

    QByteArray pattern = QByteArray::fromHex(patternStr.toUtf8());
    if (pattern.isEmpty()) return makeToolError(QStringLiteral("Invalid hex pattern"));

    QJsonArray matches;
    uint32_t chunkSize = 4096;

    for (uint32_t addr = startAddr; addr < endAddr && matches.size() < maxResults; addr += chunkSize) {
        uint32_t remaining = endAddr - addr;
        uint32_t thisChunk = (remaining < chunkSize) ? remaining : chunkSize;

        void *ptr = virt_mem_ptr(addr, thisChunk);
        if (!ptr) continue;

        const uint8_t *data = static_cast<const uint8_t*>(ptr);
        for (uint32_t offset = 0; offset + (uint32_t)pattern.size() <= thisChunk; offset++) {
            if (memcmp(data + offset, pattern.constData(), pattern.size()) == 0) {
                matches.append(QStringLiteral("0x%1").arg(addr + offset, 8, 16, QLatin1Char('0')));
                if (matches.size() >= maxResults) break;
            }
        }
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("pattern")] = patternStr;
    result[QStringLiteral("matches_count")] = matches.size();
    result[QStringLiteral("matches")] = matches;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// --- Read string from memory ---

QJsonObject MCPServer::toolEmulatorReadString(const QJsonObject &args)
{
    QString addrStr = args.value(QStringLiteral("address")).toString();
    QString encoding = args.value(QStringLiteral("encoding")).toString(QStringLiteral("ascii"));
    int maxLength = args.value(QStringLiteral("max_length")).toInt(256);

    bool ok;
    uint32_t addr = addrStr.toUInt(&ok, 0);
    if (!ok) return makeToolError(QStringLiteral("Invalid address: ") + addrStr);

    if (maxLength < 1) maxLength = 1;
    if (maxLength > 4096) maxLength = 4096;

    void *ptr = virt_mem_ptr(addr, maxLength);
    if (!ptr) return makeToolError(QStringLiteral("Address 0x%1 is not accessible").arg(addr, 8, 16, QLatin1Char('0')));

    QString str;
    if (encoding == QStringLiteral("ascii")) {
        const char *cptr = static_cast<const char*>(ptr);
        int len = 0;
        while (len < maxLength && cptr[len] != '\0') len++;
        str = QString::fromLatin1(cptr, len);
    } else if (encoding == QStringLiteral("utf16")) {
        const char16_t *u16ptr = static_cast<const char16_t*>(ptr);
        int maxChars = maxLength / 2;
        int len = 0;
        while (len < maxChars && u16ptr[len] != 0) len++;
        str = QString::fromUtf16(u16ptr, len);
    } else {
        return makeToolError(QStringLiteral("Invalid encoding: ") + encoding + QStringLiteral(". Use 'ascii' or 'utf16'"));
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("address")] = QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0'));
    result[QStringLiteral("encoding")] = encoding;
    result[QStringLiteral("length")] = str.length();
    result[QStringLiteral("string")] = str;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// --- Run until breakpoint ---

QJsonObject MCPServer::toolEmulatorRunUntil(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString addrStr = args.value(QStringLiteral("address")).toString();
    int timeoutMs = args.value(QStringLiteral("timeout_ms")).toInt(5000);
    QString type = args.value(QStringLiteral("type")).toString(QStringLiteral("exec"));

    bool ok;
    uint32_t addr = addrStr.toUInt(&ok, 0);
    if (!ok) return makeToolError(QStringLiteral("Invalid address: ") + addrStr);

    // Determine breakpoint flags
    QString flags;
    if (type == QStringLiteral("exec")) flags = QStringLiteral("+x");
    else if (type == QStringLiteral("read")) flags = QStringLiteral("+r");
    else if (type == QStringLiteral("write")) flags = QStringLiteral("+w");
    else return makeToolError(QStringLiteral("Invalid type: ") + type);

    // Set breakpoint
    QString setCmd = QStringLiteral("k 0x%1 %2").arg(addr, 8, 16, QLatin1Char('0')).arg(flags);
    QByteArray setCmdBytes = setCmd.toUtf8();
    char cmdline[256];
    strncpy(cmdline, setCmdBytes.constData(), sizeof(cmdline) - 1);
    cmdline[sizeof(cmdline) - 1] = '\0';
    process_debug_cmd(cmdline);

    // Resume execution if paused
    if (emu_thread.isPaused()) {
        emu_thread.setPaused(false);
    }

    // Poll for breakpoint hit (check if emulator gets paused)
    int elapsed = 0;
    int pollInterval = 10;
    bool hit = false;
    while (elapsed < timeoutMs) {
        QThread::msleep(pollInterval);
        elapsed += pollInterval;
        if (emu_thread.isPaused()) {
            hit = true;
            break;
        }
        // Also check if CPU entered debugger
        if (in_debugger) {
            hit = true;
            break;
        }
    }

    if (!hit) {
        // Timeout — pause emulator and clean up
        emu_thread.setPaused(true);
        QThread::msleep(50);
    }

    // Capture registers
    QJsonObject regs;
    for (int i = 0; i < 16; i++) {
        regs[QStringLiteral("r%1").arg(i)] = QStringLiteral("0x%1").arg(arm.reg[i], 8, 16, QLatin1Char('0'));
    }
    regs[QStringLiteral("cpsr")] = QStringLiteral("0x%1").arg(get_cpsr(), 8, 16, QLatin1Char('0'));

    // Capture disassembly at PC
    bool thumb = (get_cpsr() & 0x20) != 0;
    QString disasm;
    EmuThread::beginCapture(disasm);
    uint32_t pc = arm.reg[15];
    for (int i = 0; i < 5; i++) {
        uint32_t len = thumb ? disasm_thumb_insn(pc) : disasm_arm_insn(pc);
        if (!len) break;
        pc += len;
    }
    EmuThread::endCapture();

    // Clear the breakpoint
    QString clearCmd = QStringLiteral("k 0x%1 -x-r-w").arg(addr, 8, 16, QLatin1Char('0'));
    QByteArray clearCmdBytes = clearCmd.toUtf8();
    strncpy(cmdline, clearCmdBytes.constData(), sizeof(cmdline) - 1);
    cmdline[sizeof(cmdline) - 1] = '\0';
    process_debug_cmd(cmdline);

    QJsonObject result;
    result[QStringLiteral("hit")] = hit;
    result[QStringLiteral("address")] = QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0'));
    result[QStringLiteral("pc")] = QStringLiteral("0x%1").arg(arm.reg[15], 8, 16, QLatin1Char('0'));
    result[QStringLiteral("registers")] = regs;
    result[QStringLiteral("disassembly")] = disasm;
    if (!hit) {
        result[QStringLiteral("warning")] = QStringLiteral("Breakpoint was not hit within timeout");
    }
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// --- Execution trace ---

QJsonObject MCPServer::toolEmulatorTrace(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    int count = args.value(QStringLiteral("count")).toInt(10);
    if (count < 1) count = 1;
    if (count > 1000) count = 1000;

    // Auto-detect Thumb mode from CPSR if not specified
    bool thumb;
    if (args.contains(QStringLiteral("thumb"))) {
        thumb = args.value(QStringLiteral("thumb")).toBool();
    } else {
        thumb = (get_cpsr() & 0x20) != 0;
    }

    // Pause the emulator to get a consistent snapshot
    bool wasPaused = emu_thread.isPaused();
    if (!wasPaused) {
        emu_thread.setPaused(true);
        QThread::msleep(50);
    }

    // Linear disassembly from current PC
    // Note: This shows upcoming instructions, not executed ones.
    // True execution tracing would require deeper debugger integration.
    uint32_t pc = arm.reg[15];
    QString trace;
    EmuThread::beginCapture(trace);
    for (int i = 0; i < count; i++) {
        uint32_t len = thumb ? disasm_thumb_insn(pc) : disasm_arm_insn(pc);
        if (!len) {
            trace += QStringLiteral("0x%1: (inaccessible)\n").arg(pc, 8, 16, QLatin1Char('0'));
            break;
        }
        pc += len;
    }
    EmuThread::endCapture();

    // Restore original pause state
    if (!wasPaused) {
        emu_thread.setPaused(false);
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("count")] = count;
    result[QStringLiteral("mode")] = thumb ? QStringLiteral("thumb") : QStringLiteral("arm");
    result[QStringLiteral("pc")] = QStringLiteral("0x%1").arg(arm.reg[15], 8, 16, QLatin1Char('0'));
    result[QStringLiteral("trace")] = trace;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// --- Function prologue scanner ---

QJsonObject MCPServer::toolEmulatorFindFunctions(const QJsonObject &args)
{
    QString startStr = args.value(QStringLiteral("start_address")).toString();
    QString endStr = args.value(QStringLiteral("end_address")).toString();

    bool ok;
    uint32_t startAddr = startStr.toUInt(&ok, 0);
    if (!ok) return makeToolError(QStringLiteral("Invalid start_address: ") + startStr);

    uint32_t endAddr = endStr.toUInt(&ok, 0);
    if (!ok) return makeToolError(QStringLiteral("Invalid end_address: ") + endStr);

    if (endAddr <= startAddr) return makeToolError(QStringLiteral("end_address must be greater than start_address"));

    // Limit scan range to 16MB to avoid long delays
    if (endAddr - startAddr > 0x1000000) {
        return makeToolError(QStringLiteral("Scan range too large (max 16MB). Try a smaller range."));
    }

    QJsonArray armFunctions;
    QJsonArray thumbFunctions;
    uint32_t chunkSize = 4096;

    for (uint32_t addr = startAddr; addr < endAddr; addr += chunkSize) {
        uint32_t remaining = endAddr - addr;
        uint32_t thisChunk = (remaining < chunkSize) ? remaining : chunkSize;

        void *ptr = virt_mem_ptr(addr, thisChunk);
        if (!ptr) continue;

        const uint8_t *data = static_cast<const uint8_t*>(ptr);

        // Scan for ARM STMFD SP!, {…, LR}: E92D.x.. where bit 14 is set
        // Pattern: byte[3]=0xE9, byte[2]=0x2D, byte[1] has bit 6 set (bit 14 of halfword)
        for (uint32_t offset = 0; offset + 4 <= thisChunk; offset += 4) {
            uint32_t insn = data[offset] | (data[offset+1] << 8) | (data[offset+2] << 16) | (data[offset+3] << 24);
            // STMFD SP!, {…, LR} = STMDB SP!, {…, LR}
            // Encoding: cond 1001 0010 1101 register_list
            // cond=E (always), so top byte = 0xE9, next nibble = 0x2D
            // LR is bit 14 of register_list
            if ((insn & 0xFFFF0000) == 0xE92D0000 && (insn & 0x4000)) {
                armFunctions.append(QStringLiteral("0x%1").arg(addr + offset, 8, 16, QLatin1Char('0')));
            }
        }

        // Scan for Thumb PUSH {…, LR}: B5xx (bit 8 set = LR included)
        for (uint32_t offset = 0; offset + 2 <= thisChunk; offset += 2) {
            uint16_t insn = data[offset] | (data[offset+1] << 8);
            if ((insn & 0xFF00) == 0xB500) {
                thumbFunctions.append(QStringLiteral("0x%1").arg(addr + offset, 8, 16, QLatin1Char('0')));
            }
        }
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("arm_functions")] = armFunctions;
    result[QStringLiteral("arm_count")] = armFunctions.size();
    result[QStringLiteral("thumb_functions")] = thumbFunctions;
    result[QStringLiteral("thumb_count")] = thumbFunctions.size();
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// --- Deploy and run ---

QJsonObject MCPServer::toolEmulatorDeployAndRun(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString localPath = args.value(QStringLiteral("local_path")).toString();
    QString calcPath = args.value(QStringLiteral("calc_path")).toString();
    bool waitForExit = args.value(QStringLiteral("wait_for_exit")).toBool(true);

    if (!QFile::exists(localPath)) {
        return makeToolError(QStringLiteral("Local file does not exist: ") + localPath);
    }

    // Step 1: Upload file
    m_asyncMutex.lock();
    m_asyncComplete = false;
    m_asyncError.clear();
    m_asyncProgress = 0;
    m_asyncMutex.unlock();

    usblink_queue_put_file(localPath.toStdString(), calcPath.toStdString(), progress_callback, this);

    m_asyncMutex.lock();
    bool completed = m_asyncCondition.wait(&m_asyncMutex, 60000);
    m_asyncMutex.unlock();

    if (!completed) {
        return makeToolError(QStringLiteral("File upload timed out"));
    }
    if (!m_asyncError.isEmpty()) {
        return makeToolError(QStringLiteral("Upload failed: ") + m_asyncError);
    }

    // Wait for "Document Received" dialog, then dismiss it
    waitForStableScreen(3000, 300, 50);
    pressKeyAndWaitForTransition(QStringLiteral("enter"), 3000, 300, 50);

    // After dismissing dialog we're on the home screen with My Documents selected.
    // Go directly to My Documents (skip "on" press — we're already on home).
    pressKeyAndWaitForTransition(QStringLiteral("enter"), 3000, 300, 50);

    // Extract filename from calc path
    QString filename = calcPath;
    int lastSlash = filename.lastIndexOf(QLatin1Char('/'));
    if (lastSlash >= 0) filename = filename.mid(lastSlash + 1);
    // Remove .tns extension
    if (filename.endsWith(QStringLiteral(".tns"), Qt::CaseInsensitive)) {
        filename = filename.left(filename.length() - 4);
    }

    // Type first letter to jump to file
    QString firstLetter = filename.left(1).toLower();
    pressKeyAndWaitForTransition(firstLetter, 2000, 200, 50);

    // Press enter to open the document
    pressKeyAndWaitForTransition(QStringLiteral("enter"), 5000, 500, 50);

    // Wait for program to fully load
    QString screenshotPath;
    int stableMs = waitForExit ? 1000 : 300;
    waitForStableScreen(waitForExit ? 10000 : 3000, stableMs, 100, &screenshotPath);

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("local_path")] = localPath;
    result[QStringLiteral("calc_path")] = calcPath;
    result[QStringLiteral("screenshot")] = screenshotPath;
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

QJsonObject MCPServer::toolEmulatorOcrCalibrate(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QString knownText = args.value(QStringLiteral("known_text")).toString();

    if (knownText.isEmpty()) {
        return makeToolError(QStringLiteral(
            "Provide 'known_text' matching what's currently on the first visible "
            "text line of the screen. Include ALL characters exactly as displayed. "
            "Tip: type a known string into the REPL first, e.g. all printable ASCII."));
    }

    QImage image = renderFramebuffer();
    if (image.isNull()) {
        return makeToolError(QStringLiteral("Failed to capture framebuffer"));
    }

    image = image.convertToFormat(QImage::Format_ARGB32);

    QVector<bool> binary = framebufferToBinary(image);
    FontMetrics fm = detectFontMetrics(binary, 320, 240);

    if (!fm.valid) {
        return makeToolError(QStringLiteral("Could not detect font metrics. Is there text on screen?"));
    }

    m_ocrCharWidth = fm.charWidth;
    m_ocrCharHeight = fm.charHeight;
    m_ocrBaselineX = fm.startX;
    m_ocrBaselineY = fm.startY;

    m_ocrGlyphTable.clear();
    int mapped = 0;

    for (int i = 0; i < knownText.length() && i < fm.cols; ++i) {
        char ch = knownText.at(i).toLatin1();
        if (ch == 0) continue;

        int cellX = fm.startX + i * fm.charWidth;
        int cellY = fm.startY;

        QByteArray glyph = extractGlyph(binary, 320, cellX, cellY, fm.charWidth, fm.charHeight);

        bool allEmpty = true;
        for (char b : glyph) { if (b != 0) { allEmpty = false; break; } }

        if (ch == ' ') {
            m_ocrGlyphTable[glyph] = ' ';
            mapped++;
        } else if (!allEmpty) {
            m_ocrGlyphTable[glyph] = ch;
            mapped++;
        }
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("char_width")] = fm.charWidth;
    result[QStringLiteral("char_height")] = fm.charHeight;
    result[QStringLiteral("start_x")] = fm.startX;
    result[QStringLiteral("start_y")] = fm.startY;
    result[QStringLiteral("cols")] = fm.cols;
    result[QStringLiteral("rows")] = fm.rows;
    result[QStringLiteral("glyphs_mapped")] = mapped;
    result[QStringLiteral("total_unique_glyphs")] = m_ocrGlyphTable.size();
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

QJsonObject MCPServer::toolEmulatorOcr(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    QImage image = renderFramebuffer();
    if (image.isNull()) {
        return makeToolError(QStringLiteral("Failed to capture framebuffer"));
    }

    image = image.convertToFormat(QImage::Format_ARGB32);
    QVector<bool> binary = framebufferToBinary(image);

    int charW, charH, startX, startY;

    if (m_ocrCharWidth > 0) {
        charW = m_ocrCharWidth;
        charH = m_ocrCharHeight;
        startX = m_ocrBaselineX;
        startY = m_ocrBaselineY;
    } else {
        FontMetrics fm = detectFontMetrics(binary, 320, 240);
        if (!fm.valid) {
            return makeToolError(QStringLiteral(
                "Could not detect text. Run emulator_ocr_calibrate first, "
                "or ensure there is text on screen."));
        }
        charW = fm.charWidth;
        charH = fm.charHeight;
        startX = fm.startX;
        startY = fm.startY;
    }

    int cols = (320 - startX) / charW;
    int rows = (240 - startY) / charH;

    int regionTop = args.value(QStringLiteral("line_start")).toInt(0);
    int regionLines = args.value(QStringLiteral("line_count")).toInt(rows);
    if (regionTop + regionLines > rows) regionLines = rows - regionTop;

    QStringList lines;
    int unknownCount = 0;

    for (int row = regionTop; row < regionTop + regionLines; ++row) {
        QString line;
        int cellY = startY + row * charH;

        for (int col = 0; col < cols; ++col) {
            int cellX = startX + col * charW;

            QByteArray glyph = extractGlyph(binary, 320, cellX, cellY, charW, charH);

            bool allEmpty = true;
            for (char b : glyph) { if (b != 0) { allEmpty = false; break; } }

            if (allEmpty) {
                line += QLatin1Char(' ');
            } else if (m_ocrGlyphTable.contains(glyph)) {
                line += QLatin1Char(m_ocrGlyphTable[glyph]);
            } else {
                line += QLatin1Char('?');
                unknownCount++;
            }
        }

        while (line.endsWith(QLatin1Char(' '))) line.chop(1);
        lines.append(line);
    }

    while (!lines.isEmpty() && lines.last().isEmpty()) lines.removeLast();

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("text")] = lines.join(QStringLiteral("\n"));
    result[QStringLiteral("lines")] = lines.size();
    result[QStringLiteral("cols")] = cols;
    result[QStringLiteral("char_size")] = QStringLiteral("%1x%2").arg(charW).arg(charH);
    if (unknownCount > 0) {
        result[QStringLiteral("unknown_glyphs")] = unknownCount;
        result[QStringLiteral("hint")] = QStringLiteral("Run emulator_ocr_calibrate with known text to improve recognition");
    }
    result[QStringLiteral("calibrated")] = (m_ocrCharWidth > 0);
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// ============ GDB RSP protocol helpers ============

static const char hexLut[] = "0123456789abcdef";

void MCPServer::gdbSendPacket(const QByteArray &payload)
{
    uint8_t cksum = 0;
    for (char c : payload) cksum += static_cast<uint8_t>(c);

    QByteArray pkt;
    pkt.reserve(payload.size() + 4);
    pkt.append('$');
    pkt.append(payload);
    pkt.append('#');
    pkt.append(hexLut[cksum >> 4]);
    pkt.append(hexLut[cksum & 0xf]);

    m_gdbSocket->write(pkt);
    m_gdbSocket->flush();
}

QByteArray MCPServer::gdbReadPacket(int timeoutMs)
{
    QByteArray buf;
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        if (m_gdbSocket->bytesAvailable() > 0 || m_gdbSocket->waitForReadyRead(50)) {
            buf.append(m_gdbSocket->readAll());
        }

        // Skip ack bytes
        while (!buf.isEmpty() && (buf[0] == '+' || buf[0] == '-'))
            buf.remove(0, 1);

        // Look for $payload#XX
        int start = buf.indexOf('$');
        if (start >= 0) {
            int hash = buf.indexOf('#', start + 1);
            if (hash >= 0 && hash + 2 < buf.size()) {
                QByteArray payload = buf.mid(start + 1, hash - start - 1);
                // Ack the packet
                m_gdbSocket->write("+", 1);
                m_gdbSocket->flush();
                return payload;
            }
        }
    }
    return QByteArray();
}

QByteArray MCPServer::gdbCommand(const QByteArray &cmd, int timeoutMs)
{
    gdbSendPacket(cmd);

    QByteArray resp = gdbReadPacket(timeoutMs);

    // If we got a stop reply (CPU just halted), absorb it and read the actual response
    while (!resp.isEmpty() && (resp[0] == 'T' || resp[0] == 'S' || resp[0] == 'W')) {
        resp = gdbReadPacket(timeoutMs);
    }

    return resp;
}

// ============ GDB tool: connect ============

QJsonObject MCPServer::toolEmulatorGdbConnect(const QJsonObject &args)
{
    if (!emu_thread.isRunning()) {
        return makeToolError(QStringLiteral("Emulator is not running"));
    }

    int port = args.value(QStringLiteral("port")).toInt(3333);

    // Clean up existing connection
    if (m_gdbSocket) {
        m_gdbSocket->disconnectFromHost();
        delete m_gdbSocket;
        m_gdbSocket = nullptr;
    }

    m_gdbSocket = new QTcpSocket(this);
    m_gdbSocket->connectToHost(QStringLiteral("127.0.0.1"), port);

    if (!m_gdbSocket->waitForConnected(3000)) {
        QString err = m_gdbSocket->errorString();
        delete m_gdbSocket;
        m_gdbSocket = nullptr;
        return makeToolError(QStringLiteral("Failed to connect to GDB stub on port %1: %2. "
            "Ensure the emulator was started with GDB enabled.").arg(port).arg(err));
    }

    // Send '?' to trigger initial halt and handshake
    // The response to '?' IS a stop reply (T05...), so use gdbReadPacket directly
    gdbSendPacket("?");
    QByteArray stopReply = gdbReadPacket(5000);
    // CPU is now halted in gdbstub_loop, waiting for next command

    // Read registers for info
    QByteArray regResp = gdbCommand("g", 3000);

    // Resume the CPU
    gdbSendPacket("c");

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("port")] = port;
    result[QStringLiteral("message")] = QStringLiteral("Connected to GDB stub. CPU resumed.");

    // Parse PC from register data if available
    if (regResp.size() >= 16 * 8) {
        // Registers are little-endian 32-bit values as hex
        // PC is register 15, at offset 15*8 = 120
        QByteArray pcHex = regResp.mid(15 * 8, 8);
        // GDB sends registers in target byte order (little-endian)
        // So bytes are: b0 b1 b2 b3 as hex pairs
        bool ok;
        uint32_t pcLE = pcHex.toUInt(&ok, 16);
        if (ok) {
            // Byte-swap from LE wire format: the hex string is LE bytes
            // e.g., "78563412" means 0x12345678
            uint32_t pc = ((pcLE >> 24) & 0xFF) | ((pcLE >> 8) & 0xFF00)
                        | ((pcLE << 8) & 0xFF0000) | ((pcLE << 24) & 0xFF000000);
            result[QStringLiteral("pc")] = QStringLiteral("0x%1").arg(pc, 8, 16, QLatin1Char('0'));
        }
    }

    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// Helper: parse 32-bit LE hex from GDB register dump
static uint32_t parseGdbReg(const QByteArray &data, int regIndex)
{
    int offset = regIndex * 8;
    if (offset + 8 > data.size()) return 0;

    // GDB sends registers as little-endian hex bytes
    // e.g., register value 0x12345678 is sent as "78563412"
    uint32_t val = 0;
    for (int i = 0; i < 4; ++i) {
        int hi = (offset + i * 2 < data.size()) ? data[offset + i * 2] : '0';
        int lo = (offset + i * 2 + 1 < data.size()) ? data[offset + i * 2 + 1] : '0';
        auto hexVal = [](int c) -> uint8_t {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return 0;
        };
        uint8_t byte = (hexVal(hi) << 4) | hexVal(lo);
        val |= static_cast<uint32_t>(byte) << (i * 8);
    }
    return val;
}

// Helper: encode 32-bit value as LE hex for GDB
static QByteArray encodeGdbReg(uint32_t val)
{
    QByteArray out;
    out.reserve(8);
    for (int i = 0; i < 4; ++i) {
        uint8_t byte = (val >> (i * 8)) & 0xFF;
        out.append(hexLut[byte >> 4]);
        out.append(hexLut[byte & 0xf]);
    }
    return out;
}

// ============ GDB tool: read memory ============

QJsonObject MCPServer::toolEmulatorGdbReadMemory(const QJsonObject &args)
{
    if (!m_gdbSocket || m_gdbSocket->state() != QAbstractSocket::ConnectedState) {
        return makeToolError(QStringLiteral("GDB not connected. Call emulator_gdb_connect first."));
    }

    QString addrStr = args.value(QStringLiteral("address")).toString();
    int size = args.value(QStringLiteral("size")).toInt();

    if (size <= 0 || size > 1023) {
        return makeToolError(QStringLiteral("Size must be 1-1023 (GDB packet buffer limit)"));
    }

    uint32_t addr = addrStr.toUInt(nullptr, 0);

    QByteArray cmd = QByteArray("m") + QByteArray::number(addr, 16) + "," + QByteArray::number(size, 16);
    QByteArray resp = gdbCommand(cmd, 5000);

    // Resume CPU
    gdbSendPacket("c");

    if (resp.isEmpty()) {
        return makeToolError(QStringLiteral("Timeout reading memory"));
    }
    if (resp.startsWith('E')) {
        return makeToolError(QStringLiteral("GDB error: ") + QString::fromLatin1(resp));
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("address")] = QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0'));
    result[QStringLiteral("size")] = size;
    result[QStringLiteral("hex")] = QString::fromLatin1(resp);
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// ============ GDB tool: write memory ============

QJsonObject MCPServer::toolEmulatorGdbWriteMemory(const QJsonObject &args)
{
    if (!m_gdbSocket || m_gdbSocket->state() != QAbstractSocket::ConnectedState) {
        return makeToolError(QStringLiteral("GDB not connected. Call emulator_gdb_connect first."));
    }

    QString addrStr = args.value(QStringLiteral("address")).toString();
    QString dataHex = args.value(QStringLiteral("data")).toString();

    uint32_t addr = addrStr.toUInt(nullptr, 0);
    int len = dataHex.length() / 2;

    QByteArray cmd = QByteArray("M") + QByteArray::number(addr, 16) + ","
                   + QByteArray::number(len, 16) + ":" + dataHex.toLatin1();
    QByteArray resp = gdbCommand(cmd, 5000);

    gdbSendPacket("c");

    if (resp.isEmpty()) {
        return makeToolError(QStringLiteral("Timeout writing memory"));
    }
    if (resp.startsWith('E')) {
        return makeToolError(QStringLiteral("GDB error: ") + QString::fromLatin1(resp));
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("address")] = QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0'));
    result[QStringLiteral("bytes_written")] = len;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// ============ GDB tool: read registers ============

QJsonObject MCPServer::toolEmulatorGdbReadRegisters()
{
    if (!m_gdbSocket || m_gdbSocket->state() != QAbstractSocket::ConnectedState) {
        return makeToolError(QStringLiteral("GDB not connected. Call emulator_gdb_connect first."));
    }

    QByteArray resp = gdbCommand("g", 5000);

    gdbSendPacket("c");

    if (resp.isEmpty()) {
        return makeToolError(QStringLiteral("Timeout reading registers"));
    }
    if (resp.startsWith('E')) {
        return makeToolError(QStringLiteral("GDB error: ") + QString::fromLatin1(resp));
    }

    // Parse the register dump: 42 registers × 8 hex chars each
    // R0-R15 (indices 0-15), CPSR (index 41)
    static const char *regNames[] = {
        "r0","r1","r2","r3","r4","r5","r6","r7",
        "r8","r9","r10","r11","r12","sp","lr","pc"
    };

    QJsonObject regs;
    for (int i = 0; i < 16; ++i) {
        uint32_t val = parseGdbReg(resp, i);
        regs[QString::fromLatin1(regNames[i])] = QStringLiteral("0x%1").arg(val, 8, 16, QLatin1Char('0'));
    }
    // CPSR is register index 41
    uint32_t cpsr = parseGdbReg(resp, 41);
    regs[QStringLiteral("cpsr")] = QStringLiteral("0x%1").arg(cpsr, 8, 16, QLatin1Char('0'));

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("registers")] = regs;
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// ============ GDB tool: set register ============

QJsonObject MCPServer::toolEmulatorGdbSetRegister(const QJsonObject &args)
{
    if (!m_gdbSocket || m_gdbSocket->state() != QAbstractSocket::ConnectedState) {
        return makeToolError(QStringLiteral("GDB not connected. Call emulator_gdb_connect first."));
    }

    QString regName = args.value(QStringLiteral("register")).toString().toLower();
    QString valStr = args.value(QStringLiteral("value")).toString();
    uint32_t value = valStr.toUInt(nullptr, 0);

    // Map register name to GDB index
    int regIndex = -1;
    if (regName.startsWith(QLatin1Char('r'))) {
        bool ok;
        int n = regName.midRef(1).toInt(&ok);
        if (ok && n >= 0 && n <= 15) regIndex = n;
    }
    if (regName == QStringLiteral("sp")) regIndex = 13;
    else if (regName == QStringLiteral("lr")) regIndex = 14;
    else if (regName == QStringLiteral("pc")) regIndex = 15;
    else if (regName == QStringLiteral("cpsr")) regIndex = 41;

    if (regIndex < 0) {
        return makeToolError(QStringLiteral("Unknown register: ") + regName);
    }

    // GDB 'P' packet: Pn=VVVVVVVV (register index in hex, value as LE hex)
    QByteArray cmd = QByteArray("P") + QByteArray::number(regIndex, 16) + "=" + encodeGdbReg(value);
    QByteArray resp = gdbCommand(cmd, 5000);

    gdbSendPacket("c");

    if (resp != "OK") {
        return makeToolError(QStringLiteral("GDB error setting register: ") + QString::fromLatin1(resp));
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("register")] = regName;
    result[QStringLiteral("value")] = QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0'));
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// ============ GDB tool: breakpoint ============

QJsonObject MCPServer::toolEmulatorGdbBreakpoint(const QJsonObject &args)
{
    if (!m_gdbSocket || m_gdbSocket->state() != QAbstractSocket::ConnectedState) {
        return makeToolError(QStringLiteral("GDB not connected. Call emulator_gdb_connect first."));
    }

    QString addrStr = args.value(QStringLiteral("address")).toString();
    bool set = args.value(QStringLiteral("set")).toBool(true);
    uint32_t addr = addrStr.toUInt(nullptr, 0);

    // Z0,addr,4 = set software breakpoint; z0,addr,4 = clear
    QByteArray cmd;
    cmd.append(set ? 'Z' : 'z');
    cmd.append("0,");
    cmd.append(QByteArray::number(addr, 16));
    cmd.append(",4");

    QByteArray resp = gdbCommand(cmd, 5000);

    gdbSendPacket("c");

    if (resp != "OK") {
        return makeToolError(QStringLiteral("GDB error: ") + QString::fromLatin1(resp));
    }

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("address")] = QStringLiteral("0x%1").arg(addr, 8, 16, QLatin1Char('0'));
    result[QStringLiteral("action")] = set ? QStringLiteral("set") : QStringLiteral("cleared");
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// ============ GDB tool: continue ============

QJsonObject MCPServer::toolEmulatorGdbContinue()
{
    if (!m_gdbSocket || m_gdbSocket->state() != QAbstractSocket::ConnectedState) {
        return makeToolError(QStringLiteral("GDB not connected. Call emulator_gdb_connect first."));
    }

    gdbSendPacket("c");

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("message")] = QStringLiteral("Execution resumed");
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// ============ GDB tool: step ============

QJsonObject MCPServer::toolEmulatorGdbStep()
{
    if (!m_gdbSocket || m_gdbSocket->state() != QAbstractSocket::ConnectedState) {
        return makeToolError(QStringLiteral("GDB not connected. Call emulator_gdb_connect first."));
    }

    // Send step command
    gdbSendPacket("s");

    // Wait for the stop reply (CPU executes one instruction, then halts)
    QByteArray stopReply = gdbReadPacket(5000);

    // Now read registers at the new PC
    QByteArray regResp = gdbCommand("g", 3000);

    // CPU stays halted — user must call gdb_continue to resume

    QJsonObject result;
    result[QStringLiteral("success")] = true;
    result[QStringLiteral("halted")] = true;

    if (!stopReply.isEmpty()) {
        result[QStringLiteral("stop_reply")] = QString::fromLatin1(stopReply);
    }

    if (regResp.size() >= 16 * 8) {
        uint32_t pc = parseGdbReg(regResp, 15);
        uint32_t cpsr = parseGdbReg(regResp, 41);
        result[QStringLiteral("pc")] = QStringLiteral("0x%1").arg(pc, 8, 16, QLatin1Char('0'));
        result[QStringLiteral("cpsr")] = QStringLiteral("0x%1").arg(cpsr, 8, 16, QLatin1Char('0'));
        result[QStringLiteral("thumb")] = (cpsr & 0x20) != 0;
    }

    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

// ============ GDB tool: raw packet ============

QJsonObject MCPServer::toolEmulatorGdbRaw(const QJsonObject &args)
{
    if (!m_gdbSocket || m_gdbSocket->state() != QAbstractSocket::ConnectedState) {
        return makeToolError(QStringLiteral("GDB not connected. Call emulator_gdb_connect first."));
    }

    QString packet = args.value(QStringLiteral("packet")).toString();
    QByteArray resp = gdbCommand(packet.toLatin1(), 5000);

    QJsonObject result;
    result[QStringLiteral("success")] = !resp.isEmpty();
    result[QStringLiteral("response")] = QString::fromLatin1(resp);
    return makeToolResult(QString::fromUtf8(QJsonDocument(result).toJson(QJsonDocument::Compact)));
}

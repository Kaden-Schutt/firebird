#ifndef MCPSERVER_H
#define MCPSERVER_H

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <QMutex>
#include <QWaitCondition>

class StdioReader;

class MCPServer : public QObject
{
    Q_OBJECT

public:
    explicit MCPServer(QObject *parent = nullptr);
    ~MCPServer();

    void start();
    void stop();

public slots:
    void handleLine(const QString &line);

private:
    void sendResponse(const QJsonObject &response);
    void sendError(const QJsonValue &id, int code, const QString &message);
    void sendResult(const QJsonValue &id, const QJsonObject &result);

    // MCP protocol handlers
    void handleInitialize(const QJsonValue &id, const QJsonObject &params);
    void handleInitialized(const QJsonValue &id);
    void handleToolsList(const QJsonValue &id);
    void handleToolsCall(const QJsonValue &id, const QJsonObject &params);

    // Tool implementations
    QJsonObject toolEmulatorStatus();
    QJsonObject toolEmulatorStart(const QJsonObject &args);
    QJsonObject toolEmulatorStop();
    QJsonObject toolEmulatorReset();
    QJsonObject toolEmulatorPause(const QJsonObject &args);
    QJsonObject toolEmulatorSetTurbo(const QJsonObject &args);
    QJsonObject toolEmulatorScreenshot(const QJsonObject &args);
    QJsonObject toolEmulatorPressKey(const QJsonObject &args);
    QJsonObject toolEmulatorKeyDown(const QJsonObject &args);
    QJsonObject toolEmulatorKeyUp(const QJsonObject &args);
    QJsonObject toolEmulatorTypeText(const QJsonObject &args);
    QJsonObject toolEmulatorTouchpad(const QJsonObject &args);
    QJsonObject toolEmulatorListFiles(const QJsonObject &args);
    QJsonObject toolEmulatorUploadFile(const QJsonObject &args);
    QJsonObject toolEmulatorSendOs(const QJsonObject &args);
    QJsonObject toolEmulatorDownloadFile(const QJsonObject &args);
    QJsonObject toolEmulatorDeleteFile(const QJsonObject &args);
    QJsonObject toolEmulatorCreateDir(const QJsonObject &args);
    QJsonObject toolEmulatorReadMemory(const QJsonObject &args);
    QJsonObject toolEmulatorWriteMemory(const QJsonObject &args);
    QJsonObject toolEmulatorDisassemble(const QJsonObject &args);
    QJsonObject toolEmulatorGetRegisters();
    QJsonObject toolEmulatorSetRegister(const QJsonObject &args);
    QJsonObject toolEmulatorSetBreakpoint(const QJsonObject &args);
    QJsonObject toolEmulatorClearBreakpoint(const QJsonObject &args);
    QJsonObject toolEmulatorStep();
    QJsonObject toolEmulatorContinue();
    QJsonObject toolEmulatorExecuteProgram(const QJsonObject &args);
    QJsonObject toolEmulatorSaveSnapshot(const QJsonObject &args);
    QJsonObject toolEmulatorLoadSnapshot(const QJsonObject &args);

    // New high-level tools
    QJsonObject toolEmulatorArrow(const QJsonObject &args);
    QJsonObject toolEmulatorWaitStable(const QJsonObject &args);
    QJsonObject toolEmulatorNavigate(const QJsonObject &args);
    QJsonObject toolEmulatorOpenDocument(const QJsonObject &args);
    QJsonObject toolEmulatorInputExpr(const QJsonObject &args);
    QJsonObject toolEmulatorSearchMemory(const QJsonObject &args);
    QJsonObject toolEmulatorReadString(const QJsonObject &args);
    QJsonObject toolEmulatorRunUntil(const QJsonObject &args);
    QJsonObject toolEmulatorTrace(const QJsonObject &args);
    QJsonObject toolEmulatorFindFunctions(const QJsonObject &args);
    QJsonObject toolEmulatorDeployAndRun(const QJsonObject &args);

    // Internal helpers
    bool waitForStableScreen(int timeoutMs, int stableMs, int intervalMs, QString *screenshotPath = nullptr);
    bool pressKeyAndWaitForTransition(const QString &keyName, int changeTimeoutMs = 3000, int stableMs = 300, int intervalMs = 50, QString *screenshotPath = nullptr);
    void simulateArrow(const QString &direction);

    // Helper to build tool result
    QJsonObject makeToolResult(const QString &text);
    QJsonObject makeToolError(const QString &text);

    StdioReader *m_reader;
    bool m_initialized;

public:
    // For async file operations (public for callbacks)
    QMutex m_asyncMutex;
    QWaitCondition m_asyncCondition;
    bool m_asyncComplete;
    int m_asyncProgress;
    QString m_asyncError;
    QJsonArray m_dirListResult;
};

extern MCPServer *the_mcp_server;

#endif // MCPSERVER_H

#ifndef STDIOREADER_H
#define STDIOREADER_H

#include <QThread>
#include <QJsonObject>
#include <atomic>

class StdioReader : public QThread
{
    Q_OBJECT

public:
    explicit StdioReader(QObject *parent = nullptr);
    ~StdioReader();

    void stop();

signals:
    void lineReceived(const QString &line);
    void errorOccurred(const QString &error);

protected:
    void run() override;

private:
    std::atomic<bool> m_running{false};
};

#endif // STDIOREADER_H

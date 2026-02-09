#include "stdioreader.h"

#include <QTextStream>
#include <iostream>
#include <string>

StdioReader::StdioReader(QObject *parent)
    : QThread(parent)
{
}

StdioReader::~StdioReader()
{
    stop();
    wait();
}

void StdioReader::stop()
{
    m_running = false;
}

void StdioReader::run()
{
    m_running = true;
    std::string line;

    while (m_running && std::getline(std::cin, line)) {
        if (!m_running)
            break;

        QString qline = QString::fromStdString(line).trimmed();
        if (!qline.isEmpty()) {
            emit lineReceived(qline);
        }
    }
}

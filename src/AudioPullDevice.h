#pragma once

#include <QIODevice>
#include <QMutex>
#include <QByteArray>

class AudioPullDevice : public QIODevice {
    Q_OBJECT
public:
    explicit AudioPullDevice(int bufferMs = 200, QObject* parent = nullptr);

    qint64 readData(char* data, qint64 maxSize) override;
    qint64 writeData(const char* data, qint64 maxSize) override;

    void abort();
    qint64 bytesAvailable() const override;
    bool isSequential() const override { return true; }

private:
    mutable QMutex m_mutex;
    QByteArray m_buffer;
    qint64 m_writePos = 0;
    qint64 m_readPos  = 0;
    int m_bufferSize  = 0;
    bool m_abort = false;
};

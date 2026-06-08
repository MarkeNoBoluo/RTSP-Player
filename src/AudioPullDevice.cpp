#include "AudioPullDevice.h"

AudioPullDevice::AudioPullDevice(int bufferMs, QObject* parent)
    : QIODevice(parent)
{
    m_bufferSize = 48000 * 2 * 2 * bufferMs / 1000; // 48k * stereo * s16 * ms
    m_buffer.resize(m_bufferSize);
}

qint64 AudioPullDevice::readData(char* data, qint64 maxSize) {
    QMutexLocker lock(&m_mutex);
    if (m_abort) return 0;

    qint64 available = (m_writePos >= m_readPos)
        ? m_writePos - m_readPos
        : m_bufferSize - m_readPos + m_writePos;

    if (available == 0) return 0;

    qint64 toRead = qMin(maxSize, available);
    qint64 firstChunk = qMin(toRead, m_bufferSize - m_readPos);

    memcpy(data, m_buffer.constData() + m_readPos, static_cast<size_t>(firstChunk));
    if (firstChunk < toRead) {
        memcpy(data + firstChunk, m_buffer.constData(),
               static_cast<size_t>(toRead - firstChunk));
    }

    m_readPos = (m_readPos + toRead) % m_bufferSize;
    return toRead;
}

qint64 AudioPullDevice::writeData(const char* data, qint64 maxSize) {
    QMutexLocker lock(&m_mutex);
    if (m_abort) return -1;

    qint64 available = m_bufferSize - 1 -
        ((m_writePos >= m_readPos) ? (m_writePos - m_readPos)
                                   : (m_bufferSize - m_readPos + m_writePos));

    if (available <= 0) return 0;

    qint64 toWrite = qMin(maxSize, available);
    qint64 firstChunk = qMin(toWrite, m_bufferSize - m_writePos);

    memcpy(m_buffer.data() + m_writePos, data, static_cast<size_t>(firstChunk));
    if (firstChunk < toWrite) {
        memcpy(m_buffer.data(), data + firstChunk,
               static_cast<size_t>(toWrite - firstChunk));
    }

    m_writePos = (m_writePos + toWrite) % m_bufferSize;
    return toWrite;
}

qint64 AudioPullDevice::bytesAvailable() const {
    QMutexLocker lock(&m_mutex);
    return (m_writePos >= m_readPos)
        ? m_writePos - m_readPos
        : m_bufferSize - m_readPos + m_writePos;
}

void AudioPullDevice::abort() {
    QMutexLocker lock(&m_mutex);
    m_writePos = 0;
    m_readPos = 0;
    m_abort = true;
}

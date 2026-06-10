#include "AudioRingBuffer.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

#include <cstring>

AudioRingBuffer::AudioRingBuffer(int bufferMs)
    : m_bufferMs(bufferMs)
{
    m_totalSize = bufferMs * kSampleRate * kChannels * kBytesPerSample / 1000;
    m_buffer = new uint8_t[m_totalSize];
    memset(m_buffer, 0, m_totalSize);
}

AudioRingBuffer::~AudioRingBuffer() {
    delete[] m_buffer;
}

int AudioRingBuffer::bytesPerMs() const {
    return kSampleRate * kChannels * kBytesPerSample / 1000;
}

bool AudioRingBuffer::write(const uint8_t* data, int len, double pts, int serial) {
    if (len <= 0) return true;
    if (serial != m_serial.load(std::memory_order_acquire)) return false;

    std::unique_lock<std::mutex> lock(m_mutex);

    while (m_avail + len > m_totalSize && !m_abort) {
        m_cv.wait(lock);
    }
    if (m_abort) return false;

    int first = len;
    if (m_writePos + len > m_totalSize) {
        first = m_totalSize - m_writePos;
    }

    memcpy(m_buffer + m_writePos, data, first);
    if (len > first) {
        memcpy(m_buffer, data + first, len - first);
    }

    if (m_avail == 0) {
        m_ptsOffset   = pts;
        m_ptsWritePos = 0;
    }

    m_writePos = (m_writePos + len) % m_totalSize;
    m_avail   += len;

    m_ptsWritePos += len;
    double writtenPts = m_ptsOffset + (double)m_ptsWritePos / (kSampleRate * kChannels * kBytesPerSample);
    m_writtenPts.store(writtenPts, std::memory_order_release);

    lock.unlock();
    m_cv.notify_one();
    return true;
}

int AudioRingBuffer::read(uint8_t* dst, int len, double* outPts) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_avail <= 0) {
        if (outPts) *outPts = m_writtenPts.load(std::memory_order_acquire);
        if (m_stats) m_stats->audioUnderruns++;
        return 0;
    }

    int actual = len < m_avail ? len : m_avail;

    int first = actual;
    if (m_readPos + actual > m_totalSize) {
        first = m_totalSize - m_readPos;
    }

    memcpy(dst, m_buffer + m_readPos, first);
    if (actual > first) {
        memcpy(dst + first, m_buffer, actual - first);
    }

    if (outPts) {
        double consumedSec = (double)(m_readPos - m_ptsWritePos + m_avail > 0 ? 0 : 0);
        // pts at current read position
        int bytesBefore = (m_readPos >= m_ptsWritePos && m_avail > 0)
            ? (m_totalSize - m_writePos + m_readPos)
            : (m_readPos - ((m_writePos - m_avail + m_totalSize) % m_totalSize));

        double consumedSamples = 0.0;
        if (m_avail > 0) {
            int startPos = (m_writePos - m_avail + m_totalSize) % m_totalSize;
            int dist = (m_readPos - startPos + m_totalSize) % m_totalSize;
            consumedSamples = (double)dist / (kChannels * kBytesPerSample);
        }
        *outPts = m_ptsOffset + consumedSamples / kSampleRate;
    }

    m_readPos = (m_readPos + actual) % m_totalSize;
    m_avail  -= actual;

    if (actual < len && m_stats) {
        m_stats->audioUnderruns++;
    }

    m_cv.notify_one();
    return actual;
}

void AudioRingBuffer::flush() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_writePos    = 0;
    m_readPos     = 0;
    m_avail       = 0;
    m_ptsOffset   = 0.0;
    m_ptsWritePos = 0;
    m_writtenPts.store(0.0);
    m_serial.fetch_add(1);
    m_abort = false;
}

void AudioRingBuffer::abort() {
    m_abort = true;
    m_cv.notify_all();
}

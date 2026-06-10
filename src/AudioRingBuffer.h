#pragma once

#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <atomic>

class PlayerStats;

class AudioRingBuffer {
public:
    struct Chunk {
        uint8_t* data = nullptr;
        int32_t  len = 0;
        double   pts = 0.0;
        int      serial = 0;
    };

    AudioRingBuffer(int bufferMs = 100);
    ~AudioRingBuffer();

    void setStats(PlayerStats* stats) { m_stats = stats; }

    bool write(const uint8_t* data, int len, double pts, int serial);
    int  read(uint8_t* dst, int len, double* outPts, int* outChunkOffset);
    void flush();
    void abort();
    int  serial() const { return m_serial.load(); }
    void setSerial(int s) { m_serial.store(s); }

private:
    static constexpr int kMaxChunks = 32;

    int m_bufferMs;
    Chunk m_chunks[kMaxChunks];

    int m_writeIdx = 0;
    int m_readIdx  = 0;
    int m_readOffset = 0;  // bytes already consumed from current chunk
    int m_avail = 0;       // number of readable chunks

    std::atomic<int> m_serial{0};

    std::mutex m_mutex;
    std::condition_variable m_cv;

    PlayerStats* m_stats = nullptr;
    std::atomic<bool> m_abort{false};
};

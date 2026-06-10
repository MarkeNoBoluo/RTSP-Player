#pragma once

#include <cstdint>
#include <mutex>
#include <condition_variable>
#include <atomic>

class PlayerStats;

class AudioRingBuffer {
public:
    AudioRingBuffer(int bufferMs = 100);
    ~AudioRingBuffer();

    void setStats(PlayerStats* stats) { m_stats = stats; }

    bool write(const uint8_t* data, int len, double pts, int serial);
    int  read(uint8_t* dst, int len, double* outPts);
    void flush();
    void abort();
    int  serial() const { return m_serial.load(); }

    // 写指针位置（对应已写入的样本数），用于 audio clock 计算
    double writtenPts() const { return m_writtenPts.load(std::memory_order_acquire); }

private:
    int bytesPerMs() const;

    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels   = 2;
    static constexpr int kBytesPerSample = 2;  // s16

    int m_bufferMs;
    int m_totalSize;
    uint8_t* m_buffer;

    // 环形读写位置
    int m_writePos = 0;
    int m_readPos  = 0;
    int m_avail    = 0;  // 当前可读字节数

    // PTS 追踪
    double m_ptsOffset = 0.0;
    int    m_ptsWritePos = 0;
    std::atomic<double> m_writtenPts{0.0};

    std::atomic<int> m_serial{0};

    std::mutex m_mutex;
    std::condition_variable m_cv;

    PlayerStats* m_stats = nullptr;
    std::atomic<bool> m_abort{false};
};

#pragma once

#include "Common.h"
#include <atomic>
#include <cstdint>

class VideoFrameQueue {
public:
    static constexpr int kSlotCount = 4;

    VideoFrameQueue();
    ~VideoFrameQueue();

    bool writeFrame(AVFrame* srcFrame, int64_t pts, int serial);

    bool hasNewFrame() const;
    int count() const { return m_count.load(std::memory_order_acquire); }
    int64_t peekPts() const;
    int peekSerial() const;
    AVFrame* renderFrame();

    void advanceDisplay();
    void discardRender();

    void flush();

private:
    AVFrame*          m_avFrames[kSlotCount];
    VideoFrame        m_slots[kSlotCount];
    int               m_serial[kSlotCount];

    int m_writeIdx{0};
    int m_readIdx{0};
    std::atomic<int> m_count{0};

    int m_width{0};
    int m_height{0};
};

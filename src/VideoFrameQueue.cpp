#include "VideoFrameQueue.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/frame.h>
}

VideoFrameQueue::VideoFrameQueue() {
    for (int i = 0; i < kSlotCount; i++) {
        m_avFrames[i] = av_frame_alloc();
        m_slots[i].frame = m_avFrames[i];
        m_serial[i] = 0;
    }
}

VideoFrameQueue::~VideoFrameQueue() {
    for (int i = 0; i < kSlotCount; i++) {
        av_frame_free(&m_avFrames[i]);
    }
}

bool VideoFrameQueue::writeFrame(AVFrame* srcFrame, int64_t pts, int serial) {
    if (m_count.load(std::memory_order_acquire) >= kSlotCount) return false;

    int wi = m_writeIdx;

    av_frame_unref(m_avFrames[wi]);
    av_frame_move_ref(m_avFrames[wi], srcFrame);
    m_slots[wi].pts = pts;
    m_serial[wi] = serial;

    if (m_width != srcFrame->width || m_height != srcFrame->height) {
        m_width  = srcFrame->width;
        m_height = srcFrame->height;
    }

    m_writeIdx = (wi + 1) % kSlotCount;
    m_count.fetch_add(1, std::memory_order_release);
    return true;
}

bool VideoFrameQueue::hasNewFrame() const {
    return m_count.load(std::memory_order_acquire) > 0;
}

int64_t VideoFrameQueue::peekPts() const {
    if (m_count.load(std::memory_order_acquire) == 0) return -1;
    int ri = m_readIdx;
    if (!m_avFrames[ri]->data[0]) return -1;
    return m_slots[ri].pts;
}

int VideoFrameQueue::peekSerial() const {
    if (m_count.load(std::memory_order_acquire) == 0) return -1;
    return m_serial[m_readIdx];
}

AVFrame* VideoFrameQueue::renderFrame() {
    if (m_count.load(std::memory_order_acquire) == 0) return nullptr;
    return m_avFrames[m_readIdx];
}

void VideoFrameQueue::advanceDisplay() {
    if (m_count.load(std::memory_order_acquire) == 0) return;
    m_readIdx = (m_readIdx + 1) % kSlotCount;
    m_count.fetch_sub(1, std::memory_order_release);
}

void VideoFrameQueue::discardRender() {
    advanceDisplay();
}

void VideoFrameQueue::flush() {
    for (int i = 0; i < kSlotCount; i++) {
        av_frame_unref(m_avFrames[i]);
        m_serial[i] = 0;
    }
    m_writeIdx = 0;
    m_readIdx  = 0;
    m_count.store(0, std::memory_order_relaxed);
    m_width  = 0;
    m_height = 0;
    LOG_INFO("VideoFrameQueue flushed");
}

#include "VideoFrameQueue.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/frame.h>
}

VideoFrameQueue::VideoFrameQueue() {
    for (int i = 0; i < kSlotCount; i++) {
        m_avFrames[i] = av_frame_alloc();
        m_slots[i].frame = m_avFrames[i];
        m_slots[i].serial = 0;
    }
}

VideoFrameQueue::~VideoFrameQueue() {
    for (int i = 0; i < kSlotCount; i++) {
        av_frame_free(&m_avFrames[i]);
    }
}

bool VideoFrameQueue::writeFrame(AVFrame* srcFrame, int64_t pts, int serial) {
    if (m_count.load(std::memory_order_acquire) >= kSlotCount) return false;

    int di = m_decodeIdx;

    av_frame_unref(m_avFrames[di]);
    av_frame_move_ref(m_avFrames[di], srcFrame);
    m_slots[di].pts = pts;
    m_slots[di].serial = serial;

    if (m_width != srcFrame->width || m_height != srcFrame->height) {
        m_width  = srcFrame->width;
        m_height = srcFrame->height;
    }

    m_renderIdx = di;
    m_decodeIdx = (di + 1) % kSlotCount;
    m_count.fetch_add(1, std::memory_order_release);
    return true;
}

bool VideoFrameQueue::hasNewFrame() const {
    return m_count.load(std::memory_order_acquire) > 0;
}

int64_t VideoFrameQueue::peekDisplayPts() const {
    if (m_count.load(std::memory_order_acquire) == 0) return -1;
    int di = m_displayIdx;
    if (!m_avFrames[di]->data[0]) return -1;
    return m_slots[di].pts;
}

int VideoFrameQueue::peekDisplaySerial() const {
    if (m_count.load(std::memory_order_acquire) == 0) return -1;
    return m_slots[m_displayIdx].serial;
}

AVFrame* VideoFrameQueue::displayFrame() {
    if (m_count.load(std::memory_order_acquire) == 0) return nullptr;
    return m_avFrames[m_displayIdx];
}

void VideoFrameQueue::advanceDisplay() {
    if (m_count.load(std::memory_order_acquire) == 0) return;
    m_displayIdx = (m_displayIdx + 1) % kSlotCount;
    m_count.fetch_sub(1, std::memory_order_release);
}

void VideoFrameQueue::discardAndAdvance() {
    advanceDisplay();
}

void VideoFrameQueue::flush() {
    for (int i = 0; i < kSlotCount; i++) {
        av_frame_unref(m_avFrames[i]);
        m_slots[i].serial = 0;
    }
    m_decodeIdx  = 0;
    m_renderIdx  = 0;
    m_displayIdx = 0;
    m_count.store(0, std::memory_order_relaxed);
    m_width  = 0;
    m_height = 0;
    LOG_INFO("VideoFrameQueue flushed");
}

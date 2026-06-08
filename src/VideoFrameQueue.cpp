#include "VideoFrameQueue.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/frame.h>
}

VideoFrameQueue::VideoFrameQueue() {
    for (int i = 0; i < 3; i++) {
        m_avFrames[i] = av_frame_alloc();
        m_slots[i].frame = m_avFrames[i];
    }
}

VideoFrameQueue::~VideoFrameQueue() {
    for (int i = 0; i < 3; i++) {
        av_frame_free(&m_avFrames[i]);
    }
}

bool VideoFrameQueue::writeFrame(AVFrame* srcFrame, int64_t pts) {
    av_frame_unref(m_avFrames[m_decodeIdx]);
    av_frame_move_ref(m_avFrames[m_decodeIdx], srcFrame);
    m_slots[m_decodeIdx].pts = pts;

    if (m_width != srcFrame->width || m_height != srcFrame->height) {
        m_width  = srcFrame->width;
        m_height = srcFrame->height;
        LOG_INFO("Video resolution changed: %dx%d", srcFrame->width, srcFrame->height);
    }

    m_decodeIdx = m_renderIdx.exchange(m_decodeIdx, std::memory_order_acq_rel);

    m_newFrameCv.notify_one();
    return true;
}

void VideoFrameQueue::waitForNewFrame(int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_cvMutex);
    m_newFrameCv.wait_for(lock, std::chrono::milliseconds(timeoutMs));
}

int64_t VideoFrameQueue::peekRenderPts() const {
    int ri = m_renderIdx.load(std::memory_order_acquire);
    if (ri == m_displayIdx) return -1;
    if (!m_avFrames[ri]->data[0]) return -1;
    return m_slots[ri].pts;
}

void VideoFrameQueue::discardRender() {
    // no-op: render slot will be overwritten by next writeFrame's swap
}

bool VideoFrameQueue::commitDisplay() {
    int ri = m_renderIdx.load(std::memory_order_acquire);
    if (ri == m_displayIdx) return false;
    m_renderIdx.store(m_displayIdx, std::memory_order_release);
    m_displayIdx = ri;
    return true;
}

const VideoFrame* VideoFrameQueue::displayFrame() const {
    return &m_slots[m_displayIdx];
}

void VideoFrameQueue::flush() {
    LOG_INFO("VideoFrameQueue flushed");
    for (int i = 0; i < 3; i++) {
        av_frame_unref(m_avFrames[i]);
    }
    m_renderIdx.store(1, std::memory_order_relaxed);
    m_displayIdx = 0;
    m_decodeIdx  = 2;
    m_width  = 0;
    m_height = 0;
}

void VideoFrameQueue::notifyAll() {
    LOG_DEBUG("VideoFrameQueue notifyAll");
    m_newFrameCv.notify_all();
}

#pragma once

#include "Common.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

class VideoFrameQueue {
public:
    VideoFrameQueue();
    ~VideoFrameQueue();

    // VideoDecodeThread
    bool writeFrame(AVFrame* srcFrame, int64_t pts);

    // RenderScheduler thread (exclusive)
    void    waitForNewFrame(int timeoutMs);
    int64_t peekRenderPts() const;
    void    discardRender();
    bool    commitDisplay();

    // UI Thread (paintGL)
    const VideoFrame* displayFrame() const;

    // StreamLifecycleManager (called after all threads stopped)
    void flush();
    void notifyAll();

private:
    AVFrame*          m_avFrames[3];
    VideoFrame        m_slots[3];
    std::atomic<int>  m_renderIdx{1};
    int               m_displayIdx{0};
    int               m_decodeIdx{2};
    int               m_width{0};
    int               m_height{0};

    std::condition_variable m_newFrameCv;
    std::mutex              m_cvMutex;
};

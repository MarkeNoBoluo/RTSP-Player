#include "RenderScheduler.h"
#include "VideoFrameQueue.h"
#include "AVClock.h"
#include "GLVideoWidget.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/time.h>
}

RenderScheduler::RenderScheduler(VideoFrameQueue* frameQueue, AVClock* clock,
                                 GLVideoWidget* widget, PlayerStats* stats,
                                 QObject* parent)
    : QThread(parent)
    , m_frameQueue(frameQueue)
    , m_clock(clock)
    , m_widget(widget)
    , m_stats(stats)
{
}

RenderScheduler::~RenderScheduler() {
    stop();
    wait();
}

void RenderScheduler::stop() {
    m_running = false;
    m_frameQueue->notifyAll();
}

void RenderScheduler::setFrameDuration(double frameDurationUs) {
    m_frameDurationUs = frameDurationUs;
    LOG_INFO("Frame drop threshold set: frameDuration=%.0fus, dropThreshold=%.0fus",
             frameDurationUs, frameDurationUs * 1.5);
}

void RenderScheduler::run() {
    m_running = true;

    LOG_INFO("Render scheduler thread running");

    while (m_running) {
        m_frameQueue->waitForNewFrame(10);
        if (!m_running) break;

        int64_t pts = m_frameQueue->peekRenderPts();
        if (pts < 0) continue;

        int64_t latenessUs = 0;
        if (m_clock->isReady()) {
            int64_t nowUs = av_gettime_relative();
            auto clock = m_clock->videoClock();
            int64_t clockPtsUs = static_cast<int64_t>(clock.pts * AV_TIME_BASE);
            int64_t elapsedUs = nowUs - clock.systemTime;
            int64_t currentPlaybackPtsUs = clockPtsUs + elapsedUs;
            latenessUs = currentPlaybackPtsUs - pts;
        }
        m_stats->lastLatenessUs = latenessUs;

        if (shouldDrop(latenessUs)) {
            m_frameQueue->discardRender();
            m_stats->framesDropped++;
            continue;
        }

        m_frameQueue->commitDisplay();
        m_clock->setVideoClock(pts / (double)AV_TIME_BASE);

        auto* displayFrame = m_frameQueue->displayFrame();
        m_widget->setDisplayFrame(displayFrame);

        m_stats->framesRendered++;
    }

    LOG_INFO("Render scheduler thread exiting, rendered=%lld, dropped=%lld",
             (long long)m_stats->framesRendered.load(), (long long)m_stats->framesDropped.load());
}

bool RenderScheduler::shouldDrop(int64_t latenessUs) const {
    if (!m_clock->isReady()) return false;
    if (m_frameDurationUs <= 0.0) return false;

    int64_t threshold = static_cast<int64_t>(m_frameDurationUs * 1.5);
    if (threshold < 30000) threshold = 30000;

    return latenessUs > threshold;
}

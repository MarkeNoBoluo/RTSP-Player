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

void RenderScheduler::run() {
    m_running = true;

    LOG_INFO("Render scheduler thread running");

    while (m_running) {
        m_frameQueue->waitForNewFrame(10);
        if (!m_running) break;

        int64_t pts = m_frameQueue->peekRenderPts();
        if (pts < 0) {
            continue;
        }

        if (shouldDrop(pts)) {
            m_frameQueue->discardRender();
            m_stats->framesDropped++;
            LOG_DEBUG("Dropping frame pts=%lld, dropped=%lld", (long long)pts, (long long)m_stats->framesDropped.load());
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

bool RenderScheduler::shouldDrop(int64_t pts) const {
    // Phase 1: disable drop, render all frames
    (void)pts;
    return false;
}

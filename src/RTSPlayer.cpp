#include "RTSPlayer.h"
#include "PlayerStateMachine.h"
#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include "AVClock.h"
#include "PlayerStats.h"
#include "StreamLifecycleManager.h"
#include "SDLRenderer.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/time.h>
}

RTSPlayer::RTSPlayer()
    : m_stateMachine(new PlayerStateMachine)
    , m_videoQueue(new PacketQueue)
    , m_audioQueue(new PacketQueue)
    , m_frameQueue(new VideoFrameQueue)
    , m_clock(new AVClock)
    , m_stats(new PlayerStats)
{
    // Renderer created externally via main.cpp and passed in
    m_lifecycle = new StreamLifecycleManager(m_stateMachine, m_stats,
                                              m_videoQueue, m_audioQueue,
                                              m_frameQueue, m_clock);
}

RTSPlayer::~RTSPlayer() {
    close();
    delete m_lifecycle;
    delete m_stateMachine;
    delete m_videoQueue;
    delete m_audioQueue;
    delete m_frameQueue;
    delete m_clock;
    delete m_stats;
}

bool RTSPlayer::open(const char* url) {
    return m_lifecycle->open(url);
}

void RTSPlayer::close() {
    m_lifecycle->close();
}

void RTSPlayer::setRenderer(IRenderer* renderer) {
    m_renderer = renderer;
}

IRenderer* RTSPlayer::renderer() const {
    return m_renderer;
}

PlayerStats* RTSPlayer::stats() const {
    return m_lifecycle->stats();
}

PlayerState RTSPlayer::state() const {
    return m_lifecycle->state();
}

void RTSPlayer::setStateCallback(StateCallback cb) {
    m_lifecycle->setStateCallback(std::move(cb));
}

void RTSPlayer::setErrorCallback(ErrorCallback cb) {
    m_lifecycle->setErrorCallback(std::move(cb));
}

int RTSPlayer::pktSerial() const {
    return m_lifecycle->pktSerial();
}

void RTSPlayer::videoRefresh() {
    if (m_inVideoRefresh) return;
    m_inVideoRefresh = true;
    struct _ { bool& flag; ~_() { flag = false; } } _guard{m_inVideoRefresh};

    if (!m_frameQueue->hasNewFrame()) return;

    int globalSerial = pktSerial();
    int frameSerial = m_frameQueue->peekSerial();
    if (frameSerial >= 0 && frameSerial != globalSerial) {
        m_frameQueue->discardRender();
        return;
    }

    int64_t pts = m_frameQueue->peekPts();
    if (pts < 0) return;

    if (m_clock->isReady()) {
        int64_t nowUs = av_gettime_relative();
        auto vc = m_clock->videoClock();
        int64_t clockPtsUs = static_cast<int64_t>(vc.pts * AV_TIME_BASE);
        int64_t elapsedUs = nowUs - vc.systemTime;
        int64_t currentPtsUs = clockPtsUs + elapsedUs;

        if (m_clock->hasAudio()) {
            double drift = m_clock->drift();
            double frameDuration = 0.033;
            double delay = frameDuration + drift;
            if (delay < 0.005) delay = 0.005;
            if (delay > 0.5) delay = 0.5;

            if (pts > currentPtsUs + static_cast<int64_t>(delay * AV_TIME_BASE)) {
                return;
            }
        } else {
            if (pts > currentPtsUs + 20000) {
                return;
            }
        }

        int64_t latencyUs = currentPtsUs - pts;
        m_stats->lastLatenessUs = latencyUs;

        {
            int64_t absLat = (latencyUs < 0) ? -latencyUs : latencyUs;
            int64_t curMax = m_stats->maxLatenessUs.load(std::memory_order_acquire);
            while (absLat > curMax) {
                if (m_stats->maxLatenessUs.compare_exchange_weak(
                        curMax, absLat,
                        std::memory_order_release, std::memory_order_acquire)) {
                    break;
                }
            }
        }

        if (latencyUs > 50000) {
            if (m_consecutiveDrops >= 2 && latencyUs < 200000) {
                // Burst recovery: render to re-anchor video clock
            } else {
                m_frameQueue->discardRender();
                m_stats->framesDropped++;
                m_consecutiveDrops++;

                int64_t curBurst = m_stats->renderSkipBurst.load(std::memory_order_acquire);
                while (m_consecutiveDrops > curBurst) {
                    if (m_stats->renderSkipBurst.compare_exchange_weak(
                            curBurst, m_consecutiveDrops,
                            std::memory_order_release, std::memory_order_acquire)) {
                        break;
                    }
                }

                LOG_DEBUG("Drop frame pts=%.3fs latency=%lldus (burst=%d)",
                          pts / 1000000.0, (long long)latencyUs, m_consecutiveDrops);
                return;
            }
        }
    }

    AVFrame* frame = m_frameQueue->renderFrame();
    if (!frame || !frame->data[0]) return;

    int64_t displayBeforeUs = av_gettime_relative();
    IRenderer* r = renderer();
    if (r) {
        r->displayFrame(frame);
    }
    int64_t displayAfterUs = av_gettime_relative();
    m_stats->recordPaintLatency(displayAfterUs - displayBeforeUs);

    m_clock->setVideoClock(pts / (double)AV_TIME_BASE);
    m_frameQueue->advanceDisplay();
    m_stats->framesRendered++;
    m_stats->frameId++;
    m_consecutiveDrops = 0;

    int64_t renderNowUs = av_gettime_relative();
    if (m_lastRenderUs > 0) {
        m_stats->recordPaintInterval(renderNowUs - m_lastRenderUs);
    }
    m_lastRenderUs = renderNowUs;

    auto rn = m_stats->framesRendered.load();
    if (rn <= 3 || rn % 30 == 0) {
        if (rn % 30 == 0) {
            m_stats->recordQueueDepth(
                m_videoQueue->durationMs(),
                m_frameQueue->count() * 33);
        }
        LOG_INFO("Render #%lld pts=%.3fs latency=%lldus",
                 (long long)rn, pts / 1000000.0, (long long)m_stats->lastLatenessUs.load());
    }
}

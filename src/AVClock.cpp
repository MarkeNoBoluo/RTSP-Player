#include "AVClock.h"

extern "C" {
#include <libavutil/time.h>
}

int64_t AVClock::nowUs() const {
    return av_gettime_relative();
}

void AVClock::setVideoClock(double pts) {
    m_videoPts.store(pts, std::memory_order_release);
    m_videoSysTime.store(nowUs(), std::memory_order_release);
    m_videoReady.store(true, std::memory_order_release);
}

ClockPoint AVClock::videoClock() const {
    for (;;) {
        int64_t sys1 = m_videoSysTime.load(std::memory_order_acquire);
        double pts = m_videoPts.load(std::memory_order_acquire);
        int64_t sys2 = m_videoSysTime.load(std::memory_order_acquire);
        if (sys1 == sys2)
            return { pts, sys1 };
    }
}

bool AVClock::isReady() const {
    return m_videoReady.load(std::memory_order_acquire);
}

void AVClock::setAudioClock(double pts) {
    m_audioPts.store(pts, std::memory_order_release);
    m_audioSysTime.store(nowUs(), std::memory_order_release);
}

bool AVClock::hasAudio() const {
    return m_audioSysTime.load(std::memory_order_acquire) > 0;
}

ClockPoint AVClock::audioClock() const {
    for (;;) {
        int64_t sys1 = m_audioSysTime.load(std::memory_order_acquire);
        double pts = m_audioPts.load(std::memory_order_acquire);
        int64_t sys2 = m_audioSysTime.load(std::memory_order_acquire);
        if (sys1 == sys2)
            return { pts, sys1 };
    }
}

double AVClock::drift() const {
    auto v = videoClock();
    auto a = audioClock();
    return (a.pts - v.pts) - (a.systemTime - v.systemTime) / 1000000.0;
}

void AVClock::reset() {
    m_videoPts.store(0.0);
    m_videoSysTime.store(0);
    m_videoReady.store(false);
    m_audioPts.store(0.0);
    m_audioSysTime.store(0);
}

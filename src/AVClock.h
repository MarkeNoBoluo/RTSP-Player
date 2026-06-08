#pragma once

#include "Common.h"
#include <atomic>

class AVClock {
public:
    void       setVideoClock(double pts);
    ClockPoint videoClock() const;
    bool       isReady() const;

    void       setAudioClock(double pts);
    ClockPoint audioClock() const;

    double     drift() const;
    void       reset();

private:
    int64_t nowUs() const;

    std::atomic<double>  m_videoPts{0.0};
    std::atomic<int64_t> m_videoSysTime{0};
    std::atomic<bool>    m_videoReady{false};

    std::atomic<double>  m_audioPts{0.0};
    std::atomic<int64_t> m_audioSysTime{0};
};

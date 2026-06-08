#pragma once

#include <atomic>
#include <cstdint>

class PlayerStats {
public:
    std::atomic<int64_t> framesDecoded{0};
    std::atomic<int64_t> framesRendered{0};
    std::atomic<int64_t> framesDropped{0};
    std::atomic<int>     reconnectCount{0};
    std::atomic<int>     queueVideoDurationMs{0};
    std::atomic<int64_t> lastLatenessUs{0};

    double decodeFps() const;
    double renderFps() const;
    double dropRate() const;
    void   reset();
};

#include "PlayerStats.h"

double PlayerStats::decodeFps() const {
    return static_cast<double>(framesDecoded.load());
}

double PlayerStats::renderFps() const {
    return static_cast<double>(framesRendered.load());
}

double PlayerStats::dropRate() const {
    auto d = framesDropped.load();
    auto r = framesRendered.load();
    if (r == 0) return 0.0;
    return static_cast<double>(d) / static_cast<double>(r + d);
}

void PlayerStats::reset() {
    framesDecoded  = 0;
    framesRendered = 0;
    framesDropped  = 0;
    reconnectCount = 0;
    queueVideoDurationMs = 0;
}

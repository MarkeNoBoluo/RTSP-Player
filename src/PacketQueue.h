#pragma once

#include <deque>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
#ifdef __cplusplus
}
#endif

class PacketQueue {
public:
    PacketQueue();
    ~PacketQueue();

    void init(AVRational timeBase, int capacityMs = 200);
    bool push(AVPacket* pkt, int serial = 0);
    bool pop(AVPacket* pkt, int timeoutMs);
    void flush();
    void abort();
    int  size();
    int  durationMs() const;

    // Returns true if a key frame was dropped since last check (clears flag)
    bool checkKeyFrameDropped();

private:
    struct PacketNode {
        AVPacket* pkt = nullptr;
        int64_t   durationUs = 0;
        int       serial = 0;
    };

    bool    m_initialized = false;
    int     m_capacityMs  = 200;
    double  m_timeBaseUs  = 0.0;

    std::deque<PacketNode> m_queue;
    std::mutex             m_mutex;
    std::condition_variable m_cond;
    std::atomic<bool>       m_abort{false};
    std::atomic<bool>       m_keyFrameDropped{false};
    int                     m_totalDurationUs = 0;
};

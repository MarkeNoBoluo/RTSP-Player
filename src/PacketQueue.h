#pragma once

#include <queue>
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
    bool push(AVPacket* pkt);
    bool pop(AVPacket* pkt, int timeoutMs);
    void flush();
    void abort();
    int  size();
    int  durationMs() const;

private:
    struct PacketNode {
        AVPacket* pkt;
        int64_t   durationUs;
    };

    bool    m_initialized = false;
    int     m_capacityMs  = 200;
    double  m_timeBaseUs  = 0.0;

    std::deque<PacketNode> m_queue;
    std::mutex             m_mutex;
    std::condition_variable m_cond;
    std::atomic<bool>       m_abort{false};
    int                     m_totalDurationUs = 0;
};

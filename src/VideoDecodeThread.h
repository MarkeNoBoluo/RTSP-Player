#pragma once

#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include <thread>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif

class PlayerStats;

class VideoDecodeThread {
public:
    VideoDecodeThread(AVCodecContext* codecCtx, AVRational timeBase,
                      PacketQueue* queue, VideoFrameQueue* frameQueue,
                      PlayerStats* stats);
    ~VideoDecodeThread();

    void setSerial(int serial) { m_serial.store(serial, std::memory_order_release); }

    void start();
    void stop();
    void join();

private:
    void run();

    AVCodecContext*    m_codecCtx;
    AVRational         m_timeBase;
    PacketQueue*       m_queue;
    VideoFrameQueue*   m_frameQueue;
    PlayerStats*       m_stats;

    std::atomic<bool>  m_abort{false};
    std::atomic<int>   m_serial{0};
    std::thread        m_thread;
};

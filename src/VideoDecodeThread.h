#pragma once

#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include <QThread>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif

class PlayerStats;

class VideoDecodeThread : public QThread {
    Q_OBJECT
public:
    VideoDecodeThread(AVCodecContext* codecCtx, AVRational timeBase,
                      PacketQueue* queue, VideoFrameQueue* frameQueue,
                      PlayerStats* stats, QObject* parent = nullptr);
    ~VideoDecodeThread() override;

    void stop();

protected:
    void run() override;

private:
    AVCodecContext*    m_codecCtx;
    AVRational         m_timeBase;
    PacketQueue*       m_queue;
    VideoFrameQueue*   m_frameQueue;
    PlayerStats*       m_stats;
    std::atomic<bool>  m_abort{false};
};

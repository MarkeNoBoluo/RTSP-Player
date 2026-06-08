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
    VideoDecodeThread(PacketQueue* queue, VideoFrameQueue* frameQueue,
                      PlayerStats* stats, QObject* parent = nullptr);
    ~VideoDecodeThread() override;

    bool open(AVCodecParameters* codecPar, AVRational timeBase);
    void stop();
    AVCodecContext* codecCtx() const { return m_codecCtx; }

protected:
    void run() override;

private:
    PacketQueue*       m_queue;
    VideoFrameQueue*   m_frameQueue;
    PlayerStats*       m_stats;
    AVCodecContext*    m_codecCtx = nullptr;
    std::atomic<bool>  m_abort{false};
    AVRational         m_timeBase{1, 90000};
};

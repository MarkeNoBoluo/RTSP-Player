#pragma once

#include "PacketQueue.h"
#include <QThread>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#ifdef __cplusplus
}
#endif

class PlayerStateMachine;
class PlayerStats;

class DemuxThread : public QThread {
    Q_OBJECT
public:
    DemuxThread(PlayerStateMachine* sm, PlayerStats* stats,
                PacketQueue* videoQueue, PacketQueue* audioQueue,
                QObject* parent = nullptr);
    ~DemuxThread() override;

    void prepareForOpen();
    void setContext(AVFormatContext* fmtCtx, AVStream* videoStream, AVStream* audioStream);
    void stop();

    static int interruptCallback(void* opaque);

signals:
    void streamError();

protected:
    void run() override;

private:
    PlayerStateMachine* m_stateMachine;
    PlayerStats*        m_stats;
    AVFormatContext*    m_fmtCtx = nullptr;
    AVStream*           m_videoStream = nullptr;
    AVStream*           m_audioStream = nullptr;
    PacketQueue*        m_videoQueue;
    PacketQueue*        m_audioQueue;

    std::atomic<bool>   m_abort{false};
    int64_t             m_lastReadTime = 0;
};

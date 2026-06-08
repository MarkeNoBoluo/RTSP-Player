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

    bool open(const char* url);
    void stop();

    AVStream*     videoStream() const { return m_videoStream; }
    AVStream*     audioStream() const { return m_audioStream; }
    AVCodecParameters* videoCodecPar() const { return m_videoCodecPar; }
    AVCodecParameters* audioCodecPar() const { return m_audioCodecPar; }
    AVRational    videoTimeBase() const;
    AVRational    audioTimeBase() const;

signals:
    void streamInfoReady();

protected:
    void run() override;

private:
    static int interruptCallback(void* opaque);

    PlayerStateMachine* m_stateMachine;
    PlayerStats*        m_stats;
    PacketQueue*        m_videoQueue;
    PacketQueue*        m_audioQueue;

    AVFormatContext*    m_fmtCtx    = nullptr;
    AVStream*           m_videoStream = nullptr;
    AVStream*           m_audioStream = nullptr;
    AVCodecParameters*  m_videoCodecPar = nullptr;
    AVCodecParameters*  m_audioCodecPar = nullptr;

    std::atomic<bool>   m_abort{false};
    int64_t             m_lastReadTime = 0;
};

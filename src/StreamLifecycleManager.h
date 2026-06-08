#pragma once

#include "Common.h"
#include <QObject>
#include <QTimer>
#include <QThread>
#include <atomic>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#ifdef __cplusplus
}
#endif

class PlayerStateMachine;
class PacketQueue;
class VideoFrameQueue;
class AVClock;
class GLVideoWidget;
class PlayerStats;
class DemuxThread;
class VideoDecodeThread;
class RenderScheduler;
class AudioWorker;

class StreamLifecycleManager : public QObject {
    Q_OBJECT
public:
    StreamLifecycleManager(PlayerStateMachine* sm, PlayerStats* stats,
                           PacketQueue* videoQ, PacketQueue* audioQ,
                           VideoFrameQueue* frameQ, AVClock* clock,
                           GLVideoWidget* widget, QObject* parent = nullptr);
    ~StreamLifecycleManager() override;

    bool open(const char* url);
    void close();

    GLVideoWidget* videoWidget() const { return m_glWidget; }
    PlayerStats*   stats() const       { return m_stats; }
    PlayerState    state() const;

signals:
    void stateChanged(PlayerState state);
    void errorOccurred(const QString& message);

private slots:
    void onStreamError();

private:
    bool initDemux(const char* url);
    bool initDecoders();
    void startThreads();

    void shutdownPipeline();
    void scheduleReconnect();
    void doReconnect();

    PlayerStateMachine*  m_stateMachine;
    PlayerStats*         m_stats;
    PacketQueue*         m_videoQueue;
    PacketQueue*         m_audioQueue;
    VideoFrameQueue*     m_frameQueue;
    AVClock*             m_clock;
    GLVideoWidget*       m_glWidget;

    DemuxThread*         m_demuxThread     = nullptr;
    VideoDecodeThread*   m_decodeThread    = nullptr;
    RenderScheduler*     m_renderScheduler = nullptr;
    AudioWorker*         m_audioWorker     = nullptr;
    QThread*             m_audioThread     = nullptr;

    AVFormatContext*     m_fmtCtx          = nullptr;
    AVStream*            m_videoStream     = nullptr;
    AVStream*            m_audioStream     = nullptr;
    AVCodecParameters*   m_videoCodecPar   = nullptr;
    AVCodecParameters*   m_audioCodecPar   = nullptr;
    AVCodecContext*      m_videoCodecCtx   = nullptr;
    AVCodecContext*      m_audioCodecCtx   = nullptr;

    QTimer*              m_reconnectTimer = nullptr;
    int                  m_reconnectDelayMs = 1000;
    int                  m_backoffCount     = 0;

    std::atomic<uint64_t> m_generation{1};
    std::string           m_url;
};

#pragma once

#include "Common.h"
#include <QObject>

class PlayerStateMachine;
class PacketQueue;
class VideoFrameQueue;
class AVClock;
class GLVideoWidget;
class PlayerStats;
class DemuxThread;
class VideoDecodeThread;
class RenderScheduler;

class RTSPlayer : public QObject {
    Q_OBJECT
public:
    explicit RTSPlayer(QObject* parent = nullptr);
    ~RTSPlayer() override;

    bool open(const char* url);
    void close();

    GLVideoWidget* videoWidget() const { return m_glWidget; }
    PlayerStats*   stats() const       { return m_stats; }
    PlayerState    state() const;

signals:
    void stateChanged(PlayerState state);
    void errorOccurred(const QString& message);

private slots:
    void onStreamInfoReady();

private:
    void shutdown();

    PlayerStateMachine*  m_stateMachine;
    PacketQueue*         m_videoQueue;
    PacketQueue*         m_audioQueue;
    VideoFrameQueue*     m_frameQueue;
    AVClock*             m_clock;
    GLVideoWidget*       m_glWidget;
    PlayerStats*         m_stats;
    DemuxThread*         m_demuxThread;
    VideoDecodeThread*   m_videoDecodeThread;
    RenderScheduler*     m_renderScheduler;

    bool m_initialized = false;
};

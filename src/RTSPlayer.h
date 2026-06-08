#pragma once

#include "Common.h"
#include <QObject>

class PlayerStateMachine;
class PacketQueue;
class VideoFrameQueue;
class AVClock;
class GLVideoWidget;
class PlayerStats;
class StreamLifecycleManager;

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

private:
    PlayerStateMachine*     m_stateMachine;
    PacketQueue*            m_videoQueue;
    PacketQueue*            m_audioQueue;
    VideoFrameQueue*        m_frameQueue;
    AVClock*                m_clock;
    GLVideoWidget*          m_glWidget;
    PlayerStats*            m_stats;
    StreamLifecycleManager* m_lifecycle;
};

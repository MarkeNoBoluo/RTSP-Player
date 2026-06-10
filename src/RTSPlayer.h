#pragma once

#include "Common.h"
#include <functional>

class PlayerStateMachine;
class PacketQueue;
class VideoFrameQueue;
class AVClock;
class PlayerStats;
class StreamLifecycleManager;
class IRenderer;

class RTSPlayer {
public:
    using StateCallback = std::function<void(PlayerState)>;
    using ErrorCallback = std::function<void(const char*)>;

    RTSPlayer();
    ~RTSPlayer();

    bool open(const char* url);
    void close();

    void         setRenderer(IRenderer* renderer);
    IRenderer*   renderer() const;
    PlayerStats* stats()    const;
    PlayerState  state()    const;

    void setStateCallback(StateCallback cb);
    void setErrorCallback(ErrorCallback cb);

    void videoRefresh();

    int pktSerial() const;

private:
    PlayerStateMachine*     m_stateMachine;
    PacketQueue*            m_videoQueue;
    PacketQueue*            m_audioQueue;
    VideoFrameQueue*        m_frameQueue;
    AVClock*                m_clock;
    PlayerStats*            m_stats;
    IRenderer*              m_renderer = nullptr;
    StreamLifecycleManager* m_lifecycle;

    int64_t m_consecutiveDrops = 0;
    int64_t m_lastRenderUs    = 0;
    bool    m_inVideoRefresh   = false;   // reentrancy guard
};

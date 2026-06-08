#include "RTSPlayer.h"
#include "PlayerStateMachine.h"
#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include "AVClock.h"
#include "GLVideoWidget.h"
#include "PlayerStats.h"
#include "StreamLifecycleManager.h"
#include "logger/Logger.h"

RTSPlayer::RTSPlayer(QObject* parent)
    : QObject(parent)
    , m_stateMachine(new PlayerStateMachine)
    , m_videoQueue(new PacketQueue)
    , m_audioQueue(new PacketQueue)
    , m_frameQueue(new VideoFrameQueue)
    , m_clock(new AVClock)
    , m_glWidget(new GLVideoWidget)
    , m_stats(new PlayerStats)
    , m_lifecycle(new StreamLifecycleManager(m_stateMachine, m_stats,
                   m_videoQueue, m_audioQueue, m_frameQueue, m_clock,
                   m_glWidget, this))
{
    connect(m_lifecycle, &StreamLifecycleManager::stateChanged,
            this, &RTSPlayer::stateChanged);
    connect(m_lifecycle, &StreamLifecycleManager::errorOccurred,
            this, &RTSPlayer::errorOccurred);
}

RTSPlayer::~RTSPlayer() {
    close();
}

bool RTSPlayer::open(const char* url) {
    return m_lifecycle->open(url);
}

void RTSPlayer::close() {
    m_lifecycle->close();
}

PlayerState RTSPlayer::state() const {
    return m_stateMachine->state();
}

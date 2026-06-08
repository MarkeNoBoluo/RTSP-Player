#include "RTSPlayer.h"
#include "PlayerStateMachine.h"
#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include "AVClock.h"
#include "GLVideoWidget.h"
#include "PlayerStats.h"
#include "DemuxThread.h"
#include "VideoDecodeThread.h"
#include "RenderScheduler.h"
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
    , m_demuxThread(new DemuxThread(m_stateMachine, m_stats, m_videoQueue, m_audioQueue, this))
    , m_videoDecodeThread(new VideoDecodeThread(m_videoQueue, m_frameQueue, m_stats, this))
    , m_renderScheduler(new RenderScheduler(m_frameQueue, m_clock, m_glWidget, m_stats, this))
{
    connect(m_demuxThread, &DemuxThread::streamInfoReady,
            this, &RTSPlayer::onStreamInfoReady);
}

RTSPlayer::~RTSPlayer() {
    close();
}

bool RTSPlayer::open(const char* url) {
    if (!m_stateMachine->transition(PlayerState::Stopped, PlayerState::Connecting)) {
        return false;
    }

    m_stateMachine->forceState(PlayerState::Stopped);
    LOG_INFO("Opening stream: %s", url);
    shutdown();

    if (!m_stateMachine->transition(PlayerState::Stopped, PlayerState::Connecting)) {
        return false;
    }

    if (!m_demuxThread->open(url)) {
        m_stateMachine->forceState(PlayerState::Error);
        emit stateChanged(PlayerState::Error);
        emit errorOccurred(QStringLiteral("Failed to open RTSP stream"));
        LOG_ERROR("Failed to open stream: %s", url);
        return false;
    }

    m_initialized = true;
    m_demuxThread->start();
    LOG_INFO("Demux thread started, waiting for stream info...");
    return true;
}

void RTSPlayer::close() {
    if (m_stateMachine->state() == PlayerState::Stopped) return;

    m_stateMachine->transition(m_stateMachine->state(), PlayerState::Closing);
    emit stateChanged(PlayerState::Closing);

    shutdown();
    m_stateMachine->forceState(PlayerState::Stopped);
    emit stateChanged(PlayerState::Stopped);
}

void RTSPlayer::shutdown() {
    LOG_INFO("Shutting down player");
    if (m_demuxThread->isRunning()) {
        m_demuxThread->stop();
        m_demuxThread->wait(3000);
    }
    if (m_videoDecodeThread->isRunning()) {
        m_videoDecodeThread->stop();
        m_videoDecodeThread->wait(3000);
    }
    if (m_renderScheduler->isRunning()) {
        m_renderScheduler->stop();
        m_renderScheduler->wait(3000);
    }

    m_videoQueue->flush();
    m_audioQueue->flush();
    m_frameQueue->flush();
    m_clock->reset();
    LOG_INFO("Player shutdown complete");
    m_initialized = false;
}

void RTSPlayer::onStreamInfoReady() {
    LOG_INFO("Stream info ready, opening decoder");
    auto* codecPar = m_demuxThread->videoCodecPar();
    if (!codecPar) {
        LOG_ERROR("No video codec parameters in stream");
        m_stateMachine->forceState(PlayerState::Error);
        emit errorOccurred(QStringLiteral("No video codec parameters"));
        return;
    }

    AVRational timeBase = m_demuxThread->videoTimeBase();

    if (!m_videoDecodeThread->open(codecPar, timeBase)) {
        LOG_ERROR("Failed to open video decoder");
        m_stateMachine->forceState(PlayerState::Error);
        emit errorOccurred(QStringLiteral("Failed to open video decoder"));
        return;
    }

    LOG_INFO("Video decoder opened, starting playback threads");
    m_videoDecodeThread->start();
    m_renderScheduler->start();
}

PlayerState RTSPlayer::state() const {
    return m_stateMachine->state();
}

#include "StreamLifecycleManager.h"
#include "PlayerStateMachine.h"
#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include "AVClock.h"
#include "GLVideoWidget.h"
#include "PlayerStats.h"
#include "DemuxThread.h"
#include "VideoDecodeThread.h"
#include "RenderScheduler.h"
#include "AudioWorker.h"
#include "AudioPullDevice.h"
#include "logger/Logger.h"

#include <QAudioOutput>
#include <QAudioFormat>
#include <QAudioDeviceInfo>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
}

StreamLifecycleManager::StreamLifecycleManager(
    PlayerStateMachine* sm, PlayerStats* stats,
    PacketQueue* videoQ, PacketQueue* audioQ,
    VideoFrameQueue* frameQ, AVClock* clock,
    GLVideoWidget* widget, QObject* parent)
    : QObject(parent)
    , m_stateMachine(sm)
    , m_stats(stats)
    , m_videoQueue(videoQ)
    , m_audioQueue(audioQ)
    , m_frameQueue(frameQ)
    , m_clock(clock)
    , m_glWidget(widget)
{
    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setSingleShot(true);
    connect(m_reconnectTimer, &QTimer::timeout, this, &StreamLifecycleManager::doReconnect);
}

StreamLifecycleManager::~StreamLifecycleManager() {
    close();
}

PlayerState StreamLifecycleManager::state() const {
    return m_stateMachine->state();
}

bool StreamLifecycleManager::open(const char* url) {
    if (!m_stateMachine->transition(PlayerState::Stopped, PlayerState::Connecting)) {
        return false;
    }

    m_stateMachine->forceState(PlayerState::Stopped);
    shutdownPipeline();

    if (!m_stateMachine->transition(PlayerState::Stopped, PlayerState::Connecting)) {
        return false;
    }

    m_generation++;
    m_url = url;
    m_backoffCount = 0;
    m_reconnectDelayMs = 1000;

    if (!initDemux(url)) {
        m_stateMachine->forceState(PlayerState::Error);
        emit stateChanged(PlayerState::Error);
        emit errorOccurred(QStringLiteral("Failed to open RTSP stream"));
        return false;
    }

    if (!initDecoders()) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        m_stateMachine->forceState(PlayerState::Error);
        emit stateChanged(PlayerState::Error);
        emit errorOccurred(QStringLiteral("Failed to open decoder"));
        return false;
    }

    startThreads();
    return true;
}

void StreamLifecycleManager::close() {
    if (m_stateMachine->state() == PlayerState::Stopped) return;

    m_generation++;
    m_reconnectTimer->stop();

    m_stateMachine->transition(m_stateMachine->state(), PlayerState::Closing);
    emit stateChanged(PlayerState::Closing);

    shutdownPipeline();
    m_stateMachine->forceState(PlayerState::Stopped);
    emit stateChanged(PlayerState::Stopped);
}

void StreamLifecycleManager::onStreamError() {
    if (!m_stateMachine->transition(PlayerState::Playing, PlayerState::Recovering)) {
        return;
    }
    emit stateChanged(PlayerState::Recovering);

    LOG_INFO("Stream error detected, shutting down for reconnect");

    shutdownPipeline();
    scheduleReconnect();
}

bool StreamLifecycleManager::initDemux(const char* url) {
    LOG_INFO("Opening stream: %s", url);

    delete m_demuxThread;
    m_demuxThread = new DemuxThread(m_stateMachine, m_stats,
                                     m_videoQueue, m_audioQueue, this);
    m_demuxThread->prepareForOpen();

    m_fmtCtx = avformat_alloc_context();
    if (!m_fmtCtx) {
        LOG_ERROR("Failed to allocate format context");
        return false;
    }

    AVIOInterruptCB intrCb = { DemuxThread::interruptCallback, m_demuxThread };
    m_fmtCtx->interrupt_callback = intrCb;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "probesize", "32768", 0);
    av_dict_set(&opts, "analyzeduration", "100000", 0);
    av_dict_set(&opts, "max_delay", "100000", 0);

    int ret = avformat_open_input(&m_fmtCtx, url, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[256] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("avformat_open_input failed: %s (code=%d)", errbuf, ret);
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        return false;
    }

    m_fmtCtx->flags |= AVFMT_FLAG_NOBUFFER;
    m_fmtCtx->max_analyze_duration = 100000;

    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) {
        char errbuf[256] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("avformat_find_stream_info failed: %s (code=%d)", errbuf, ret);
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        return false;
    }

    int videoIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int audioIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (videoIdx >= 0) {
        m_videoStream   = m_fmtCtx->streams[videoIdx];
        m_videoCodecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(m_videoCodecPar, m_videoStream->codecpar);
        m_videoQueue->init(m_videoStream->time_base, 200);
        LOG_INFO("Video stream: index=%d, codec=%d, %dx%d",
                 videoIdx, m_videoCodecPar->codec_id,
                 m_videoCodecPar->width, m_videoCodecPar->height);
    }

    if (audioIdx >= 0) {
        m_audioStream   = m_fmtCtx->streams[audioIdx];
        m_audioCodecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(m_audioCodecPar, m_audioStream->codecpar);
        m_audioQueue->init(m_audioStream->time_base, 80);
        LOG_INFO("Audio stream: index=%d, codec=%d, %dHz/%dch",
                 audioIdx, m_audioCodecPar->codec_id,
                 m_audioCodecPar->sample_rate, m_audioCodecPar->channels);
    }

    if (!m_videoStream) {
        LOG_ERROR("No video stream found");
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        return false;
    }

    m_demuxThread->setContext(m_fmtCtx, m_videoStream, m_audioStream);
    connect(m_demuxThread, &DemuxThread::streamError,
            this, &StreamLifecycleManager::onStreamError);

    LOG_INFO("Demux initialization complete");
    return true;
}

bool StreamLifecycleManager::initDecoders() {
    // Video decoder
    if (m_videoCodecPar) {
        const AVCodec* codec = avcodec_find_decoder(m_videoCodecPar->codec_id);
        if (!codec) {
            LOG_ERROR("Video decoder not found for codec_id=%d", m_videoCodecPar->codec_id);
            return false;
        }
        m_videoCodecCtx = avcodec_alloc_context3(codec);
        if (!m_videoCodecCtx) return false;
        avcodec_parameters_to_context(m_videoCodecCtx, m_videoCodecPar);
        m_videoCodecCtx->thread_count = 2;
        if (avcodec_open2(m_videoCodecCtx, codec, nullptr) < 0) return false;
        LOG_INFO("Video decoder opened: %dx%d", m_videoCodecCtx->width, m_videoCodecCtx->height);
    }

    // Audio decoder
    if (m_audioCodecPar) {
        const AVCodec* codec = avcodec_find_decoder(m_audioCodecPar->codec_id);
        if (!codec) {
            LOG_ERROR("Audio decoder not found for codec_id=%d", m_audioCodecPar->codec_id);
            return false;
        }
        m_audioCodecCtx = avcodec_alloc_context3(codec);
        if (!m_audioCodecCtx) return false;
        avcodec_parameters_to_context(m_audioCodecCtx, m_audioCodecPar);
        m_audioCodecCtx->thread_count = 1;
        if (avcodec_open2(m_audioCodecCtx, codec, nullptr) < 0) return false;
        LOG_INFO("Audio decoder opened: %dHz/%dch",
                 m_audioCodecCtx->sample_rate, m_audioCodecCtx->channels);
    }

    return true;
}

void StreamLifecycleManager::startThreads() {
    delete m_decodeThread;
    delete m_renderScheduler;

    AVRational videoTimeBase = m_videoStream ? m_videoStream->time_base : AVRational{1, 90000};

    if (m_videoCodecCtx) {
        m_decodeThread = new VideoDecodeThread(m_videoCodecCtx, videoTimeBase,
                                                m_videoQueue, m_frameQueue, m_stats, this);
    }

    m_renderScheduler = new RenderScheduler(m_frameQueue, m_clock, m_glWidget, m_stats, this);

    if (m_videoStream && m_videoStream->avg_frame_rate.num > 0 && m_videoStream->avg_frame_rate.den > 0) {
        double fps = av_q2d(m_videoStream->avg_frame_rate);
        m_renderScheduler->setFrameDuration((1.0 / fps) * 1000000.0);
    } else {
        m_renderScheduler->setFrameDuration(33333.0);
    }

    // Audio output on GUI thread (WASAPI requires message pump)
    if (m_audioCodecCtx) {
        delete m_audioWorker;
        delete m_audioThread;
        delete m_audioOutput;
        delete m_audioPullDevice;

        AVRational audioTimeBase = m_audioStream ? m_audioStream->time_base : AVRational{1, 90000};

        m_audioPullDevice = new AudioPullDevice(200, this);
        m_audioPullDevice->open(QIODevice::ReadOnly);

        QAudioFormat format;
        format.setSampleRate(48000);
        format.setChannelCount(2);
        format.setSampleSize(16);
        format.setCodec("audio/pcm");
        format.setByteOrder(QAudioFormat::LittleEndian);
        format.setSampleType(QAudioFormat::SignedInt);

        QAudioDeviceInfo info = QAudioDeviceInfo::defaultOutputDevice();
        if (info.isFormatSupported(format)) {
            m_audioOutput = new QAudioOutput(info, format, this);
            m_audioOutput->setBufferSize(4096);
            m_audioOutput->start(m_audioPullDevice);
            LOG_INFO("Audio output started: 48000Hz/stereo/s16 (pull mode)");
        } else {
            LOG_ERROR("Audio format not supported, audio disabled");
        }

        m_audioThread = new QThread(this);
        m_audioWorker = new AudioWorker(m_audioCodecCtx, audioTimeBase,
                                         m_audioQueue, m_clock,
                                         m_audioPullDevice);
        m_audioWorker->moveToThread(m_audioThread);

        connect(m_audioThread, &QThread::started, m_audioWorker, &AudioWorker::start);
        connect(m_audioWorker, &AudioWorker::destroyed, m_audioThread, &QThread::quit);
        connect(m_audioThread, &QThread::finished, m_audioThread, &QObject::deleteLater);
    }

    m_demuxThread->start();
    if (m_decodeThread) m_decodeThread->start();
    m_renderScheduler->start();
    if (m_audioThread) m_audioThread->start();
}

void StreamLifecycleManager::shutdownPipeline() {
    LOG_INFO("Shutting down pipeline");

    m_reconnectTimer->stop();

    m_videoQueue->abort();
    m_audioQueue->abort();
    m_frameQueue->notifyAll();

    // Stop audio thread first (uses audio queue)
    if (m_audioWorker) {
        m_audioWorker->stop();
    }
    if (m_audioThread && m_audioThread->isRunning()) {
        if (!m_audioThread->wait(3000)) {
            LOG_ERROR("AudioThread wait timeout — abandoned");
        }
    }

    if (m_demuxThread) {
        m_demuxThread->stop();
        if (m_demuxThread->isRunning()) {
            if (!m_demuxThread->wait(3000)) {
                LOG_ERROR("DemuxThread wait timeout — abandoned");
            }
        }
    }
    if (m_decodeThread) {
        m_decodeThread->stop();
        if (m_decodeThread->isRunning()) {
            if (!m_decodeThread->wait(3000)) {
                LOG_ERROR("VideoDecodeThread wait timeout — abandoned");
            }
        }
    }
    if (m_renderScheduler) {
        m_renderScheduler->stop();
        if (m_renderScheduler->isRunning()) {
            if (!m_renderScheduler->wait(3000)) {
                LOG_ERROR("RenderScheduler wait timeout — abandoned");
            }
        }
    }

    delete m_demuxThread;
    delete m_decodeThread;
    delete m_renderScheduler;
    delete m_audioWorker;
    delete m_audioOutput;
    delete m_audioPullDevice;
    m_demuxThread     = nullptr;
    m_decodeThread    = nullptr;
    m_renderScheduler = nullptr;
    m_audioWorker     = nullptr;
    m_audioThread     = nullptr;
    m_audioOutput     = nullptr;
    m_audioPullDevice = nullptr;

    if (m_videoCodecCtx) { avcodec_flush_buffers(m_videoCodecCtx); avcodec_free_context(&m_videoCodecCtx); }
    if (m_audioCodecCtx) { avcodec_flush_buffers(m_audioCodecCtx); avcodec_free_context(&m_audioCodecCtx); }

    if (m_videoCodecPar) { avcodec_parameters_free(&m_videoCodecPar); }
    if (m_audioCodecPar) { avcodec_parameters_free(&m_audioCodecPar); }

    if (m_fmtCtx) {
        avformat_close_input(&m_fmtCtx);
    }

    m_fmtCtx         = nullptr;
    m_videoStream    = nullptr;
    m_audioStream    = nullptr;
    m_videoCodecPar  = nullptr;
    m_audioCodecPar  = nullptr;
    m_videoCodecCtx  = nullptr;
    m_audioCodecCtx  = nullptr;

    m_videoQueue->flush();
    m_audioQueue->flush();
    m_frameQueue->flush();
    m_clock->reset();

    LOG_INFO("Pipeline shutdown complete");
}

void StreamLifecycleManager::scheduleReconnect() {
    m_stateMachine->transition(PlayerState::Recovering, PlayerState::Reconnecting);
    emit stateChanged(PlayerState::Reconnecting);

    LOG_INFO("Reconnecting in %d ms (attempt #%d)", m_reconnectDelayMs, m_backoffCount + 1);
    m_reconnectTimer->start(m_reconnectDelayMs);
}

void StreamLifecycleManager::doReconnect() {
    if (!m_stateMachine->transition(PlayerState::Reconnecting, PlayerState::Connecting)) {
        LOG_INFO("Reconnect skipped: not in Reconnecting state");
        return;
    }
    emit stateChanged(PlayerState::Connecting);

    LOG_INFO("Attempting reconnect #%d to %s", m_backoffCount + 1, m_url.c_str());

    if (!initDemux(m_url.c_str())) {
        m_backoffCount++;
        m_reconnectDelayMs = m_reconnectDelayMs * 2;
        if (m_reconnectDelayMs > 8000) m_reconnectDelayMs = 8000;
        shutdownPipeline();
        scheduleReconnect();
        return;
    }

    if (!initDecoders()) {
        m_backoffCount++;
        m_reconnectDelayMs = m_reconnectDelayMs * 2;
        if (m_reconnectDelayMs > 8000) m_reconnectDelayMs = 8000;
        shutdownPipeline();
        scheduleReconnect();
        return;
    }

    m_backoffCount = 0;
    m_reconnectDelayMs = 1000;

    startThreads();
    LOG_INFO("Reconnect successful");
}

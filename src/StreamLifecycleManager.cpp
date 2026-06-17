#include "StreamLifecycleManager.h"
#include "PlayerStateMachine.h"
#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include "AVClock.h"
#include "PlayerStats.h"
#include "DemuxThread.h"
#include "VideoDecodeThread.h"
#include "AudioWorker.h"
#include "AudioRingBuffer.h"
#include "SDLAudio.h"
#include "logger/Logger.h"

#include <algorithm>
#include <chrono>
#include <thread>
#include <SDL.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/time.h>
}

StreamLifecycleManager::StreamLifecycleManager(
    PlayerStateMachine* sm, PlayerStats* stats,
    PacketQueue* videoQ, PacketQueue* audioQ,
    VideoFrameQueue* frameQ, AVClock* clock)
    : m_stateMachine(sm)
    , m_stats(stats)
    , m_videoQueue(videoQ)
    , m_audioQueue(audioQ)
    , m_frameQueue(frameQ)
    , m_clock(clock)
    ,m_audioEnabled(true)
{
    m_stats->initCsv("stats.csv");
}

StreamLifecycleManager::~StreamLifecycleManager() {
    close();
    m_stats->closeCsv();
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

    if (!initDemux(url)) {
        m_stateMachine->forceState(PlayerState::Error);
        if (m_onState) m_onState(PlayerState::Error);
        if (m_onError) m_onError("Failed to open RTSP stream");
        return false;
    }

    if (!initDecoders()) {
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        m_stateMachine->forceState(PlayerState::Error);
        if (m_onState) m_onState(PlayerState::Error);
        if (m_onError) m_onError("Failed to open decoder");
        return false;
    }

    startThreads();
    return true;
}

void StreamLifecycleManager::close() {
    if (m_stateMachine->state() == PlayerState::Stopped) return;

    m_generation++;

    m_stateMachine->transition(m_stateMachine->state(), PlayerState::Closing);
    if (m_onState) m_onState(PlayerState::Closing);

    shutdownPipeline();
    m_stateMachine->forceState(PlayerState::Stopped);
    if (m_onState) m_onState(PlayerState::Stopped);
}

int StreamLifecycleManager::calcBackoffMs() {
    int delay = 1000 << std::min(m_backoffCount, 3);
    m_backoffCount++;
    return delay;
}

bool StreamLifecycleManager::initDemux(const char* url) {
    LOG_INFO("Opening stream: %s", url);

    delete m_demuxThread;
    m_demuxThread = new DemuxThread(m_stateMachine, m_stats,
                                    m_videoQueue, m_audioQueue);
    m_demuxThread->prepareForOpen();
    m_demuxThread->setStreamErrorCallback([this]() {
        if (!m_stateMachine->transition(PlayerState::Playing, PlayerState::Recovering)) {
            return;
        }
        if (m_onState) m_onState(PlayerState::Recovering);
        m_stats->reconnectStartUs.store(av_gettime_relative());
        LOG_INFO("Stream error detected, shutting down for reconnect");
        shutdownPipeline();
        scheduleReconnect();
    });

    m_fmtCtx = avformat_alloc_context();
    if (!m_fmtCtx) {
        LOG_ERROR("Failed to allocate format context");
        return false;
    }

    AVIOInterruptCB intrCb = { DemuxThread::interruptCallback, m_demuxThread };
    m_fmtCtx->interrupt_callback = intrCb;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "udp", 0);
    av_dict_set(&opts, "probesize", "32000", 0);
    av_dict_set(&opts, "analyzeduration", "0", 0);
    av_dict_set(&opts, "max_delay", "100000", 0);

    int ret = avformat_open_input(&m_fmtCtx, url, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char errbuf[256] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("avformat_open_input failed: %s", errbuf);
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        return false;
    }

    m_fmtCtx->flags |= AVFMT_FLAG_NOBUFFER;
    m_fmtCtx->max_analyze_duration = 5000000;

    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) {
        char errbuf[256] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("avformat_find_stream_info failed: %s", errbuf);
        avformat_close_input(&m_fmtCtx);
        m_fmtCtx = nullptr;
        return false;
    }

    int videoIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int audioIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    // Fallback: if av_find_best_stream fails, manually search for audio stream
    if (audioIdx < 0) {
        for (unsigned i = 0; i < m_fmtCtx->nb_streams; i++) {
            if (m_fmtCtx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
                audioIdx = (int)i;
                LOG_INFO("Audio stream fallback: found stream %d (codec=%d)",
                         audioIdx, m_fmtCtx->streams[i]->codecpar->codec_id);
                break;
            }
        }
    }

    if (videoIdx >= 0) {
        m_videoStream   = m_fmtCtx->streams[videoIdx];
        m_videoCodecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(m_videoCodecPar, m_videoStream->codecpar);
        m_videoQueue->init(m_videoStream->time_base, m_videoQueueCapacityMs, "video");
        LOG_INFO("Video stream: index=%d, codec=%d, %dx%d",
                 videoIdx, m_videoCodecPar->codec_id,
                 m_videoCodecPar->width, m_videoCodecPar->height);
    }

    if (audioIdx >= 0 && m_audioEnabled) {
        m_audioStream   = m_fmtCtx->streams[audioIdx];
        m_audioCodecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(m_audioCodecPar, m_audioStream->codecpar);
        m_audioQueue->init(m_audioStream->time_base, 200, "audio");
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

    LOG_INFO("Demux initialization complete");
    return true;
}

bool StreamLifecycleManager::initDecoders() {
    if (m_videoCodecPar) {
        const AVCodec* codec = avcodec_find_decoder(m_videoCodecPar->codec_id);
        if (!codec) {
            LOG_ERROR("Video decoder not found");
            return false;
        }
        m_videoCodecCtx = avcodec_alloc_context3(codec);
        if (!m_videoCodecCtx) return false;
        avcodec_parameters_to_context(m_videoCodecCtx, m_videoCodecPar);
        m_videoCodecCtx->thread_count = 0;
        if (avcodec_open2(m_videoCodecCtx, codec, nullptr) < 0) return false;
        LOG_INFO("Video decoder opened: %dx%d", m_videoCodecCtx->width, m_videoCodecCtx->height);
    }

    if (m_audioCodecPar && m_audioEnabled) {
        const AVCodec* codec = avcodec_find_decoder(m_audioCodecPar->codec_id);
        if (!codec) {
            LOG_ERROR("Audio decoder not found");
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
    m_decodeThread = nullptr;

    AVRational videoTimeBase = m_videoStream ? m_videoStream->time_base : AVRational{1, 90000};

    if (m_videoCodecCtx) {
        m_decodeThread = new VideoDecodeThread(m_videoCodecCtx, videoTimeBase,
                                                m_videoQueue, m_frameQueue, m_stats);
    }

    // Audio pipeline
    if (m_audioCodecCtx && m_audioEnabled) {
        delete m_audioWorker;
        m_audioWorker = nullptr;
        delete m_audioRingBuffer;
        m_audioRingBuffer = nullptr;
        delete m_sdlAudio;
        m_sdlAudio = nullptr;

        AVRational audioTimeBase = m_audioStream ? m_audioStream->time_base : AVRational{1, 90000};

        m_audioRingBuffer = new AudioRingBuffer(100);
        m_audioRingBuffer->setStats(m_stats);

        m_sdlAudio = new SDLAudio(m_audioRingBuffer, m_clock, m_stats);
        if (!m_sdlAudio->init(48000, 2)) {
            LOG_WARN("SDL audio init failed, audio disabled");
            delete m_sdlAudio;
            m_sdlAudio = nullptr;
            delete m_audioRingBuffer;
            m_audioRingBuffer = nullptr;
        } else {
            m_audioWorker = new AudioWorker(m_audioCodecCtx, audioTimeBase,
                                              m_audioQueue, m_clock,
                                              m_audioRingBuffer, m_stats);
            LOG_INFO("Audio pipeline: SDLAudio + AudioRingBuffer(100ms)");
        }
    }

    // Set initial serial
    incrementSerial();

    // Start decoder threads first, wait for ready, then start demux
    if (m_decodeThread) m_decodeThread->start();
    if (m_audioWorker && m_audioEnabled) m_audioWorker->start();
    if (m_sdlAudio && m_audioEnabled) m_sdlAudio->start();

    // Wait for video decode thread to signal ready (timeout 2s)
    if (m_decodeThread) {
        int waited = 0;
        while (!m_decodeThread->isReady() && waited < 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            waited += 5;
        }
        if (!m_decodeThread->isReady()) {
            LOG_WARN("Video decode thread not ready after %dms, starting demux anyway", waited);
        } else {
            LOG_DEBUG("Video decode thread ready after %dms", waited);
        }
    }

    // Wait for audio worker to signal ready (timeout 2s)
    if (m_audioWorker && m_audioEnabled) {
        int waited = 0;
        while (!m_audioWorker->isReady() && waited < 2000) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            waited += 5;
        }
        if (!m_audioWorker->isReady()) {
            LOG_WARN("Audio worker not ready after %dms, starting demux anyway", waited);
        } else {
            LOG_DEBUG("Audio worker ready after %dms", waited);
        }
    }

    // Now start demux — decoders are ready to consume
    m_demuxThread->start();

    LOG_INFO("All threads started");
}

void StreamLifecycleManager::shutdownPipeline() {
    LOG_INFO("Shutting down pipeline");

    if (m_reconnectTimerId) {
        SDL_RemoveTimer(m_reconnectTimerId);
        m_reconnectTimerId = 0;
    }

    m_videoQueue->abort();
    m_audioQueue->abort();

    if (m_sdlAudio && m_audioEnabled) {
        m_sdlAudio->stop();
    }

    if (m_audioWorker && m_audioEnabled) {
        m_audioWorker->stop();
        m_audioWorker->join();
    }

    if (m_demuxThread) {
        m_demuxThread->stop();
        m_demuxThread->join();
    }
    if (m_decodeThread) {
        m_decodeThread->stop();
        m_decodeThread->join();
    }

    if (m_sdlAudio && m_audioEnabled) {
        m_sdlAudio->close();
    }

    delete m_demuxThread;
    delete m_decodeThread;
    

    m_demuxThread     = nullptr;
    m_decodeThread    = nullptr;

    if(m_audioEnabled){
        delete m_audioWorker;
        delete m_sdlAudio;
        delete m_audioRingBuffer;
        m_audioWorker     = nullptr;
        m_sdlAudio        = nullptr;
        m_audioRingBuffer = nullptr;
    }

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

static Uint32 onReconnectTimer(Uint32 interval, void* param) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_USEREVENT;
    event.user.code = EVENT_RECONNECT;
    event.user.data1 = param;
    SDL_PushEvent(&event);
    return 0; // non-repeating
}

void StreamLifecycleManager::scheduleReconnect() {
    m_stateMachine->transition(PlayerState::Recovering, PlayerState::Reconnecting);
    if (m_onState) m_onState(PlayerState::Reconnecting);

    int delay = calcBackoffMs();
    LOG_INFO("Reconnecting in %d ms (attempt #%d)", delay, m_backoffCount);

    m_reconnectTimerId = SDL_AddTimer(delay, onReconnectTimer, this);
}

void StreamLifecycleManager::doReconnect() {
    m_reconnectTimerId = 0;

    if (!m_stateMachine->transition(PlayerState::Reconnecting, PlayerState::Connecting)) {
        LOG_INFO("Reconnect skipped: not in Reconnecting state");
        return;
    }
    if (m_onState) m_onState(PlayerState::Connecting);

    LOG_INFO("Attempting reconnect #%d to %s", m_stats->reconnectCount.load() + 1, m_url.c_str());

    if (!initDemux(m_url.c_str())) {
        scheduleReconnect();
        return;
    }

    if (!initDecoders()) {
        shutdownPipeline();
        scheduleReconnect();
        return;
    }

    m_backoffCount = 0;

    m_stats->reconnectCount++;
    int64_t reconnectUs = m_stats->reconnectStartUs.load();
    if (reconnectUs > 0) {
        int64_t recoveryMs = (av_gettime_relative() - reconnectUs) / 1000;
        m_stats->totalReconnectMs.fetch_add(recoveryMs);
        m_stats->reconnectStartUs.store(0);
    }

    startThreads();
    LOG_INFO("Reconnect successful");
}

void StreamLifecycleManager::incrementSerial() {
    int s = m_pktSerial.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (m_demuxThread) m_demuxThread->setSerial(s);
    if (m_decodeThread) m_decodeThread->setSerial(s);
    if (m_audioWorker && m_audioEnabled) m_audioWorker->setSerial(s);
    if (m_audioRingBuffer && m_audioEnabled) m_audioRingBuffer->setSerial(s);
}

#include "DemuxThread.h"
#include "PlayerStateMachine.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/time.h>
}

DemuxThread::DemuxThread(PlayerStateMachine* sm, PlayerStats* stats,
                         PacketQueue* videoQueue, PacketQueue* audioQueue,
                         QObject* parent)
    : QThread(parent)
    , m_stateMachine(sm)
    , m_stats(stats)
    , m_videoQueue(videoQueue)
    , m_audioQueue(audioQueue)
{
}

DemuxThread::~DemuxThread() {
    stop();
    wait();
    if (m_videoCodecPar) avcodec_parameters_free(&m_videoCodecPar);
    if (m_audioCodecPar) avcodec_parameters_free(&m_audioCodecPar);
    if (m_fmtCtx) avformat_close_input(&m_fmtCtx);
}

bool DemuxThread::open(const char* url) {
    LOG_INFO("Demux opening URL: %s", url);
    m_lastReadTime = av_gettime_relative();
    m_fmtCtx = avformat_alloc_context();
    if (!m_fmtCtx) {
        LOG_ERROR("Demux open failed for URL: %s", url);
        return false;
    }

    AVIOInterruptCB intrCb = { &interruptCallback, this };
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
        LOG_ERROR("avformat_open_input failed: %s (code=%d), url: %s", errbuf, ret, url);
        return false;
    }
    LOG_INFO("avformat_open_input succeeded");

    m_fmtCtx->flags |= AVFMT_FLAG_NOBUFFER;
    m_fmtCtx->max_analyze_duration = 100000;

    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) {
        char errbuf[256] = {0};
        av_strerror(ret, errbuf, sizeof(errbuf));
        LOG_ERROR("avformat_find_stream_info failed: %s (code=%d)", errbuf, ret);
        return false;
    }

    int videoIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int audioIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (videoIdx >= 0) {
        m_videoStream   = m_fmtCtx->streams[videoIdx];
        m_videoCodecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(m_videoCodecPar, m_videoStream->codecpar);
        m_videoQueue->init(m_videoStream->time_base, 200);
        LOG_INFO("Video stream found: index=%d, codec=%d, %dx%d",
                 videoIdx, m_videoCodecPar->codec_id,
                 m_videoCodecPar->width, m_videoCodecPar->height);
    }

    if (audioIdx >= 0) {
        m_audioStream   = m_fmtCtx->streams[audioIdx];
        m_audioCodecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(m_audioCodecPar, m_audioStream->codecpar);
        m_audioQueue->init(m_audioStream->time_base, 200);
        LOG_INFO("Audio stream found: index=%d", audioIdx);
    }

    if (!m_videoStream) {
        LOG_ERROR("Demux open failed for URL: %s", url);
        return false;
    }

    LOG_INFO("Demux open complete, emitting streamInfoReady");
    emit streamInfoReady();
    return true;
}

void DemuxThread::stop() {
    m_abort = true;
    m_videoQueue->abort();
    m_audioQueue->abort();
}

void DemuxThread::run() {
    LOG_INFO("Demux thread running");
    m_abort = false;
    m_stateMachine->transition(PlayerState::Connecting, PlayerState::Playing);

    AVPacket* pkt = av_packet_alloc();

    while (!m_abort) {
        m_lastReadTime = av_gettime_relative();
        int ret = av_read_frame(m_fmtCtx, pkt);

        if (ret < 0) {
            if (ret == AVERROR_EOF) {
                LOG_INFO("av_read_frame returned EOF");
                break;
            }
            if (m_abort) break;
            char errbuf[256] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("av_read_frame error: %s (code=%d)", errbuf, ret);
            m_stateMachine->transition(PlayerState::Playing, PlayerState::Error);
            break;
        }

        if (pkt->stream_index == m_videoStream->index) {
            m_videoQueue->push(pkt);
        } else if (m_audioStream && pkt->stream_index == m_audioStream->index) {
            m_audioQueue->push(pkt);
        }

        av_packet_unref(pkt);
    }

    LOG_INFO("Demux thread exiting");

    av_packet_free(&pkt);
}

int DemuxThread::interruptCallback(void* opaque) {
    auto* self = static_cast<DemuxThread*>(opaque);
    if (self->m_abort) return 1;
    int64_t now = av_gettime_relative();
    if (now - self->m_lastReadTime > 3000000LL) return 1;
    return 0;
}

AVRational DemuxThread::videoTimeBase() const {
    return m_videoStream ? m_videoStream->time_base : AVRational{1, 90000};
}

AVRational DemuxThread::audioTimeBase() const {
    return m_audioStream ? m_audioStream->time_base : AVRational{1, 90000};
}

#include "AudioWorker.h"
#include "PacketQueue.h"
#include "AVClock.h"
#include "logger/Logger.h"

#include <QAudioOutput>
#include <QAudioFormat>
#include <QThread>

extern "C" {
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
}

#define TARGET_RATE      48000
#define TARGET_CHANNELS  2
#define TARGET_FORMAT    AV_SAMPLE_FMT_S16

AudioWorker::AudioWorker(AVCodecContext* codecCtx, AVRational timeBase,
                         PacketQueue* queue, AVClock* clock, QObject* parent)
    : QObject(parent)
    , m_codecCtx(codecCtx)
    , m_timeBase(timeBase.num > 0 ? timeBase : AVRational{1, 90000})
    , m_queue(queue)
    , m_clock(clock)
{
}

AudioWorker::~AudioWorker() {
    stop();
}

void AudioWorker::start() {
    m_running = true;

    LOG_INFO("Audio worker starting");

    int64_t layout = (m_codecCtx->channel_layout && m_codecCtx->channels ==
        av_get_channel_layout_nb_channels(m_codecCtx->channel_layout))
        ? m_codecCtx->channel_layout
        : static_cast<int64_t>(av_get_default_channel_layout(m_codecCtx->channels));

    m_swrCtx = swr_alloc_set_opts(nullptr,
        AV_CH_LAYOUT_STEREO, TARGET_FORMAT, TARGET_RATE,
        layout, m_codecCtx->sample_fmt, m_codecCtx->sample_rate,
        0, nullptr);
    if (!m_swrCtx || swr_init(m_swrCtx) < 0) {
        LOG_ERROR("Failed to initialize swresample: in=%dHz/%dch/%d out=%dHz/stereo/s16",
                  m_codecCtx->sample_rate, m_codecCtx->channels, m_codecCtx->sample_fmt, TARGET_RATE);
        if (m_swrCtx) swr_free(&m_swrCtx);
        m_running = false;
        return;
    }

    LOG_INFO("SwrContext: in=%dHz/%dch out=%dHz/stereo/s16",
             m_codecCtx->sample_rate, m_codecCtx->channels, TARGET_RATE);

    QAudioFormat format;
    format.setSampleRate(TARGET_RATE);
    format.setChannelCount(TARGET_CHANNELS);
    format.setSampleSize(16);
    format.setCodec("audio/pcm");
    format.setByteOrder(QAudioFormat::LittleEndian);
    format.setSampleType(QAudioFormat::SignedInt);

    QAudioDeviceInfo info = QAudioDeviceInfo::defaultOutputDevice();
    if (!info.isFormatSupported(format)) {
        LOG_ERROR("Audio format not supported");
        swr_free(&m_swrCtx);
        m_running = false;
        return;
    }

    QAudioOutput* audioOutput = new QAudioOutput(info, format, this);
    QIODevice* device = audioOutput->start();

    if (!device) {
        LOG_ERROR("Failed to start audio output");
        swr_free(&m_swrCtx);
        m_running = false;
        return;
    }

    LOG_INFO("Audio output started: %dHz/%dch/s16", TARGET_RATE, TARGET_CHANNELS);

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    int bytesPerSecond = TARGET_RATE * TARGET_CHANNELS * 2; // s16 = 2 bytes
    int timeoutCount = 0;

    while (m_running) {
        if (!m_queue->pop(pkt, 100)) {
            timeoutCount++;
            if (timeoutCount == 30) {
                LOG_INFO("Audio: queue empty, flushing decoder");
                avcodec_send_packet(m_codecCtx, nullptr);
                while (true) {
                    int ret = avcodec_receive_frame(m_codecCtx, frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) break;
                    if (ret < 0) break;
                }
                break;
            }
            continue;
        }
        timeoutCount = 0;

        int ret = avcodec_send_packet(m_codecCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        while (true) {
            ret = avcodec_receive_frame(m_codecCtx, frame);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) break;
            if (ret < 0) break;

            int dstSamples = swr_get_out_samples(m_swrCtx, frame->nb_samples);
            int dstBufSize = av_samples_get_buffer_size(nullptr, TARGET_CHANNELS,
                dstSamples, TARGET_FORMAT, 0);
            if (dstBufSize <= 0) continue;

            uint8_t* dstBuf = static_cast<uint8_t*>(av_malloc(dstBufSize));
            int converted = swr_convert(m_swrCtx, &dstBuf, dstSamples,
                const_cast<const uint8_t**>(frame->data), frame->nb_samples);
            if (converted <= 0) {
                av_free(dstBuf);
                continue;
            }

            int actualSize = converted * TARGET_CHANNELS * 2;

            // backpressure: wait if device buffer is full
            while (m_running && audioOutput->bytesFree() < actualSize) {
                QThread::msleep(5);
            }
            if (!m_running) { av_free(dstBuf); break; }

            device->write(reinterpret_cast<const char*>(dstBuf), actualSize);
            av_free(dstBuf);

            // audioClock = pts + buffered duration
            int64_t pts = frame->pts;
            if (pts == AV_NOPTS_VALUE) pts = frame->pkt_dts;
            if (pts != AV_NOPTS_VALUE) {
                double ptsSec = pts * av_q2d(m_timeBase);
                int bufferedBytes = audioOutput->bufferSize() - audioOutput->bytesFree();
                double bufferedSec = static_cast<double>(bufferedBytes) / bytesPerSecond;
                m_clock->setAudioClock(ptsSec + bufferedSec);
            }
        }
    }

    audioOutput->stop();
    swr_free(&m_swrCtx);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    LOG_INFO("Audio worker stopped");
}

void AudioWorker::stop() {
    m_running = false;
    m_queue->abort();
}

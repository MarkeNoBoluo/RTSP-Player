#include "AudioWorker.h"
#include "AudioRingBuffer.h"
#include "PacketQueue.h"
#include "AVClock.h"
#include "logger/Logger.h"

#include <cstring>

extern "C" {
#include <libavutil/time.h>
#include <libavutil/channel_layout.h>
}

AudioWorker::AudioWorker(AVCodecContext* codecCtx, AVRational timeBase,
                         PacketQueue* queue, AVClock* clock,
                         AudioRingBuffer* ringBuffer)
    : m_codecCtx(codecCtx)
    , m_timeBase(timeBase.num > 0 ? timeBase : AVRational{1, 90000})
    , m_queue(queue)
    , m_clock(clock)
    , m_ringBuffer(ringBuffer)
{
}

AudioWorker::~AudioWorker() {
    stop();
    join();
}

void AudioWorker::start() {
    m_thread = std::thread(&AudioWorker::run, this);
}

void AudioWorker::stop() {
    m_running = false;
    m_queue->abort();
    if (m_ringBuffer) {
        m_ringBuffer->abort();
    }
}

void AudioWorker::join() {
    if (m_thread.joinable()) {
        m_thread.join();
    }
}

void AudioWorker::writeWavHeader() {
    if (!m_wavFile) return;
    uint8_t header[44] = {0};
    int sampleRate = kTargetRate;
    int channels   = kTargetChannels;
    int bits       = 16;
    int byteRate   = sampleRate * channels * bits / 8;
    int blockAlign = channels * bits / 8;

    memcpy(header,     "RIFF", 4);
    memcpy(header + 8, "WAVE", 4);
    memcpy(header + 12, "fmt ", 4);
    header[16] = 16; header[17] = 0; header[18] = 0; header[19] = 0;
    header[20] = 1;  header[21] = 0;
    header[22] = channels & 0xFF;
    header[23] = (channels >> 8) & 0xFF;
    header[24] = sampleRate & 0xFF;
    header[25] = (sampleRate >> 8) & 0xFF;
    header[26] = (sampleRate >> 16) & 0xFF;
    header[27] = (sampleRate >> 24) & 0xFF;
    header[28] = byteRate & 0xFF;
    header[29] = (byteRate >> 8) & 0xFF;
    header[30] = (byteRate >> 16) & 0xFF;
    header[31] = (byteRate >> 24) & 0xFF;
    header[32] = blockAlign & 0xFF;
    header[33] = (blockAlign >> 8) & 0xFF;
    header[34] = bits & 0xFF;
    header[35] = (bits >> 8) & 0xFF;
    memcpy(header + 36, "data", 4);

    fwrite(header, 1, 44, m_wavFile);
}

void AudioWorker::updateWavHeader() {
    if (!m_wavFile) return;
    int fileSize = 36 + m_dataSize;
    fseek(m_wavFile, 4, SEEK_SET);
    fwrite(&fileSize, 4, 1, m_wavFile);
    fseek(m_wavFile, 40, SEEK_SET);
    fwrite(&m_dataSize, 4, 1, m_wavFile);
}

void AudioWorker::run() {
    m_running = true;

    LOG_INFO("Audio worker starting");

    int64_t layout = (m_codecCtx->channel_layout && m_codecCtx->channels ==
        av_get_channel_layout_nb_channels(m_codecCtx->channel_layout))
        ? m_codecCtx->channel_layout
        : static_cast<int64_t>(av_get_default_channel_layout(m_codecCtx->channels));

    m_swrCtx = swr_alloc_set_opts(nullptr,
        AV_CH_LAYOUT_STEREO, (AVSampleFormat)kTargetFormat, kTargetRate,
        layout, m_codecCtx->sample_fmt, m_codecCtx->sample_rate,
        0, nullptr);
    if (!m_swrCtx || swr_init(m_swrCtx) < 0) {
        LOG_ERROR("swr_init failed: in=%dHz/%dch/%d out=%dHz/stereo/s16",
                  m_codecCtx->sample_rate, m_codecCtx->channels, m_codecCtx->sample_fmt);
        if (m_swrCtx) swr_free(&m_swrCtx);
        m_running = false;
        return;
    }

    m_wavFile = fopen(kWavPath, "wb");
    if (m_wavFile) {
        writeWavHeader();
        LOG_INFO("WAV file opened for diagnostic: %s", kWavPath);
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    int timeoutCount = 0;
    bool hasAudio = false;
    int curSerial = m_serial.load(std::memory_order_acquire);

    while (m_running) {
        if (!m_queue->pop(pkt, 100)) {
            timeoutCount++;
            if (timeoutCount == 50 && !hasAudio) {
                LOG_INFO("Audio: no packets after 5s, exiting");
                break;
            }
            if (timeoutCount >= 300 && hasAudio) {
                LOG_INFO("Audio: queue empty 30s, flushing");
                avcodec_send_packet(m_codecCtx, nullptr);
                while (true) {
                    int flushRet = avcodec_receive_frame(m_codecCtx, frame);
                    if (flushRet == AVERROR(EAGAIN) || flushRet == AVERROR_EOF) break;
                    if (flushRet < 0) break;
                }
                break;
            }
            continue;
        }
        timeoutCount = 0;

        int ret = avcodec_send_packet(m_codecCtx, pkt);
        av_packet_unref(pkt);
        if (ret < 0) continue;

        curSerial = m_serial.load(std::memory_order_acquire);

        while (true) {
            ret = avcodec_receive_frame(m_codecCtx, frame);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) break;
            if (ret < 0) break;

            int dstSamples = swr_get_out_samples(m_swrCtx, frame->nb_samples);
            int dstBufSize = av_samples_get_buffer_size(nullptr, kTargetChannels,
                dstSamples, (AVSampleFormat)kTargetFormat, 0);
            if (dstBufSize <= 0) continue;

            uint8_t* dstBuf = static_cast<uint8_t*>(av_malloc(dstBufSize));
            int converted = swr_convert(m_swrCtx, &dstBuf, dstSamples,
                const_cast<const uint8_t**>(frame->data), frame->nb_samples);
            if (converted <= 0) {
                av_free(dstBuf);
                continue;
            }

            int actualSize = converted * kTargetChannels * 2;

            if (m_wavFile) {
                fwrite(dstBuf, 1, actualSize, m_wavFile);
                m_dataSize += actualSize;
            }

            int64_t pts = frame->pts;
            if (pts == AV_NOPTS_VALUE) pts = frame->pkt_dts;
            double ptsSec = (pts != AV_NOPTS_VALUE) ? pts * av_q2d(m_timeBase) : 0.0;

            if (m_ringBuffer) {
                m_ringBuffer->write(dstBuf, actualSize, ptsSec, curSerial);
            }

            if (!hasAudio) {
                hasAudio = true;
                LOG_INFO("Audio started: first %d bytes", actualSize);
            }

            av_free(dstBuf);
        }
    }

    if (m_wavFile) {
        updateWavHeader();
        fclose(m_wavFile);
        m_wavFile = nullptr;
        LOG_INFO("WAV file closed: %s (%d bytes PCM)", kWavPath, m_dataSize);
    }

    swr_free(&m_swrCtx);
    av_frame_free(&frame);
    av_packet_free(&pkt);

    LOG_INFO("Audio worker stopped (received=%d)", hasAudio);
}

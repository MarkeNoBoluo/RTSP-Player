#include "VideoDecodeThread.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/time.h>
}

VideoDecodeThread::VideoDecodeThread(PacketQueue* queue, VideoFrameQueue* frameQueue,
                                     PlayerStats* stats, QObject* parent)
    : QThread(parent)
    , m_queue(queue)
    , m_frameQueue(frameQueue)
    , m_stats(stats)
{
}

VideoDecodeThread::~VideoDecodeThread() {
    stop();
    wait();
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
}

bool VideoDecodeThread::open(AVCodecParameters* codecPar, AVRational timeBase) {
    if (!codecPar) return false;

    LOG_INFO("Video decoder opening, codec_id=%d, extradata_size=%d",
             codecPar->codec_id, codecPar->extradata_size);

    const AVCodec* codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) return false;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    avcodec_parameters_to_context(m_codecCtx, codecPar);
    m_codecCtx->thread_count = 2;
    m_timeBase = (timeBase.num > 0 && timeBase.den > 0) ? timeBase : AVRational{1, 90000};

    LOG_INFO("Decoder ctx: width=%d, height=%d, pix_fmt=%d, extradata_size=%d",
             m_codecCtx->width, m_codecCtx->height, m_codecCtx->pix_fmt,
             m_codecCtx->extradata_size);

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) return false;

    LOG_INFO("Video decoder opened successfully, thread_count=%d", m_codecCtx->thread_count);
    return true;
}

void VideoDecodeThread::stop() {
    m_abort = true;
    m_queue->abort();
}

void VideoDecodeThread::run() {
    m_abort = false;

    LOG_INFO("Video decode thread running");

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    int timeoutCount = 0;

    while (!m_abort) {
        if (!m_queue->pop(pkt, 100)) {
            timeoutCount++;
            if (timeoutCount <= 5) {
                LOG_DEBUG("VideoDecode: pop timeout #%d, queue_size=%d", timeoutCount, m_queue->size());
            }
            // If queue is empty for a while, flush decoder
            if (timeoutCount == 30) {
                LOG_INFO("VideoDecode: queue empty, flushing decoder");
                avcodec_send_packet(m_codecCtx, nullptr);
                while (true) {
                    int flushRet = avcodec_receive_frame(m_codecCtx, frame);
                    if (flushRet == AVERROR(EAGAIN) || flushRet == AVERROR_EOF) break;
                    if (flushRet < 0) break;
                    int64_t pts = frame->pts;
                    if (pts == AV_NOPTS_VALUE) pts = frame->pkt_dts;
                    if (pts != AV_NOPTS_VALUE) {
                        pts = av_rescale_q(pts, m_timeBase, AVRational{1, AV_TIME_BASE});
                    }
                    m_frameQueue->writeFrame(frame, pts);
                    m_stats->framesDecoded++;
                }
                break;  // Exit loop after flush
            }
            continue;
        }

        timeoutCount = 0;

        int ret = avcodec_send_packet(m_codecCtx, pkt);
        av_packet_unref(pkt);

        if (ret < 0) {
            char errbuf[256] = {0};
            av_strerror(ret, errbuf, sizeof(errbuf));
            LOG_ERROR("avcodec_send_packet error: %s (code=%d)", errbuf, ret);
            continue;
        }

        while (true) {
            ret = avcodec_receive_frame(m_codecCtx, frame);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) {
                LOG_INFO("VideoDecode: decoder EOF");
                break;
            }
            if (ret < 0) {
                char errbuf[256] = {0};
                av_strerror(ret, errbuf, sizeof(errbuf));
                LOG_ERROR("avcodec_receive_frame error: %s (code=%d)", errbuf, ret);
                break;
            }

            int64_t pts = frame->pts;
            if (pts == AV_NOPTS_VALUE) {
                pts = frame->pkt_dts;
            }
            if (pts != AV_NOPTS_VALUE) {
                pts = av_rescale_q(pts, m_timeBase, AVRational{1, AV_TIME_BASE});
            }

            if (m_stats->framesDecoded <= 1) {
                LOG_INFO("Decoded frame #%lld: pts=%lld, %dx%d, fmt=%d",
                         (long long)m_stats->framesDecoded.load(), (long long)pts,
                         frame->width, frame->height, frame->format);
            }

            m_frameQueue->writeFrame(frame, pts);
            m_stats->framesDecoded++;
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);

    LOG_INFO("Video decode thread exiting, total decoded: %lld", (long long)m_stats->framesDecoded.load());
}

#include "VideoDecodeThread.h"
#include "PlayerStats.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/time.h>
}

VideoDecodeThread::VideoDecodeThread(AVCodecContext* codecCtx, AVRational timeBase,
                                     PacketQueue* queue, VideoFrameQueue* frameQueue,
                                     PlayerStats* stats, QObject* parent)
    : QThread(parent)
    , m_codecCtx(codecCtx)
    , m_timeBase(timeBase.num > 0 ? timeBase : AVRational{1, 90000})
    , m_queue(queue)
    , m_frameQueue(frameQueue)
    , m_stats(stats)
{
}

VideoDecodeThread::~VideoDecodeThread() {
    stop();
    wait();
}

void VideoDecodeThread::stop() {
    m_abort = true;
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

            int64_t pts = frame->pts;
            if (pts == AV_NOPTS_VALUE) pts = frame->pkt_dts;
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

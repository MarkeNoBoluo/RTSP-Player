#pragma once

#include <QObject>
#include <atomic>

class PacketQueue;
class AVClock;

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavutil/rational.h>
#include <libswresample/swresample.h>
#ifdef __cplusplus
}
#endif

class AudioWorker : public QObject {
    Q_OBJECT
public:
    AudioWorker(AVCodecContext* codecCtx, AVRational timeBase,
                PacketQueue* queue, AVClock* clock, QObject* parent = nullptr);
    ~AudioWorker() override;

public slots:
    void start();
    void stop();

private:
    AVCodecContext*    m_codecCtx;
    AVRational         m_timeBase;
    PacketQueue*       m_queue;
    AVClock*           m_clock;

    SwrContext*        m_swrCtx     = nullptr;
    int                m_sampleRate = 48000;

    std::atomic<bool>  m_running{false};
};

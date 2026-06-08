#pragma once

#include <QObject>
#include <atomic>
#include <cstdio>

class PacketQueue;
class AVClock;
class AudioPullDevice;

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
                PacketQueue* queue, AVClock* clock,
                AudioPullDevice* device, QObject* parent = nullptr);
    ~AudioWorker() override;

public slots:
    void start();
    void stop();

private:
    void writeWavHeader();
    void updateWavHeader();

    AVCodecContext*    m_codecCtx;
    AVRational         m_timeBase;
    PacketQueue*       m_queue;
    AVClock*           m_clock;
    AudioPullDevice*   m_device;

    SwrContext*        m_swrCtx   = nullptr;
    FILE*              m_wavFile  = nullptr;
    int                m_dataSize = 0;

    std::atomic<bool>  m_running{false};
};

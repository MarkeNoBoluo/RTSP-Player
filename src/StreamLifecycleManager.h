#pragma once

#include "Common.h"
#include <atomic>
#include <functional>
#include <string>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#ifdef __cplusplus
}
#endif

class PlayerStateMachine;
class PacketQueue;
class VideoFrameQueue;
class AVClock;
class PlayerStats;
class DemuxThread;
class VideoDecodeThread;
class AudioWorker;
class AudioRingBuffer;
class SDLAudio;

class StreamLifecycleManager {
public:
    using StateCallback = std::function<void(PlayerState)>;
    using ErrorCallback = std::function<void(const char*)>;

    StreamLifecycleManager(PlayerStateMachine* sm, PlayerStats* stats,
                           PacketQueue* videoQ, PacketQueue* audioQ,
                           VideoFrameQueue* frameQ, AVClock* clock);
    ~StreamLifecycleManager();

    void setStateCallback(StateCallback cb) { m_onState = std::move(cb); }
    void setErrorCallback(ErrorCallback cb) { m_onError = std::move(cb); }

    bool open(const char* url);
    void close();

    void setTransport(const char* transport);
    void setAudioEnabled(bool v) { m_audioEnabled = v; }
    void setSetptsZero(bool v)   { m_setptsZero = v; }
    bool setptsZero() const      { return m_setptsZero; }

    PlayerStats* stats()    const { return m_stats; }
    PlayerState  state()    const;

    class AudioRingBuffer* audioRingBuffer() const { return m_audioRingBuffer; }

    int  pktSerial() const { return m_pktSerial.load(std::memory_order_acquire); }
    void doReconnect();

private:
    bool initDemux(const char* url);
    bool initDecoders();
    void startThreads();

    void shutdownPipeline();
    void scheduleReconnect();
    int  calcBackoffMs();

    void incrementSerial();

    PlayerStateMachine*  m_stateMachine;
    PlayerStats*         m_stats;
    PacketQueue*         m_videoQueue;
    PacketQueue*         m_audioQueue;
    VideoFrameQueue*     m_frameQueue;
    AVClock*             m_clock;

    DemuxThread*         m_demuxThread     = nullptr;
    VideoDecodeThread*   m_decodeThread    = nullptr;
    AudioWorker*         m_audioWorker     = nullptr;
    AudioRingBuffer*     m_audioRingBuffer = nullptr;
    SDLAudio*            m_sdlAudio        = nullptr;

    AVFormatContext*     m_fmtCtx          = nullptr;
    AVStream*            m_videoStream     = nullptr;
    AVStream*            m_audioStream     = nullptr;
    AVCodecParameters*   m_videoCodecPar   = nullptr;
    AVCodecParameters*   m_audioCodecPar   = nullptr;
    AVCodecContext*      m_videoCodecCtx   = nullptr;
    AVCodecContext*      m_audioCodecCtx   = nullptr;

    std::atomic<int>     m_pktSerial{0};

    int                  m_reconnectTimerId = 0;
    int                  m_backoffCount     = 0;
    int64_t              m_videoQueueCapacityMs = 200;
    int64_t              m_lowLatencyVideoQueueCapacityMs = 33;
    bool                 m_audioEnabled     = true;
    bool                 m_setptsZero       = false;

    std::atomic<uint64_t> m_generation{1};
    std::string           m_url;
    std::string           m_transport{"udp"};

    StateCallback m_onState;
    ErrorCallback m_onError;
};

#pragma once

#include <QThread>
#include <atomic>

class VideoFrameQueue;
class AVClock;
class GLVideoWidget;
class PlayerStats;

class RenderScheduler : public QThread {
    Q_OBJECT
public:
    RenderScheduler(VideoFrameQueue* frameQueue, AVClock* clock,
                    GLVideoWidget* widget, PlayerStats* stats,
                    QObject* parent = nullptr);
    ~RenderScheduler() override;

    void stop();
    void setFrameDuration(double frameDurationUs);

protected:
    void run() override;

private:
    bool shouldDrop(int64_t latenessUs) const;

    VideoFrameQueue*  m_frameQueue;
    AVClock*          m_clock;
    GLVideoWidget*    m_widget;
    PlayerStats*      m_stats;

    std::atomic<bool> m_running{false};
    double            m_frameDurationUs = 0.0;
    int               m_consecutiveDrops = 0;
};

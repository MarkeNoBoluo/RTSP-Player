# RTSP-Player 设计文档

## 1. 概述

**目标**: 低延迟 RTSP 拉流播放器，用于投影场景。  
**平台**: Windows, Qt 5.14.2, FFmpeg C API, CMake  
**核心诉求**: 秒开、低延迟、实时交互优先、视频链路稳定

---

## 2. 线程模型（6 线程）

```
UI Thread ── RTSPlayer, GLVideoWidget::paintGL()
              │ invokeMethod(update)
              │
Render Scheduler Thread ── AV Sync / Frame Timing / Drop / invokeMethod(update)
              │ FrameQueue (triple buffer)
              │
Video Decode Thread ── 解码 AVPacket → YUV420P AVFrame
              │ PacketQueue v (200ms)
              │
Demux Thread ── av_read_frame() → 分发
              │ PacketQueue a (200ms)
              │                              ┌─ Phase 4 ─┐
Audio Thread ── decode + resample + QAudioOutput
```

**关键约束**：
- Render Scheduler **不做 OpenGL**，仅通过 `invokeMethod(update)` 通知 UI Thread
- UI Thread 的 `paintGL()` 做实际 OpenGL 渲染
- `updatePending` flag 防 `update()` 风暴

---

## 3. 模块清单

| 模块 | 线程 | 职责 |
|------|------|------|
| `RTSPlayer` | UI | 总控 |
| `PlayerStateMachine` | 任意 | 6 状态机 |
| `StreamLifecycleManager` | 任意 | open/reconnect/flush/close 全生命周期 |
| `DemuxThread` | 独立 | `av_read_frame()` → PacketQueue |
| `VideoDecodeThread` | 独立 | 解码 → VideoFrameQueue |
| `RenderScheduler` | 独立 | 帧 timing / drop / `invokeMethod(update)` |
| `AudioThread` | 独立 | Phase 4 实现 |
| `PacketQueue` | 跨线程 | 环形缓冲，KeyFrame 感知 Drop，容量 200ms |
| `VideoFrameQueue` | 跨线程 | Triple Buffer + 固定 AVFrame 池 + `av_frame_move_ref` |
| `AVClock` | 跨线程 | VideoClock (pts + systemTime)，Phase 5 加入 audioClock |
| `GLVideoWidget` | UI | QOpenGLWidget，paintGL() YUV Shader |
| `PlayerStats` | 跨线程 | 性能统计（fps / drop / latency / queue） |

---

## 4. 关键接口

### 4.1 PlayerStateMachine (6 状态)

```
Stopped → Connecting → Playing → Reconnecting → Playing
              ↓            ↓           ↓
             Error        Error       Error
              │            │           │
    重连预算耗尽:       重连预算>0:    重连预算>0:
              │            │           │
         → Stopped    → Reconnecting → Reconnecting
                     重连预算耗尽:    重连预算耗尽:
                          │               │
                      → Stopped       → Stopped
                                      Closing → Stopped
```

```cpp
enum class State { Stopped, Connecting, Playing, Reconnecting, Error, Closing };
// std::atomic<State>, 所有切换通过 compare_exchange
// Error 出边: Reconnecting（重连预算 > 0, StreamLifecycleManager 触发）
//            Stopped（重连预算耗尽 或 用户手动 stop）
```

### 4.2 VideoFrameQueue（Triple Buffer + 固定池 + 无锁设计）

三个独立 slot: 0=display, 1=render(待显示), 2=decode(写入中)。

```cpp
class VideoFrameQueue {
public:
    // VideoDecodeThread 调用
    // 内部 av_frame_move_ref() 到 decode slot, 然后 atomic swap decode↔render
    bool writeFrame(AVFrame* srcFrame, int64_t pts);

    // --- RenderScheduler 线程调用 (独占) ---
    void    waitForNewFrame(int timeoutMs);   // cv wait, timeout 10ms
    int64_t peekRenderPts() const;           // 只读 render slot pts; 无新帧返回 -1
    void    discardRender();                 // no-op, render slot 由下次 writeFrame 自然覆盖
    bool    commitDisplay();                 // swap render↔display; 有帧返回 true

    // UI Thread (paintGL) 调用
    const VideoFrame* displayFrame() const;   // 只读 display slot, 无锁

    // StreamLifecycleManager 调用 (停机顺序保证后)
    void flush();
    void notifyAll();                        // cv notify_all, RenderScheduler stop 时解阻塞

private:
    AVFrame*          m_avFrames[3];           // 永久 alloc, 统一生命周期
    VideoFrame        m_slots[3];              // { pts, presentTime }
    std::atomic<int>  m_renderIdx{1};          // render 槽索引
    int               m_displayIdx{0};         // 仅 UI 线程读 + RenderScheduler 线程写
    int               m_decodeIdx{2};          // 仅 Decode 线程写
    std::condition_variable m_newFrameCv;
    std::mutex              m_cvMutex;
};
```

**接口实现细节**：

```cpp
// writeFrame: 写入 decodeIdx slot → atomic swap decode↔render → notify cv
bool VideoFrameQueue::writeFrame(AVFrame* src, int64_t pts) {
    av_frame_move_ref(m_avFrames[m_decodeIdx], src);
    m_slots[m_decodeIdx].pts = pts;
    m_decodeIdx = m_renderIdx.exchange(m_decodeIdx, std::memory_order_acq_rel);
    m_newFrameCv.notify_one();
    return true;
}

// peekRenderPts: 只读 render slot pts, 无 swap (m_displayIdx 是普通 int, RenderScheduler 线程独占访问)
int64_t VideoFrameQueue::peekRenderPts() const {
    int ri = m_renderIdx.load(std::memory_order_acquire);
    if (ri == m_displayIdx) return -1;
    return m_slots[ri].pts;
}

// discardRender: 空实现; render slot 由下一次 writeFrame 的 decode↔render swap 自然覆盖
void VideoFrameQueue::discardRender() {}

// commitDisplay: swap render↔display
bool VideoFrameQueue::commitDisplay() {
    int ri = m_renderIdx.load(std::memory_order_acquire);
    if (ri == m_displayIdx) return false;
    m_renderIdx.store(m_displayIdx, std::memory_order_release);
    m_displayIdx = ri;
    return true;
}

// displayFrame: 仅 UI 线程调用, 只读
const VideoFrame* VideoFrameQueue::displayFrame() const {
    return &m_slots[m_displayIdx];
}

// flush: 依赖停机顺序保证安全 (无写入者 + 无 swap 调用者)
void VideoFrameQueue::flush() {
    for (int i = 0; i < 3; i++) av_frame_unref(m_avFrames[i]);
    m_renderIdx.store(1, std::memory_order_relaxed);
    m_displayIdx = 0;
    m_decodeIdx  = 2;
}

// notifyAll: RenderScheduler::stop() 时调用, 解阻塞 waitForNewFrame
void VideoFrameQueue::notifyAll() {
    m_newFrameCv.notify_all();
}
```

### 4.3 PacketQueue（KeyFrame 感知 Drop）

```cpp
class PacketQueue {
public:
    PacketQueue();  // 构造函数不设容量

    // avformat_open_input 成功后调用，此时 time_base 已知
    // reconnect 时复用 flush()，不重调 init()
    void init(AVRational timeBase, int capacityMs = 200);

    bool push(AVPacket* pkt);   // 超容量: 优先 drop P/B, 保 IDR (AV_PKT_FLAG_KEY)
    bool pop(AVPacket* pkt, int timeoutMs);
    void flush();
    void abort();               // 解除 pop 阻塞 (stop 时调用)

private:
    bool m_initialized = false;   // assert(!m_initialized) 防止重复 init
    // 环形缓冲 + condition_variable + mutex
    // 容量 200ms (按流 time_base 换算)
};
```

### 4.4 RenderScheduler（实时优先，condition_variable 等待）

```cpp
class RenderScheduler {
public:
    void run();                  // 主循环: cv 等待 → peek → drop? → commit → invokeMethod
    void stop();
    void setDropThreshold(int ms = 30);

private:
    bool shouldDrop(int64_t pts) const;   // late > 30ms, 首帧保护返回 false

    VideoFrameQueue*  m_frameQueue;
    AVClock*          m_clock;
    GLVideoWidget*    m_widget;
    std::atomic_bool  m_updatePending{false};
    std::atomic_bool  m_running{false};
};
```

**主循环实现**：

```cpp
void RenderScheduler::run() {
    while (m_running) {
        m_frameQueue->waitForNewFrame(10ms);
        if (!m_running) break;

        int64_t pts = m_frameQueue->peekRenderPts();
        if (pts < 0) continue;           // 无新帧

        if (shouldDrop(pts)) {
            m_frameQueue->discardRender(); // no-op
            m_stats->droppedFrames++;
            continue;
        }

        m_frameQueue->commitDisplay();    // swap render↔display
        m_clock->setVideoClock(pts);      // 更新 VideoClock (present time)

        if (!m_updatePending.exchange(true)) {
            QMetaObject::invokeMethod(m_widget, "update", Qt::QueuedConnection);
        }
    }
}

bool RenderScheduler::shouldDrop(int64_t pts) const {
    if (!m_clock->isReady()) return false;   // 首帧保护
    int64_t now = av_gettime_relative();
    int64_t expected = m_clock->videoClock().systemTime +
                       (pts - m_clock->videoClock().pts) / av_q2d(m_timeBase) * AV_TIME_BASE;
    return (expected - now) < -m_dropThreshold * 1000;   // late > 30ms
}
```

### 4.5 AVClock

```cpp
struct ClockPoint { double pts; int64_t systemTime; };  // systemTime 单位: us

class AVClock {
public:
    void       setVideoClock(double pts);    // 第一次调用后 isReady → true
    ClockPoint videoClock() const;
    bool       isReady() const;              // 首帧保护
    double     drift() const;                // Phase 5 实现

    void       setAudioClock(double pts);    // Phase 4
    ClockPoint audioClock() const;           // Phase 4

private:
    std::atomic<double>  m_videoPts{0.0};
    std::atomic<int64_t> m_videoSysTime{0};
    std::atomic<double>  m_audioPts{0.0};
    std::atomic<int64_t> m_audioSysTime{0};
    std::atomic<bool>    m_videoReady{false};
};
```

### 4.6 GLVideoWidget

```cpp
class GLVideoWidget : public QOpenGLWidget, protected QOpenGLFunctions {
public:
    void setDisplayFrame(const VideoFrame* frame);  // 仅在 paintGL 读取前设置, atomic pointer

protected:
    void initializeGL() override;      // 3×GL_LUMINANCE texture + shader + GL_UNPACK_ALIGNMENT=1
    void paintGL() override;           // glTexSubImage2D + shader, m_updatePending=false
    void resizeGL(int w, int h) override;

private:
    GLuint m_textures[3];              // Y, U, V plane
    QOpenGLShaderProgram* m_program;
    const VideoFrame*     m_displayFrame;
};
```

**GL 格式**：YUV420P planar + 3×GL_LUMINANCE，不做格式转换。

```cpp
void GLVideoWidget::initializeGL() {
    initializeOpenGLFunctions();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);   // 一次设置, 全局生效

    // 创建 shader program (yuv420p.vert / yuv420p.frag)

    glGenTextures(3, m_textures);
    // Y: GL_LUMINANCE, width × height
    // U: GL_LUMINANCE, width/2 × height/2
    // V: GL_LUMINANCE, width/2 × height/2
    // 每次分辨率变化时 resizeGL 重新 glTexImage2D
}

void GLVideoWidget::paintGL() {
    m_updatePending = false;  // 允许下一次 invokeMethod
    auto* frame = m_displayFrame;
    if (!frame || !frame->frame) return;

    // glTexSubImage2D 上传 Y/U/V 三平面数据 (对齐已设, 不复用 glTexImage2D)
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, m_textures[i]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0,
                        i == 0 ? m_width : m_width/2, i == 0 ? m_height : m_height/2,
                        GL_LUMINANCE, GL_UNSIGNED_BYTE, frame->frame->data[i]);
    }

    m_program->bind();
    // draw quad
}
```

### 4.7 StreamLifecycleManager

```cpp
class StreamLifecycleManager {
public:
    bool open(const char* url);
    bool reconnect();
    void flush();           // 内部顺序: 停机 → flush → close → 重建
    void close();
    void setTimeout(int ms = 3000);
    void setRetryCount(int n);

private:
    int m_retryCount = 3;
    int m_retryBudget;       // reconnect 递减, 耗尽切 Stopped
};
```

**Flush / 重连停机顺序**：

```
1. DemuxThread::stop()
   └── PacketQueue(video)::abort()  // 让 VideoDecodeThread::pop() 解阻塞
   └── PacketQueue(audio)::abort()  // 让 AudioThread::pop() 解阻塞
2. VideoDecodeThread::stop() + wait()
3. AudioThread::stop() + wait()      // Phase 4
4. RenderScheduler::stop()
   └── VideoFrameQueue::notifyAll()  // 让 waitForNewFrame() 解阻塞
5. PacketQueue(video)::flush()
6. PacketQueue(audio)::flush()
7. avcodec_flush_buffers(m_codecCtx) // 必须在 avformat_close_input 之前
8. avformat_close_input(&m_fmtCtx)
9. avcodec_free_context(&m_codecCtx) // 独立管理 codec context
10. VideoFrameQueue::flush()
11. AVClock 重置
12. avformat_open_input(...)         // 重建
13. avcodec_open2(...)               // 重建 codec
14. State → Playing
```

**interrupt_callback**: 超时 3s，检查 `std::atomic<bool> m_abort` + chrono `lastReadTime`。

### 4.8 PlayerStats

```cpp
class PlayerStats {
    std::atomic<double> decodeFps{0};
    std::atomic<double> renderFps{0};
    std::atomic<int>    queueDurationMs{0};
    std::atomic<int>    droppedFrames{0};
    std::atomic<int>    reconnectCount{0};
    std::atomic<int>    latencyEstimateMs{0};
};
```

### 4.9 Common.h

```cpp
#pragma once
#include <cstdint>

enum class PlayerState { Stopped, Connecting, Playing, Reconnecting, Error, Closing };

struct VideoFrame {
    AVFrame* frame = nullptr;    // 指向 VideoFrameQueue 固定池, 外部不管理生命周期
    int64_t  pts = AV_NOPTS_VALUE;
    int64_t  presentTime = 0;
};

struct ClockPoint {
    double  pts;           // PTS 值 (秒)
    int64_t systemTime;    // 对应系统时间 (us, av_gettime_relative)
};
```

---

## 5. 关键工程细节

### 泄漏控制
- 所有 AVFrame 由 VideoFrameQueue 固定池统一管理生命周期
- `av_frame_move_ref` 转移引用，零拷贝
- `av_packet_unref` 在 PacketQueue::pop 消费后调用
- codec context 由 StreamLifecycleManager 独立管理（`avcodec_open2` / `avcodec_free_context`），不跟随 `avformat`

### Back Pressure
- PacketQueue push 满 → drop oldest (KeyFrame 优先保留：优先丢 P/B frame，保 IDR)
- 不做生产者阻塞等待消费者

### Update 防风暴
- `std::atomic_bool m_updatePending` 保证同一时刻只有一个 update 请求在 Qt EventLoop
- `paintGL` 结束时 `m_updatePending = false`

### GL_UNPACK_ALIGNMENT
- `initializeGL` 开头 `glPixelStorei(GL_UNPACK_ALIGNMENT, 1)` 一次设置
- `paintGL` / `resizeGL` 无需重复设置（context 级别全局状态）

### AVClock 首帧保护
- `isReady()` 标志，`setVideoClock` 第一次调用后置 true
- `RenderScheduler::shouldDrop` 在 `!isReady()` 时直接返回 false

### 秒开策略
- `avformat_open_input` 设置低 `probesize` / `analyzeduration` (~32KB)
- PacketQueue 容量仅 200ms，不强缓冲
- 首帧到达立即 invokeMethod 调度渲染

---

## 6. 目录结构

```
RTSP-Player/
├── CMakeLists.txt
├── src/
│   ├── main.cpp
│   ├── RTSPlayer.h / .cpp
│   ├── PlayerStateMachine.h / .cpp
│   ├── StreamLifecycleManager.h / .cpp
│   ├── DemuxThread.h / .cpp
│   ├── VideoDecodeThread.h / .cpp
│   ├── RenderScheduler.h / .cpp
│   ├── AudioThread.h / .cpp
│   ├── PacketQueue.h / .cpp
│   ├── VideoFrameQueue.h / .cpp
│   ├── AVClock.h / .cpp
│   ├── GLVideoWidget.h / .cpp
│   ├── PlayerStats.h / .cpp
│   └── Common.h
├── resources/
│   └── shaders/
│       ├── yuv420p.vert
│       └── yuv420p.frag
```

---

## 7. Phase 实现计划

| Phase | 内容 | 模块 | 验收标准 |
|-------|------|------|----------|
| **1** | Video 秒开链路 | RTSPlayer, StateMachine, DemuxThread, VideoDecodeThread, RenderScheduler, PacketQueue, VideoFrameQueue, AVClock(基础), GLVideoWidget, PlayerStats, CMakeLists.txt | RTSP→OpenGL 稳定播放、秒开、不花屏、无泄漏 |
| **2** | Frame Drop | RenderScheduler::shouldDrop() + AVClock::isReady() | late>30ms 帧被丢弃，延迟不累积 |
| **3** | Reconnect | StreamLifecycleManager 完整 | 断线自动重连，Flush 后恢复播放 |
| **4** | Audio | AudioThread, QAudioOutput, AVClock::setAudioClock | 解码播放不崩溃，集成 audioClock（不验证同步） |
| **5** | AV Sync | AVClock::drift(), AVClock::masterClock(), audio resample | 音视频同步（VideoClock Master） |

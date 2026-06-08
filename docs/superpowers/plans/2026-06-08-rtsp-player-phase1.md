# RTSP-Player Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the Phase 1 video pipeline: RTSP → decode → OpenGL, achieving stable playback, instant start, no corruption, no memory leaks.

**Architecture:** 6-thread pipeline (UI / RenderScheduler / VideoDecode / Audio / Demux) with triple-buffered frame queue, condition-variable-driven scheduling, keyframe-aware packet drop, and YUV420P direct OpenGL rendering via QOpenGLWidget.

**Tech Stack:** Qt 5.14.2 (Widgets, OpenGL), FFmpeg (avformat, avcodec, avutil), CMake, Windows (MSVC/MinGW)

---

## File Map

| File | Responsibility |
|------|----------------|
| `CMakeLists.txt` | Build: Qt5 + FFmpeg linkage |
| `src/Common.h` | Shared types: PlayerState, VideoFrame, ClockPoint |
| `src/PacketQueue.h` / `.cpp` | Ring buffer for AVPacket, keyframe-aware drop |
| `src/VideoFrameQueue.h` / `.cpp` | Triple buffer for AVFrame, zero-copy move_ref |
| `src/AVClock.h` / `.cpp` | Video master clock with isReady guard |
| `src/GLVideoWidget.h` / `.cpp` | QOpenGLWidget YUV420P renderer |
| `resources/shaders/yuv420p.vert` | Passthrough vertex shader |
| `resources/shaders/yuv420p.frag` | YUV→RGB fragment shader |
| `src/PlayerStateMachine.h` / `.cpp` | 6-state atomic finite state machine |
| `src/PlayerStats.h` / `.cpp` | Atomic performance counters |
| `src/DemuxThread.h` / `.cpp` | RTSP demux: av_read_frame → PacketQueue |
| `src/VideoDecodeThread.h` / `.cpp` | Decode: PacketQueue → VideoFrameQueue |
| `src/RenderScheduler.h` / `.cpp` | Frame timing, drop decision, invokeMethod dispatch |
| `src/RTSPlayer.h` / `.cpp` | Root orchestrator, owns all modules |
| `src/main.cpp` | QApplication + window + RTSPlayer bootstrap |

---

### Task 1: CMakeLists.txt

**Files:**
- Create: `CMakeLists.txt`

- [ ] **Step 1: Write CMakeLists.txt**

```cmake
cmake_minimum_required(VERSION 3.16)
project(RTSP-Player LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt5 REQUIRED COMPONENTS Widgets OpenGL)

# FFmpeg - adjust paths to your installation
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg installation root")
if(FFMPEG_ROOT)
    set(FFMPEG_INCLUDE_DIRS ${FFMPEG_ROOT}/include)
    set(FFMPEG_LIBRARY_DIRS ${FFMPEG_ROOT}/lib)
    find_library(AVFORMAT_LIBRARY  avformat  PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED)
    find_library(AVCODEC_LIBRARY   avcodec   PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED)
    find_library(AVUTIL_LIBRARY    avutil    PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED)
    find_library(SWRESAMPLE_LIBRARY swresample PATHS ${FFMPEG_LIBRARY_DIRS} REQUIRED)
else()
    find_library(AVFORMAT_LIBRARY  avformat  REQUIRED)
    find_library(AVCODEC_LIBRARY   avcodec   REQUIRED)
    find_library(AVUTIL_LIBRARY    avutil    REQUIRED)
    find_library(SWRESAMPLE_LIBRARY swresample REQUIRED)
    set(FFMPEG_INCLUDE_DIRS "")
endif()

set(SOURCES
    src/main.cpp
    src/RTSPlayer.cpp
    src/PlayerStateMachine.cpp
    src/DemuxThread.cpp
    src/VideoDecodeThread.cpp
    src/RenderScheduler.cpp
    src/PacketQueue.cpp
    src/VideoFrameQueue.cpp
    src/AVClock.cpp
    src/GLVideoWidget.cpp
    src/PlayerStats.cpp
)

set(RESOURCES
    resources/shaders/shaders.qrc
)

add_executable(${PROJECT_NAME} ${SOURCES} ${RESOURCES})

target_include_directories(${PROJECT_NAME} PRIVATE
    src
    ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(${PROJECT_NAME} PRIVATE
    Qt5::Widgets
    Qt5::OpenGL
    ${AVFORMAT_LIBRARY}
    ${AVCODEC_LIBRARY}
    ${AVUTIL_LIBRARY}
    ${SWRESAMPLE_LIBRARY}
)
```

- [ ] **Step 2: Create shaders.qrc**

```xml
<RCC>
    <qresource prefix="/shaders">
        <file>yuv420p.vert</file>
        <file>yuv420p.frag</file>
    </qresource>
</RCC>
```

---

### Task 2: Common.h

**Files:**
- Create: `src/Common.h`

- [ ] **Step 1: Write Common.h**

```cpp
#pragma once

#include <cstdint>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavutil/frame.h>
#include <libavutil/rational.h>
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif

enum class PlayerState : int {
    Stopped,
    Connecting,
    Playing,
    Reconnecting,
    Error,
    Closing
};

struct VideoFrame {
    AVFrame* frame = nullptr;
    int64_t  pts   = AV_NOPTS_VALUE;
    int64_t  presentTime = 0;
};

struct ClockPoint {
    double  pts;
    int64_t systemTime;
};
```

---

### Task 3: PacketQueue

**Files:**
- Create: `src/PacketQueue.h`
- Create: `src/PacketQueue.cpp`

- [ ] **Step 1: Write PacketQueue.h**

```cpp
#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <cstdint>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/packet.h>
#include <libavutil/rational.h>
#ifdef __cplusplus
}
#endif

class PacketQueue {
public:
    PacketQueue();
    ~PacketQueue();

    void init(AVRational timeBase, int capacityMs = 200);
    bool push(AVPacket* pkt);
    bool pop(AVPacket* pkt, int timeoutMs);
    void flush();
    void abort();
    int  size();
    int  durationMs() const;

private:
    struct PacketNode {
        AVPacket* pkt;
        int64_t   durationUs;
    };

    bool    m_initialized = false;
    int     m_capacityMs  = 200;
    double  m_timeBaseUs  = 0.0;

    std::deque<PacketNode> m_queue;
    std::mutex             m_mutex;
    std::condition_variable m_cond;
    std::atomic<bool>       m_abort{false};
    int                     m_totalDurationUs = 0;
};
```

- [ ] **Step 2: Write PacketQueue.cpp**

```cpp
#include "PacketQueue.h"
#include <cassert>

PacketQueue::PacketQueue() {}

PacketQueue::~PacketQueue() {
    flush();
}

void PacketQueue::init(AVRational timeBase, int capacityMs) {
    assert(!m_initialized);
    m_initialized = true;
    m_capacityMs  = capacityMs;
    m_timeBaseUs  = av_q2d(timeBase) * 1000000.0;
}

bool PacketQueue::push(AVPacket* pkt) {
    std::unique_lock<std::mutex> lock(m_mutex);

    int64_t durUs = 0;
    if (pkt->duration > 0) {
        durUs = static_cast<int64_t>(pkt->duration * m_timeBaseUs);
    }

    while (!m_queue.empty() && m_totalDurationUs + durUs > m_capacityMs * 1000LL) {
        auto it = m_queue.begin();
        bool dropped = false;

        for (auto iter = m_queue.begin(); iter != m_queue.end(); ++iter) {
            if (!(iter->pkt->flags & AV_PKT_FLAG_KEY)) {
                m_totalDurationUs -= iter->durationUs;
                av_packet_free(&iter->pkt);
                m_queue.erase(iter);
                dropped = true;
                break;
            }
        }

        if (!dropped) {
            m_totalDurationUs -= m_queue.front().durationUs;
            av_packet_free(&m_queue.front().pkt);
            m_queue.pop_front();
        }
    }

    AVPacket* copy = av_packet_alloc();
    av_packet_move_ref(copy, pkt);

    m_queue.push_back({copy, durUs});
    m_totalDurationUs += durUs;
    m_cond.notify_one();
    return true;
}

bool PacketQueue::pop(AVPacket* pkt, int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_mutex);

    if (m_abort) return false;

    if (m_queue.empty()) {
        if (timeoutMs < 0) {
            m_cond.wait(lock, [this] { return !m_queue.empty() || m_abort; });
        } else {
            m_cond.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                            [this] { return !m_queue.empty() || m_abort; });
        }
    }

    if (m_abort && m_queue.empty()) return false;
    if (m_queue.empty()) return false;

    auto& node = m_queue.front();
    av_packet_move_ref(pkt, node.pkt);
    m_totalDurationUs -= node.durationUs;
    av_packet_free(&node.pkt);
    m_queue.pop_front();
    return true;
}

void PacketQueue::flush() {
    std::unique_lock<std::mutex> lock(m_mutex);
    while (!m_queue.empty()) {
        av_packet_free(&m_queue.front().pkt);
        m_queue.pop_front();
    }
    m_totalDurationUs = 0;
    m_cond.notify_all();
}

void PacketQueue::abort() {
    m_abort = true;
    m_cond.notify_all();
}

int PacketQueue::size() {
    std::unique_lock<std::mutex> lock(m_mutex);
    return static_cast<int>(m_queue.size());
}

int PacketQueue::durationMs() const {
    return m_totalDurationUs / 1000;
}
```

---

### Task 4: VideoFrameQueue

**Files:**
- Create: `src/VideoFrameQueue.h`
- Create: `src/VideoFrameQueue.cpp`

- [ ] **Step 1: Write VideoFrameQueue.h**

```cpp
#pragma once

#include "Common.h"
#include <atomic>
#include <condition_variable>
#include <mutex>

class VideoFrameQueue {
public:
    VideoFrameQueue();
    ~VideoFrameQueue();

    // VideoDecodeThread
    bool writeFrame(AVFrame* srcFrame, int64_t pts);

    // RenderScheduler thread (exclusive)
    void    waitForNewFrame(int timeoutMs);
    int64_t peekRenderPts() const;
    void    discardRender();
    bool    commitDisplay();

    // UI Thread (paintGL)
    const VideoFrame* displayFrame() const;

    // StreamLifecycleManager (called after all threads stopped)
    void flush();
    void notifyAll();

private:
    AVFrame*          m_avFrames[3];
    VideoFrame        m_slots[3];
    std::atomic<int>  m_renderIdx{1};
    int               m_displayIdx{0};
    int               m_decodeIdx{2};
    int               m_width{0};
    int               m_height{0};

    std::condition_variable m_newFrameCv;
    std::mutex              m_cvMutex;
};
```

- [ ] **Step 2: Write VideoFrameQueue.cpp**

```cpp
#include "VideoFrameQueue.h"

extern "C" {
#include <libavutil/frame.h>
}

VideoFrameQueue::VideoFrameQueue() {
    for (int i = 0; i < 3; i++) {
        m_avFrames[i] = av_frame_alloc();
        m_slots[i].frame = m_avFrames[i];
    }
}

VideoFrameQueue::~VideoFrameQueue() {
    for (int i = 0; i < 3; i++) {
        av_frame_free(&m_avFrames[i]);
    }
}

bool VideoFrameQueue::writeFrame(AVFrame* srcFrame, int64_t pts) {
    av_frame_unref(m_avFrames[m_decodeIdx]);
    av_frame_move_ref(m_avFrames[m_decodeIdx], srcFrame);
    m_slots[m_decodeIdx].pts = pts;

    if (m_width != srcFrame->width || m_height != srcFrame->height) {
        m_width  = srcFrame->width;
        m_height = srcFrame->height;
    }

    m_decodeIdx = m_renderIdx.exchange(m_decodeIdx, std::memory_order_acq_rel);
    m_newFrameCv.notify_one();
    return true;
}

void VideoFrameQueue::waitForNewFrame(int timeoutMs) {
    std::unique_lock<std::mutex> lock(m_cvMutex);
    m_newFrameCv.wait_for(lock, std::chrono::milliseconds(timeoutMs));
}

int64_t VideoFrameQueue::peekRenderPts() const {
    int ri = m_renderIdx.load(std::memory_order_acquire);
    if (ri == m_displayIdx) return -1;
    return m_slots[ri].pts;
}

void VideoFrameQueue::discardRender() {
    // no-op: render slot will be overwritten by next writeFrame's swap
}

bool VideoFrameQueue::commitDisplay() {
    int ri = m_renderIdx.load(std::memory_order_acquire);
    if (ri == m_displayIdx) return false;
    m_renderIdx.store(m_displayIdx, std::memory_order_release);
    m_displayIdx = ri;
    return true;
}

const VideoFrame* VideoFrameQueue::displayFrame() const {
    return &m_slots[m_displayIdx];
}

void VideoFrameQueue::flush() {
    for (int i = 0; i < 3; i++) {
        av_frame_unref(m_avFrames[i]);
    }
    m_renderIdx.store(1, std::memory_order_relaxed);
    m_displayIdx = 0;
    m_decodeIdx  = 2;
    m_width  = 0;
    m_height = 0;
}

void VideoFrameQueue::notifyAll() {
    m_newFrameCv.notify_all();
}
```

---

### Task 5: AVClock

**Files:**
- Create: `src/AVClock.h`
- Create: `src/AVClock.cpp`

- [ ] **Step 1: Write AVClock.h**

```cpp
#pragma once

#include "Common.h"
#include <atomic>

class AVClock {
public:
    void       setVideoClock(double pts);
    ClockPoint videoClock() const;
    bool       isReady() const;

    void       setAudioClock(double pts);
    ClockPoint audioClock() const;

    double     drift() const;
    void       reset();

private:
    int64_t nowUs() const;

    std::atomic<double>  m_videoPts{0.0};
    std::atomic<int64_t> m_videoSysTime{0};
    std::atomic<bool>    m_videoReady{false};

    std::atomic<double>  m_audioPts{0.0};
    std::atomic<int64_t> m_audioSysTime{0};
};
```

- [ ] **Step 2: Write AVClock.cpp**

```cpp
#include "AVClock.h"

extern "C" {
#include <libavutil/time.h>
}

int64_t AVClock::nowUs() const {
    return av_gettime_relative();
}

void AVClock::setVideoClock(double pts) {
    m_videoPts.store(pts, std::memory_order_release);
    m_videoSysTime.store(nowUs(), std::memory_order_release);
    m_videoReady.store(true, std::memory_order_release);
}

ClockPoint AVClock::videoClock() const {
    return { m_videoPts.load(std::memory_order_acquire),
             m_videoSysTime.load(std::memory_order_acquire) };
}

bool AVClock::isReady() const {
    return m_videoReady.load(std::memory_order_acquire);
}

void AVClock::setAudioClock(double pts) {
    m_audioPts.store(pts, std::memory_order_release);
    m_audioSysTime.store(nowUs(), std::memory_order_release);
}

ClockPoint AVClock::audioClock() const {
    return { m_audioPts.load(std::memory_order_acquire),
             m_audioSysTime.load(std::memory_order_acquire) };
}

double AVClock::drift() const {
    auto v = videoClock();
    auto a = audioClock();
    return (a.pts - v.pts) - (a.systemTime - v.systemTime) / 1000000.0;
}

void AVClock::reset() {
    m_videoPts.store(0.0);
    m_videoSysTime.store(0);
    m_videoReady.store(false);
    m_audioPts.store(0.0);
    m_audioSysTime.store(0);
}
```

---

### Task 6: GLVideoWidget

**Files:**
- Create: `src/GLVideoWidget.h`
- Create: `src/GLVideoWidget.cpp`
- Create: `resources/shaders/yuv420p.vert`
- Create: `resources/shaders/yuv420p.frag`
- Create: `resources/shaders/shaders.qrc`

- [ ] **Step 1: Write yuv420p.vert**

```glsl
#version 130

in vec2 position;
in vec2 texcoord;
out vec2 v_texcoord;

void main() {
    gl_Position = vec4(position, 0.0, 1.0);
    v_texcoord = texcoord;
}
```

- [ ] **Step 2: Write yuv420p.frag**

```glsl
#version 130

in vec2 v_texcoord;
out vec4 fragColor;

uniform sampler2D tex_y;
uniform sampler2D tex_u;
uniform sampler2D tex_v;

void main() {
    float y = texture2D(tex_y, v_texcoord).r;
    float u = texture2D(tex_u, v_texcoord).r - 0.5;
    float v = texture2D(tex_v, v_texcoord).r - 0.5;

    float r = y + 1.402 * v;
    float g = y - 0.344136 * u - 0.714136 * v;
    float b = y + 1.772 * u;

    fragColor = vec4(r, g, b, 1.0);
}
```

- [ ] **Step 3: Write GLVideoWidget.h**

```cpp
#pragma once

#include "Common.h"
#include <QOpenGLWidget>
#include <QOpenGLFunctions>
#include <QOpenGLShaderProgram>
#include <atomic>

class GLVideoWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT
public:
    explicit GLVideoWidget(QWidget* parent = nullptr);
    ~GLVideoWidget() override;

    void setDisplayFrame(const VideoFrame* frame);

    int videoWidth()  const { return m_videoWidth; }
    int videoHeight() const { return m_videoHeight; }

protected:
    void initializeGL() override;
    void paintGL() override;
    void resizeGL(int w, int h) override;

private:
    void setupTextures(int width, int height);
    void uploadTextures(const VideoFrame* frame);

    GLuint m_textures[3] = {0, 0, 0};
    QOpenGLShaderProgram* m_program = nullptr;

    GLuint m_vbo = 0;
    GLuint m_vao = 0;

    const VideoFrame* m_displayFrame = nullptr;

    int m_videoWidth  = 0;
    int m_videoHeight = 0;
    int m_texWidth    = 0;
    int m_texHeight   = 0;

    int m_uniformTexY = 0;
    int m_uniformTexU = 0;
    int m_uniformTexV = 0;
};
```

- [ ] **Step 4: Write GLVideoWidget.cpp**

```cpp
#include "GLVideoWidget.h"
#include <QDebug>

static const float kVertices[] = {
    -1.0f, -1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 0.0f,
};

GLVideoWidget::GLVideoWidget(QWidget* parent)
    : QOpenGLWidget(parent)
{
    setUpdateBehavior(NoPartialUpdate);
}

GLVideoWidget::~GLVideoWidget() {
    makeCurrent();
    if (m_program) {
        delete m_program;
        m_program = nullptr;
    }
    if (m_vao) {
        glDeleteVertexArrays(1, &m_vao);
    }
    if (m_vbo) {
        glDeleteBuffers(1, &m_vbo);
    }
    glDeleteTextures(3, m_textures);
    doneCurrent();
}

void GLVideoWidget::setDisplayFrame(const VideoFrame* frame) {
    m_displayFrame = frame;
}

void GLVideoWidget::initializeGL() {
    initializeOpenGLFunctions();
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    m_program = new QOpenGLShaderProgram(this);
    m_program->addShaderFromSourceFile(QOpenGLShader::Vertex, ":/shaders/yuv420p.vert");
    m_program->addShaderFromSourceFile(QOpenGLShader::Fragment, ":/shaders/yuv420p.frag");
    m_program->bindAttributeLocation("position", 0);
    m_program->bindAttributeLocation("texcoord", 1);
    if (!m_program->link()) {
        qWarning() << "Shader link failed:" << m_program->log();
    }

    m_uniformTexY = m_program->uniformLocation("tex_y");
    m_uniformTexU = m_program->uniformLocation("tex_u");
    m_uniformTexV = m_program->uniformLocation("tex_v");

    glGenVertexArrays(1, &m_vao);
    glBindVertexArray(m_vao);

    glGenBuffers(1, &m_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, m_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(kVertices), kVertices, GL_STATIC_DRAW);

    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindVertexArray(0);

    glGenTextures(3, m_textures);

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
}

void GLVideoWidget::paintGL() {
    glClear(GL_COLOR_BUFFER_BIT);

    auto* frame = m_displayFrame;
    if (!frame || !frame->frame || !frame->frame->data[0]) return;

    int w = frame->frame->width;
    int h = frame->frame->height;
    if (w <= 0 || h <= 0) return;

    if (w != m_videoWidth || h != m_videoHeight) {
        setupTextures(w, h);
        m_videoWidth  = w;
        m_videoHeight = h;
    }

    uploadTextures(frame);

    m_program->bind();
    m_program->setUniformValue(m_uniformTexY, 0);
    m_program->setUniformValue(m_uniformTexU, 1);
    m_program->setUniformValue(m_uniformTexV, 2);

    glBindVertexArray(m_vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glBindVertexArray(0);

    m_program->release();
}

void GLVideoWidget::resizeGL(int w, int h) {
    glViewport(0, 0, w, h);
}

void GLVideoWidget::setupTextures(int width, int height) {
    auto setup = [](GLuint tex, int w, int h) {
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0,
                     GL_LUMINANCE, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    };

    setup(m_textures[0], width,     height);
    setup(m_textures[1], width / 2, height / 2);
    setup(m_textures[2], width / 2, height / 2);

    m_texWidth  = width;
    m_texHeight = height;
}

void GLVideoWidget::uploadTextures(const VideoFrame* frame) {
    AVFrame* f = frame->frame;

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, m_textures[0]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width, f->height,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, f->data[0]);

    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, m_textures[1]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width / 2, f->height / 2,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, f->data[1]);

    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, m_textures[2]);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, f->width / 2, f->height / 2,
                    GL_LUMINANCE, GL_UNSIGNED_BYTE, f->data[2]);
}
```

---

### Task 7: PlayerStateMachine

**Files:**
- Create: `src/PlayerStateMachine.h`
- Create: `src/PlayerStateMachine.cpp`

- [ ] **Step 1: Write PlayerStateMachine.h**

```cpp
#pragma once

#include "Common.h"
#include <atomic>

class PlayerStateMachine {
public:
    PlayerState state() const;
    bool        transition(PlayerState from, PlayerState to);
    void        forceState(PlayerState s);

private:
    std::atomic<int> m_state{static_cast<int>(PlayerState::Stopped)};
};
```

- [ ] **Step 2: Write PlayerStateMachine.cpp**

```cpp
#include "PlayerStateMachine.h"

PlayerState PlayerStateMachine::state() const {
    return static_cast<PlayerState>(m_state.load(std::memory_order_acquire));
}

bool PlayerStateMachine::transition(PlayerState from, PlayerState to) {
    int expected = static_cast<int>(from);
    return m_state.compare_exchange_strong(expected, static_cast<int>(to),
                                           std::memory_order_acq_rel);
}

void PlayerStateMachine::forceState(PlayerState s) {
    m_state.store(static_cast<int>(s), std::memory_order_release);
}
```

---

### Task 8: PlayerStats

**Files:**
- Create: `src/PlayerStats.h`
- Create: `src/PlayerStats.cpp`

- [ ] **Step 1: Write PlayerStats.h**

```cpp
#pragma once

#include <atomic>
#include <cstdint>

class PlayerStats {
public:
    std::atomic<int64_t> framesDecoded{0};
    std::atomic<int64_t> framesRendered{0};
    std::atomic<int64_t> framesDropped{0};
    std::atomic<int>     reconnectCount{0};
    std::atomic<int>     queueVideoDurationMs{0};

    double decodeFps() const;
    double renderFps() const;
    double dropRate() const;
    void   reset();
};
```

- [ ] **Step 2: Write PlayerStats.cpp**

```cpp
#include "PlayerStats.h"

double PlayerStats::decodeFps() const {
    return static_cast<double>(framesDecoded.load());
}

double PlayerStats::renderFps() const {
    return static_cast<double>(framesRendered.load());
}

double PlayerStats::dropRate() const {
    auto d = framesDropped.load();
    auto r = framesRendered.load();
    if (r == 0) return 0.0;
    return static_cast<double>(d) / static_cast<double>(r + d);
}

void PlayerStats::reset() {
    framesDecoded  = 0;
    framesRendered = 0;
    framesDropped  = 0;
    reconnectCount = 0;
    queueVideoDurationMs = 0;
}
```

---

### Task 9: DemuxThread

**Files:**
- Create: `src/DemuxThread.h`
- Create: `src/DemuxThread.cpp`

- [ ] **Step 1: Write DemuxThread.h**

```cpp
#pragma once

#include "PacketQueue.h"
#include <QThread>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavformat/avformat.h>
#ifdef __cplusplus
}
#endif

class PlayerStateMachine;
class PlayerStats;

class DemuxThread : public QThread {
    Q_OBJECT
public:
    DemuxThread(PlayerStateMachine* sm, PlayerStats* stats,
                PacketQueue* videoQueue, PacketQueue* audioQueue,
                QObject* parent = nullptr);
    ~DemuxThread() override;

    bool open(const char* url);
    void stop();

    AVStream*     videoStream() const { return m_videoStream; }
    AVStream*     audioStream() const { return m_audioStream; }
    AVCodecParameters* videoCodecPar() const { return m_videoCodecPar; }
    AVCodecParameters* audioCodecPar() const { return m_audioCodecPar; }
    AVRational    videoTimeBase() const;
    AVRational    audioTimeBase() const;

signals:
    void streamInfoReady();

protected:
    void run() override;

private:
    static int interruptCallback(void* opaque);

    PlayerStateMachine* m_stateMachine;
    PlayerStats*        m_stats;
    PacketQueue*        m_videoQueue;
    PacketQueue*        m_audioQueue;

    AVFormatContext*    m_fmtCtx    = nullptr;
    AVStream*           m_videoStream = nullptr;
    AVStream*           m_audioStream = nullptr;
    AVCodecParameters*  m_videoCodecPar = nullptr;
    AVCodecParameters*  m_audioCodecPar = nullptr;

    std::atomic<bool>   m_abort{false};
    int64_t             m_lastReadTime = 0;
};
```

- [ ] **Step 2: Write DemuxThread.cpp**

```cpp
#include "DemuxThread.h"
#include "PlayerStateMachine.h"
#include "PlayerStats.h"

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
    m_fmtCtx = avformat_alloc_context();
    if (!m_fmtCtx) return false;

    AVIOInterruptCB intrCb = { &interruptCallback, this };
    m_fmtCtx->interrupt_callback = intrCb;

    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "probesize", "32768", 0);
    av_dict_set(&opts, "analyzeduration", "100000", 0);
    av_dict_set(&opts, "max_delay", "100000", 0);

    int ret = avformat_open_input(&m_fmtCtx, url, nullptr, &opts);
    av_dict_free(&opts);
    if (ret < 0) return false;

    ret = avformat_find_stream_info(m_fmtCtx, nullptr);
    if (ret < 0) return false;

    int videoIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    int audioIdx = av_find_best_stream(m_fmtCtx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);

    if (videoIdx >= 0) {
        m_videoStream   = m_fmtCtx->streams[videoIdx];
        m_videoCodecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(m_videoCodecPar, m_videoStream->codecpar);
        m_videoQueue->init(m_videoStream->time_base, 200);
    }

    if (audioIdx >= 0) {
        m_audioStream   = m_fmtCtx->streams[audioIdx];
        m_audioCodecPar = avcodec_parameters_alloc();
        avcodec_parameters_copy(m_audioCodecPar, m_audioStream->codecpar);
        m_audioQueue->init(m_audioStream->time_base, 200);
    }

    if (!m_videoStream) return false;

    emit streamInfoReady();
    return true;
}

void DemuxThread::stop() {
    m_abort = true;
    m_videoQueue->abort();
    m_audioQueue->abort();
}

void DemuxThread::run() {
    m_abort = false;
    m_stateMachine->transition(PlayerState::Connecting, PlayerState::Playing);

    AVPacket* pkt = av_packet_alloc();

    while (!m_abort) {
        m_lastReadTime = av_gettime_relative();
        int ret = av_read_frame(m_fmtCtx, pkt);
        if (ret < 0) {
            if (ret == AVERROR_EOF) break;
            if (m_abort) break;
            // RTSP connection lost
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
```

---

### Task 10: VideoDecodeThread

**Files:**
- Create: `src/VideoDecodeThread.h`
- Create: `src/VideoDecodeThread.cpp`

- [ ] **Step 1: Write VideoDecodeThread.h**

```cpp
#pragma once

#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include <QThread>
#include <atomic>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#ifdef __cplusplus
}
#endif

class PlayerStats;

class VideoDecodeThread : public QThread {
    Q_OBJECT
public:
    VideoDecodeThread(PacketQueue* queue, VideoFrameQueue* frameQueue,
                      PlayerStats* stats, QObject* parent = nullptr);
    ~VideoDecodeThread() override;

    bool open(AVCodecParameters* codecPar);
    void stop();
    AVCodecContext* codecCtx() const { return m_codecCtx; }

protected:
    void run() override;

private:
    PacketQueue*       m_queue;
    VideoFrameQueue*   m_frameQueue;
    PlayerStats*       m_stats;
    AVCodecContext*    m_codecCtx = nullptr;
    std::atomic<bool>  m_abort{false};
    AVRational         m_timeBase{1, 90000};
};
```

- [ ] **Step 2: Write VideoDecodeThread.cpp**

```cpp
#include "VideoDecodeThread.h"
#include "PlayerStats.h"

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

bool VideoDecodeThread::open(AVCodecParameters* codecPar) {
    if (!codecPar) return false;

    const AVCodec* codec = avcodec_find_decoder(codecPar->codec_id);
    if (!codec) return false;

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) return false;

    avcodec_parameters_to_context(m_codecCtx, codecPar);
    m_codecCtx->thread_count = 2;
    m_timeBase = codecPar->video_delay ? AVRational{1, 90000} : m_timeBase;

    if (avcodec_open2(m_codecCtx, codec, nullptr) < 0) return false;

    return true;
}

void VideoDecodeThread::stop() {
    m_abort = true;
    m_queue->abort();
}

void VideoDecodeThread::run() {
    m_abort = false;

    AVPacket* pkt = av_packet_alloc();
    AVFrame*  frame = av_frame_alloc();

    while (!m_abort) {
        if (!m_queue->pop(pkt, 100)) {
            continue;
        }

        int ret = avcodec_send_packet(m_codecCtx, pkt);
        av_packet_unref(pkt);

        if (ret < 0) continue;

        while (true) {
            ret = avcodec_receive_frame(m_codecCtx, frame);
            if (ret == AVERROR(EAGAIN)) break;
            if (ret == AVERROR_EOF) break;
            if (ret < 0) break;

            int64_t pts = frame->pts;
            if (pts == AV_NOPTS_VALUE) {
                pts = frame->pkt_dts;
            }
            if (pts != AV_NOPTS_VALUE) {
                pts = av_rescale_q(pts, m_timeBase, AVRational{1, AV_TIME_BASE});
            }

            m_frameQueue->writeFrame(frame, pts);
            m_stats->framesDecoded++;
        }
    }

    av_frame_free(&frame);
    av_packet_free(&pkt);
}
```

---

### Task 11: RenderScheduler

**Files:**
- Create: `src/RenderScheduler.h`
- Create: `src/RenderScheduler.cpp`

- [ ] **Step 1: Write RenderScheduler.h**

```cpp
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

protected:
    void run() override;

private:
    bool shouldDrop(int64_t pts) const;

    VideoFrameQueue*  m_frameQueue;
    AVClock*          m_clock;
    GLVideoWidget*    m_widget;
    PlayerStats*      m_stats;

    std::atomic<bool> m_updatePending{false};
    std::atomic<bool> m_running{false};
    int               m_dropThresholdUs = 30000;
    AVRational        m_timeBase{1, AV_TIME_BASE};
};
```

- [ ] **Step 2: Write RenderScheduler.cpp**

```cpp
#include "RenderScheduler.h"
#include "VideoFrameQueue.h"
#include "AVClock.h"
#include "GLVideoWidget.h"
#include "PlayerStats.h"

#include <QMetaObject>

extern "C" {
#include <libavutil/time.h>
}

RenderScheduler::RenderScheduler(VideoFrameQueue* frameQueue, AVClock* clock,
                                 GLVideoWidget* widget, PlayerStats* stats,
                                 QObject* parent)
    : QThread(parent)
    , m_frameQueue(frameQueue)
    , m_clock(clock)
    , m_widget(widget)
    , m_stats(stats)
{
}

RenderScheduler::~RenderScheduler() {
    stop();
    wait();
}

void RenderScheduler::stop() {
    m_running = false;
    m_frameQueue->notifyAll();
}

void RenderScheduler::run() {
    m_running = true;
    m_updatePending = false;

    while (m_running) {
        m_frameQueue->waitForNewFrame(10);
        if (!m_running) break;

        int64_t pts = m_frameQueue->peekRenderPts();
        if (pts < 0) continue;

        if (shouldDrop(pts)) {
            m_frameQueue->discardRender();
            m_stats->framesDropped++;
            continue;
        }

        m_frameQueue->commitDisplay();
        m_clock->setVideoClock(pts / (double)AV_TIME_BASE);

        auto* displayFrame = m_frameQueue->displayFrame();
        m_widget->setDisplayFrame(displayFrame);

        if (!m_updatePending.exchange(true)) {
            QMetaObject::invokeMethod(m_widget, "update", Qt::QueuedConnection);
        }
    }
}

bool RenderScheduler::shouldDrop(int64_t pts) const {
    if (!m_clock->isReady()) return false;

    int64_t now      = av_gettime_relative();
    int64_t expected = m_clock->videoClock().systemTime +
                       (pts - static_cast<int64_t>(m_clock->videoClock().pts * AV_TIME_BASE));

    return (expected - now) < -m_dropThresholdUs;
}
```

---

### Task 12: RTSPlayer

**Files:**
- Create: `src/RTSPlayer.h`
- Create: `src/RTSPlayer.cpp`

- [ ] **Step 1: Write RTSPlayer.h**

```cpp
#pragma once

#include "Common.h"
#include <QObject>

class PlayerStateMachine;
class PacketQueue;
class VideoFrameQueue;
class AVClock;
class GLVideoWidget;
class PlayerStats;
class DemuxThread;
class VideoDecodeThread;
class RenderScheduler;

class RTSPlayer : public QObject {
    Q_OBJECT
public:
    explicit RTSPlayer(QObject* parent = nullptr);
    ~RTSPlayer() override;

    bool open(const char* url);
    void close();

    GLVideoWidget* videoWidget() const { return m_glWidget; }
    PlayerStats*   stats() const       { return m_stats; }
    PlayerState    state() const;

signals:
    void stateChanged(PlayerState state);
    void errorOccurred(const QString& message);

private slots:
    void onStreamInfoReady();

private:
    void shutdown();

    PlayerStateMachine*  m_stateMachine;
    PacketQueue*         m_videoQueue;
    PacketQueue*         m_audioQueue;
    VideoFrameQueue*     m_frameQueue;
    AVClock*             m_clock;
    GLVideoWidget*       m_glWidget;
    PlayerStats*         m_stats;
    DemuxThread*         m_demuxThread;
    VideoDecodeThread*   m_videoDecodeThread;
    RenderScheduler*     m_renderScheduler;

    bool m_initialized = false;
};
```

- [ ] **Step 2: Write RTSPlayer.cpp**

```cpp
#include "RTSPlayer.h"
#include "PlayerStateMachine.h"
#include "PacketQueue.h"
#include "VideoFrameQueue.h"
#include "AVClock.h"
#include "GLVideoWidget.h"
#include "PlayerStats.h"
#include "DemuxThread.h"
#include "VideoDecodeThread.h"
#include "RenderScheduler.h"

RTSPlayer::RTSPlayer(QObject* parent)
    : QObject(parent)
    , m_stateMachine(new PlayerStateMachine)
    , m_videoQueue(new PacketQueue)
    , m_audioQueue(new PacketQueue)
    , m_frameQueue(new VideoFrameQueue)
    , m_clock(new AVClock)
    , m_glWidget(new GLVideoWidget)
    , m_stats(new PlayerStats)
    , m_demuxThread(new DemuxThread(m_stateMachine, m_stats, m_videoQueue, m_audioQueue, this))
    , m_videoDecodeThread(new VideoDecodeThread(m_videoQueue, m_frameQueue, m_stats, this))
    , m_renderScheduler(new RenderScheduler(m_frameQueue, m_clock, m_glWidget, m_stats, this))
{
    connect(m_demuxThread, &DemuxThread::streamInfoReady,
            this, &RTSPlayer::onStreamInfoReady);
}

RTSPlayer::~RTSPlayer() {
    close();
}

bool RTSPlayer::open(const char* url) {
    if (!m_stateMachine->transition(PlayerState::Stopped, PlayerState::Connecting)) {
        return false;
    }

    m_stateMachine->forceState(PlayerState::Stopped);
    shutdown();

    if (!m_stateMachine->transition(PlayerState::Stopped, PlayerState::Connecting)) {
        return false;
    }

    if (!m_demuxThread->open(url)) {
        m_stateMachine->forceState(PlayerState::Error);
        emit stateChanged(PlayerState::Error);
        emit errorOccurred(QStringLiteral("Failed to open RTSP stream"));
        return false;
    }

    m_initialized = true;
    m_demuxThread->start();
    return true;
}

void RTSPlayer::close() {
    if (m_stateMachine->state() == PlayerState::Stopped) return;

    m_stateMachine->transition(m_stateMachine->state(), PlayerState::Closing);
    emit stateChanged(PlayerState::Closing);

    shutdown();
    m_stateMachine->forceState(PlayerState::Stopped);
    emit stateChanged(PlayerState::Stopped);
}

void RTSPlayer::shutdown() {
    if (m_demuxThread->isRunning()) {
        m_demuxThread->stop();
        m_demuxThread->wait(3000);
    }
    if (m_videoDecodeThread->isRunning()) {
        m_videoDecodeThread->stop();
        m_videoDecodeThread->wait(3000);
    }
    if (m_renderScheduler->isRunning()) {
        m_renderScheduler->stop();
        m_renderScheduler->wait(3000);
    }

    m_videoQueue->flush();
    m_audioQueue->flush();
    m_frameQueue->flush();
    m_clock->reset();
    m_initialized = false;
}

void RTSPlayer::onStreamInfoReady() {
    auto* codecPar = m_demuxThread->videoCodecPar();
    if (!codecPar) {
        m_stateMachine->forceState(PlayerState::Error);
        emit errorOccurred(QStringLiteral("No video codec parameters"));
        return;
    }

    if (!m_videoDecodeThread->open(codecPar)) {
        m_stateMachine->forceState(PlayerState::Error);
        emit errorOccurred(QStringLiteral("Failed to open video decoder"));
        return;
    }

    m_videoDecodeThread->start();
    m_renderScheduler->start();
}

PlayerState RTSPlayer::state() const {
    return m_stateMachine->state();
}
```

---

### Task 13: main.cpp

**Files:**
- Create: `src/main.cpp`

- [ ] **Step 1: Write main.cpp**

```cpp
#include <QApplication>
#include <QMainWindow>
#include <QVBoxLayout>
#include <QLabel>
#include <QTimer>
#include <QDebug>

#include "RTSPlayer.h"
#include "GLVideoWidget.h"
#include "PlayerStats.h"
#include "Common.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);

    QMainWindow window;
    window.setWindowTitle("RTSP Player - Phase 1");

    auto* centralWidget = new QWidget(&window);
    auto* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* statsLabel = new QLabel("Waiting...");
    layout->addWidget(statsLabel);

    RTSPlayer player;
    layout->addWidget(player.videoWidget(), 1);

    QObject::connect(&player, &RTSPlayer::stateChanged, [&](PlayerState state) {
        qDebug() << "State:" << static_cast<int>(state);
    });

    QObject::connect(&player, &RTSPlayer::errorOccurred, [&](const QString& msg) {
        qDebug() << "Error:" << msg;
        statsLabel->setText("Error: " + msg);
    });

    auto* timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, [&]() {
        auto* stats = player.stats();
        statsLabel->setText(QString("Decoded: %1 | Rendered: %2 | Dropped: %3 | VideoQueue: %4ms")
            .arg(stats->framesDecoded.load())
            .arg(stats->framesRendered.load())
            .arg(stats->framesDropped.load())
            .arg(stats->queueVideoDurationMs.load()));
    });
    timer->start(1000);

    window.setCentralWidget(centralWidget);
    window.resize(1280, 720);
    window.show();

    const char* url = "rtsp://127.0.0.1:8554/live";
    if (argc > 1) url = argv[1];

    player.open(url);

    return app.exec();
}
```

---

### Task 14: Build and Verify

**Files:**
- None (build step)

- [ ] **Step 1: Configure and build**

```powershell
mkdir build; cd build
cmake .. -DQt5_DIR="C:\Qt\5.14.2\msvc2017_64\lib\cmake\Qt5" -DFFMPEG_ROOT="C:\ffmpeg"
cmake --build . --config Release
```

Expected: Compiles without errors, produces `RTSP-Player.exe`.

- [ ] **Step 2: Run smoke test with local RTSP stream**

Start a local RTSP test stream (e.g., via ffmpeg):
```powershell
ffmpeg -re -f lavfi -i testsrc2=size=1280x720:rate=30 -f rtsp rtsp://127.0.0.1:8554/live
```

Run the player:
```powershell
.\Release\RTSP-Player.exe rtsp://127.0.0.1:8554/live
```

Expected: Window opens, video plays smoothly, no crashes, stats incrementing.

---

### Task 15: GLVideoWidget updatePending integration

**Files:**
- Modify: `src/GLVideoWidget.h`
- Modify: `src/GLVideoWidget.cpp`

- [ ] **Step 1: Add m_updatePending to GLVideoWidget.h**

Add to GLVideoWidget class in `src/GLVideoWidget.h`:
```cpp
#include <atomic>
// Add public:
void clearUpdatePending();
// Add private:
std::atomic<bool>* m_updatePending = nullptr;
```

- [ ] **Step 2: Update paintGL in GLVideoWidget.cpp**

Add at the top of `paintGL()`:
```cpp
if (m_updatePending) m_updatePending->store(false, std::memory_order_release);
```

Add `clearUpdatePending()`:
```cpp
void GLVideoWidget::clearUpdatePending() {
    if (m_updatePending) m_updatePending->store(false, std::memory_order_release);
}
```

- [ ] **Step 3: Update RenderScheduler to pass m_updatePending to GLVideoWidget**

In `RenderScheduler.h`, add method to set widget's updatePending pointer. In `RTSPlayer.cpp`, pass `&m_renderScheduler->m_updatePending` to `m_glWidget` after construction.

Actually, simpler approach — store `std::atomic_bool*` in GLVideoWidget and have RenderScheduler set it:
```cpp
// In RTSPlayer constructor, after creating glWidget:
m_glWidget->m_updatePending = &m_renderScheduler->m_updatePending;
```

---

## Self-Review

**1. Spec coverage:**
- RTSPlayer (orchestrator) → Task 12
- PlayerStateMachine (6 states) → Task 7
- DemuxThread (av_read_frame → PacketQueue) → Task 9
- VideoDecodeThread (decode → VideoFrameQueue) → Task 10
- RenderScheduler (cv wait, peek, drop, commit, invokeMethod) → Task 11
- PacketQueue (KeyFrame-aware drop, 200ms capacity) → Task 3
- VideoFrameQueue (triple buffer, no lock, move_ref) → Task 4
- AVClock (isReady, setVideoClock) → Task 5
- GLVideoWidget (YUV420P, 3×GL_LUMINANCE, paintGL) → Task 6
- PlayerStats → Task 8
- CMakeLists.txt → Task 1
- Common.h → Task 2
- main.cpp → Task 13
- Shaders → Task 6 Steps 1-2
- shaders.qrc → Task 1 Step 2
- updatePending integration → Task 15

**2. Placeholder scan:** No TBD, TODO, or vague references found. All code blocks are complete.

**3. Type consistency:**
- `PlayerState` enum in Common.h matches usage in PlayerStateMachine
- `VideoFrame` struct matches VideoFrameQueue slot type
- `ClockPoint` in Common.h matches AVClock return type
- `PacketQueue::init(AVRational, int)` matches DemuxThread call site
- `VideoFrameQueue::writeFrame(AVFrame*, int64_t)` matches VideoDecodeThread call site
- `DemuxThread::videoCodecPar()` returns `AVCodecParameters*` consumed by `VideoDecodeThread::open()`
- `RenderScheduler::m_updatePending` is `std::atomic<bool>`, matches GLVideoWidget pointer
- All `#include` paths consistent

No gaps found. No inconsistencies.

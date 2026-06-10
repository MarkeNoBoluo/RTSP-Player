# SDL2 全替代迁移计划

## 目标

将 RTSP-Player 从 Qt Multimedia/OpenGL 渲染层迁移到纯 SDL2 + std::thread，仿 ffplay 架构。
移除全部 Qt 依赖（QApplication / QMainWindow / QOpenGLWidget / QAudioOutput / QThread / signals/slots），改用 SDL2 做音视频输出 + C++17 标准库线程。

## 前置条件

| 组件 | 路径 | 状态 |
|------|------|------|
| SDL2 x86 | `3rd/SDL2/lib/x86/SDL2.lib` + `3rd/SDL2/lib/x86/SDL2.dll` + `3rd/SDL2/include/` | 已就绪 |
| FFmpeg x86 | `3rd/FFmpeg/bin/*.dll` + `3rd/FFmpeg/lib/*.lib` + `3rd/FFmpeg/include/` | 已就绪 |

目标平台：MSVC 2017 x86 (32-bit)，Windows 32位设备支持。

## 架构变更

### 之前（Qt 6 线程）

```
UI Thread ── QMainWindow + QLabel + GLVideoWidget::paintGL()
RenderScheduler Thread ── frame timing / drop / invokeMethod(update)
VideoDecode Thread ── 解码 → VideoFrameQueue(3slot)
Demux Thread ── av_read_frame → PacketQueue
Audio Thread ── decode + swresample → AudioPullDevice → QAudioOutput
```

### 之后（SDL2 + std::thread 4 线程）

```
Main Thread (高频tick ~1ms)
  ├── SDL_PollEvent() → 事件处理 (quit/key/win/timer)
  ├── videoRefresh() → A/V sync 判断 → IRenderer::displayFrame()
  └── SDL_Delay(1)

Demux Thread (std::thread)        → av_read_frame + serial → PacketQueue(video/audio)
VideoDecode Thread (std::thread)  → avcodec → VideoFrameQueue(4slot+serial)
AudioDecode Thread (std::thread)  → avcodec → swresample → AudioRingBuffer(100ms)

SDL Audio Callback (SDL 内部线程)
  → read AudioRingBuffer → memcpy → SDL stream
  → setAudioClock(pts + consumed/sample_rate)
```

**变化要点**：
- RenderScheduler 合入主循环 `videoRefresh()`，不再独立线程
- QAudioOutput pull 模型 → SDL Audio 推模型回调
- AudioPullDevice (QIODevice) → AudioRingBuffer (纯 C++ 环形缓冲)
- GLVideoWidget + YUV shader → SDL_UpdateYUVTexture + SDL_RenderCopy
- QObject/signals/slots → std::function 回调
- QTimer → SDL_AddTimer + SDL_USEREVENT
- Qt5::Multimedia / Qt5::OpenGL / Qt5::Widgets → 全部移除

## 文件变更清单

### 删除

| 文件 | 原因 |
|------|------|
| `src/GLVideoWidget.h/.cpp` | SDLRenderer 替代 |
| `src/RenderScheduler.h/.cpp` | 合入主循环 videoRefresh() |
| `src/AudioPullDevice.h/.cpp` | AudioRingBuffer 替代 |
| `resources/shaders/yuv420p.vert` | 不再需要 GLSL |
| `resources/shaders/yuv420p.frag` | 不再需要 GLSL |
| `resources/shaders/shaders.qrc` | 不再需要 Qt Resource |

### 新增

| 文件 | 职责 |
|------|------|
| `src/SDLRenderer.h/.cpp` | IRenderer 接口 + SDLRenderer 实现 (SDL_Window/Renderer/Texture) |
| `src/SDLAudio.h/.cpp` | SDL 音频设备初始化 + 回调注册 |
| `src/AudioRingBuffer.h/.cpp` | 100ms 环形缓冲 (带 serial + PTS + chunk 追踪) |

### 修改

| 文件 | 变更 |
|------|------|
| `src/main.cpp` | 重写：SDL 窗口 + 高频 tick 主循环 + 事件处理 |
| `src/RTSPlayer.h/.cpp` | 去 QObject，std::function 回调替代 signals |
| `src/StreamLifecycleManager.h/.cpp` | 去 QObject/QTimer，+serial 机制，SDL_AddTimer 替代 QTimer |
| `src/DemuxThread.h/.cpp` | QThread→std::thread，emit streamError→m_onStreamError 回调 |
| `src/VideoDecodeThread.h/.cpp` | QThread→std::thread |
| `src/AudioWorker.h/.cpp` | QObject→普通类，AudioRingBuffer 替代 AudioPullDevice，修复忙等待 |
| `src/VideoFrameQueue.h/.cpp` | 3→4 slot，+serial，去 Qt 依赖 |
| `src/PacketQueue.h/.cpp` | +serial 字段 |
| `CMakeLists.txt` | x86 目标，SDL2 链接，移除全部 Qt 依赖 |

### 不变

`PacketQueue` / `AVClock` / `PlayerStats` / `PlayerStateMachine` / `Common.h` / `Logger` — 无 Qt 依赖。

## 关键设计决策

### 1. 主循环：高频 tick，不依赖 SDL timer 精度

```cpp
while (running) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {  // 非阻塞，清空事件队列
        switch (event.type) {
        case SDL_QUIT: running = false; break;
        case SDL_KEYDOWN: onKey(event.key.keysym.sym); break;
        case SDL_WINDOWEVENT: onResize(event.window.data1, event.window.data2); break;
        case SDL_USEREVENT: onTimer(event.user.code); break;
        }
    }
    player.videoRefresh();  // 内部判断是否到显示时间
    SDL_Delay(1);           // ~1ms tick
}
```

不使用 SDL_WaitEventTimeout 驱动渲染——Windows timer 精度不稳定，高频 tick 更可靠。

### 2. A/V Sync

主循环 `videoRefresh()` 通过 `AVClock::drift()` 计算音视频偏差，决定显示时机：

```
delay = frameDuration + drift(videoClock, audioClock)
- video 落后 audio → 减小 delay（追赶）
- video 超前 audio → 增大 delay（等待）
- delay < 0 且 |delay| > threshold → drop frame
```

### 3. 音频时钟：pts + consumed / sample_rate

SDL 音频回调中追踪消费偏移量，时钟反映实际播放位置而非解码 PTS：

```cpp
double currentPts = chunk.pts + (double)consumedSamples / sampleRate;
m_clock->setAudioClock(currentPts);
```

### 4. IRenderer 接口（为 D3D11 预留）

```cpp
class IRenderer {
public:
    virtual ~IRenderer() = default;
    virtual bool init(void* nativeWindow) = 0;
    virtual void displayFrame(AVFrame* frame) = 0;
    virtual void setWindowSize(int w, int h) = 0;
    virtual void destroy() = 0;
};

class SDLRenderer : public IRenderer {
    // SDL_UpdateYUVTexture + SDL_RenderCopy + SDL_RenderPresent
};
```

Phase 1 用 SDL_UpdateYUVTexture（CPU upload），后续可切 D3D11Renderer（GPU direct）。

### 5. Serial 机制（reconnect 安全）

```
StreamLifecycleManager::m_pktSerial (std::atomic<int>)

open() / reconnect() → m_pktSerial++

DemuxThread: 每包 stamp serial
VideoDecodeThread: pop 时 serial != global → drop
AudioWorker: pop 时 serial != global → drop
VideoFrameQueue: writeFrame 时 stamp serial
AudioRingBuffer: write chunk 时 stamp serial

videoRefresh(): displayFrame 前检查 frame.serial == global

shutdownPipeline: m_pktSerial++ → 所有旧数据自动失效
```

## 实施阶段

### P0: SDK 就绪 ✓
- [x] SDL2 x86 放置到 `3rd/SDL2/`
- [x] FFmpeg x86 拷贝到 `3rd/FFmpeg/`

### P1: CMakeLists.txt + 新建文件 + main.cpp 骨架
- [ ] CMakeLists.txt：x86, SDL2 链接, 移除全部 Qt
- [ ] `SDLRenderer.h/.cpp`：IRenderer + SDLRenderer
- [ ] `SDLAudio.h/.cpp`：SDL 音频初始化 + 回调
- [ ] `AudioRingBuffer.h/.cpp`：100ms 环形缓冲 + serial
- [ ] `main.cpp` 重写：SDL 窗口 + 高频 tick 主循环
- [ ] 编译通过

### P2: 线程 + 管理类去 Qt
- [ ] `DemuxThread`：QThread→std::thread, signal→callback
- [ ] `VideoDecodeThread`：QThread→std::thread
- [ ] `AudioWorker`：QObject→普通类, AudioRingBuffer
- [ ] `StreamLifecycleManager`：QObject→普通类, QTimer→SDL_AddTimer
- [ ] `RTSPlayer`：QObject→普通类
- [ ] `VideoFrameQueue`：3→4 slot, +serial
- [ ] `PacketQueue`：+serial
- [ ] 编译通过

### P3: A/V Sync 接入 + 旧文件删除
- [ ] 主循环 videoRefresh() 接入 AVClock::drift()
- [ ] 删除 GLVideoWidget, RenderScheduler, AudioPullDevice, shaders
- [ ] 编译通过

### P4: 联调验证
- [ ] RTSP 播放 → 音频/视频正常
- [ ] 断线重连 → serial 机制不掉旧帧
- [ ] 长时间运行 → 无泄漏、无累积延迟
- [ ] 60fps 视频流畅不 jitter

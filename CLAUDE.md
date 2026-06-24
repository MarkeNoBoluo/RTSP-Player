# CLAUDE.md — RTSP-Player v2

SDL2 + FFmpeg RTSP streaming player. Pure C++17, no Qt dependencies.

## Build & Run

```bash
cmake -S . -B build
cmake --build build --config Release   # or Debug
```

- **Compiler**: MSVC 2017, C++17, x86, `/utf-8`, `/SAFESEH:NO`
- **Output**: `bin/MSVC2017_x86_<Config>/RTSP-Player.exe`
- **Working dir at runtime**: the output directory (DLLs are copied there via POST_BUILD)

```bash
cd bin/MSVC2017_x86_Release
./RTSP-Player.exe --url rtsp://192.168.1.100:554/stream
```

## CLI Usage

```
--url <rtsp_url>          RTSP stream URL (required; default auto-generated if omitted)
--log <path>              Log file path (default: rtsp_player.log)
--csv <path>              CSV stats path (default: rtsp_player_stats.csv)
--no-csv                  Disable CSV stats
--fullscreen              Start in fullscreen mode
--windowed                Start in windowed mode (default)
--transport <tcp|udp>     RTSP transport protocol (default: udp)
--title <string>          Window title (default: "RTSP Player")
--exit-after <seconds>    Auto-exit after N seconds
--help                    Show help
```

Positional argument form (`argv[1]` = url, `argv[2]` = log) is deprecated but still accepted.

Keyboard: `Esc` / `Q` = quit, `F` = toggle fullscreen.

## Architecture

Pure single-process, multi-threaded SDL2 application. Main thread runs a 1ms-tick event loop with `SDL_PollEvent`, `videoRefresh()`, and `av_usleep` compensation.

### Component tree

```
main()
 ├── SDL_Init (VIDEO | AUDIO | TIMER)
 ├── SDLRenderer (SDL_Window + SDL_Renderer + SDL_Texture, swscale-based upload)
 ├── RTSPlayer (facade)
 │    ├── PlayerStateMachine — 7-state atomic: Stopped→Connecting→Playing→Recovering→Reconnecting→Error→Closing
 │    ├── StreamLifecycleManager — pipeline owner, owns all threads and FFmpeg contexts
 │    │    ├── DemuxThread      → reads AVPackets, pushes to PacketQueues, triggers reconnect on stream error
 │    │    ├── VideoDecodeThread → pops from video PacketQueue, decodes, writes to VideoFrameQueue
 │    │    ├── AudioWorker      → pops from audio PacketQueue, decodes, swresample→AudioRingBuffer
 │    │    └── SDLAudio         → SDL audio callback reads from AudioRingBuffer, updates AVClock audio PTS
 │    ├── PacketQueue ×2 (video, audio) — deques with condition_variable, serial-aware, peak tracking
 │    ├── VideoFrameQueue (24-slot) — ring buffer between decode and render, ~800ms at 30fps
 │    ├── AVClock — independent video/audio PTS with system timestamps, drift() for A/V sync
 │    └── PlayerStats — all std::atomic metrics, CSV export, SDL timer-driven polling
 └── SDL_AddTimer callbacks push SDL_USEREVENT:
      EVENT_STATS (5s)     → PlayerStats::writeCsvRow()
      EVENT_RECONNECT      → StreamLifecycleManager::doReconnect()
```

### Serial mechanism

A generation counter (`m_pktSerial`, `m_generation`) is incremented on reconnect and propagated to every queue and worker. All stale packets/frames with an older serial are discarded, ensuring clean pipeline restart without flushing complexity.

### AVClock & sync

`AVClock` stores `(pts, systemTime)` pairs for video and audio independently:
- **audio**: set by `SDLAudio::sdlCallback` (driven by hardware clock)
- **video**: set by `RTSPlayer::videoRefresh` after rendering a frame
- `drift()` = `audioClock - videoClock` — positive means audio ahead, video must catch up

### Render loop (videoRefresh)

1. Peek `VideoFrameQueue` display slot
2. If serial mismatch → discard and advance
3. If frame is late (pts < audio clock by threshold) → drop
4. If frame is early (pts ahead of audio by large margin) → sleep
5. Otherwise → `SDLRenderer::displayFrame()` (swscale to NV12, SDL_UpdateYUVTexture, SDL_RenderCopy)
6. Update `AVClock` video PTS, increment render stats

## FFmpeg Usage

- **Version**: bundled 4.2.9 in `3rd/FFmpeg/`
- **Headers must be wrapped** in `extern "C" {}` blocks
- `av_log_set_callback(ffmpegLogCallback)` registered in `main.cpp`, routes FFmpeg log messages to `logger::Logger`

## Logging

Singleton `logger::Logger` in `src/logger/Logger.h`:

```cpp
logger::Logger::instance().initLogFile(logPath);  // must init before use
LOG_DEBUG("msg %d", val);
LOG_INFO("msg %s", str);
LOG_WARN("msg");
LOG_ERROR("msg");
logger::Logger::instance().closeLogFile();        // via atexit or explicit
```

Thread-safe via internal `std::mutex`. Log format: `[LEVEL] file.cpp:123 msg`.

## Conventions

- **C++17**, `#pragma once`, `m_` member prefix, 4-space indent
- **UTF-8 with BOM** (MSVC `/utf-8`)
- No Qt, no signals/slots, no QWidget — all rendering via SDL2
- `src/test_qt.cpp` is an orphaned Qt smoke test from v1 era — it is **not compiled** by CMakeLists.txt and should not be deleted

## Directory Layout

```
3rd/
 ├── SDL2/          — SDL2 development libraries (x86)
 └── FFmpeg/        — FFmpeg 4.2.9 dev libraries (x86)
src/
 ├── main.cpp       — entry point, CLI parsing, SDL init, main event loop
 ├── RTSPlayer.*    — facade: videoRefresh, frame timing, sync logic
 ├── StreamLifecycleManager.* — pipeline lifecycle, open/close/reconnect
 ├── PlayerStateMachine.*     — atomic 7-state FSM
 ├── DemuxThread.*            — AVPacket reading thread
 ├── VideoDecodeThread.*      — video decode thread
 ├── AudioWorker.*            — audio decode + resample thread
 ├── AudioRingBuffer.*        — lock-free-ish ring buffer (32 chunks)
 ├── SDLAudio.*               — SDL audio device wrapper
 ├── SDLRenderer.*            — SDL window + texture + swscale
 ├── PacketQueue.*            — thread-safe packet queue with serial support
 ├── VideoFrameQueue.*        — 24-slot frame ring buffer
 ├── AVClock.*                — dual PTS clock for A/V sync
 ├── PlayerStats.*            — atomic metrics + CSV export
 ├── Common.h                 — enums, structs, FFmpeg header wrappers
 ├── test_qt.cpp              — orphaned v1 smoke test (not built)
 └── logger/
      └── Logger.*            — singleton printf-style logger
```

# RTSP-Player AGENTS.md

## Build & Run

- **Build**: Qt Creator (Qt 5.14.2, msvc2017_64). CMake → VS 2022 sln, x64.
- **CLI build**: `cmake --build build` (no Makefile/ninja wrapper — pass `--config Debug|Release`).
- **Output**: `bin/MSVC2017_x64_Debug/RTSP-Player.exe` (or `_Release`).
- **Working dir** (set in Qt Creator run config): `bin/MSVC2017_x64_<Config>/` — FFmpeg DLLs and `rtsp_player.log` land here.
- **No tests, no lint, no CI** — manual-build desktop app.
- **Tracked vs ignored**: `build/`, `bin/`, `*.user`, `*.log`, `.superpowers/` are ignored. `CMakeLists.txt.user` is tracked despite `*.user` rule (it has a literal override in `.gitignore`).

## Runtime

- `argv[1]` = RTSP URL (default in `src/main.cpp:127` is `rtsp://192.168.42.116:25544/2026_06_09`).
- `argv[2]` = log file path (default `rtsp_player.log`).
- Window title: `RTSP Player - Phase 4.5` — useful as a quick "what stage is this build" hint.

## Architecture

- **RTSPlayer** (`src/RTSPlayer.cpp:11`) is the public facade; constructor wires owned children (`PlayerStateMachine`, two `PacketQueue`s, `VideoFrameQueue`, `AVClock`, `GLVideoWidget`, `PlayerStats`, `StreamLifecycleManager`) and forwards `open/close/state` + `stateChanged/errorOccurred` signals. `open()` itself just delegates to `m_lifecycle`.
- **StreamLifecycleManager** (`src/StreamLifecycleManager.h:31`) owns the entire decode/render pipeline: demux/codec ctx, threads, queues, audio device, reconnect timer, and a `m_generation` counter for stale-frame detection.
- Pipeline (video): **DemuxThread** → `PacketQueue` (200ms, keyframe-aware drop) → **VideoDecodeThread** → **VideoFrameQueue** (4-slot ring, atomic `m_displayHead`) → **RenderScheduler** (CV wait + drop + `invokeMethod(update)`) → **GLVideoWidget::paintGL**.
- Pipeline (audio): **DemuxThread** → `PacketQueue` → **AudioWorker** (decode + `swresample` → PCM) → **AudioPullDevice** (circular `QIODevice`) → **QAudioOutput**.
- **PlayerStateMachine** (`src/PlayerStateMachine.h`): 7 states — Stopped→Connecting→Playing→Recovering→Reconnecting→Error→Closing. All fields `std::atomic`, transitions via `compare_exchange`. `PlayerState` enum lives in `src/Common.h:15`.
- **AVClock**: independent `setVideoClock` / `setAudioClock` for A/V sync. RenderScheduler uses clock to decide drop/delay.
- **PlayerStats** (`src/PlayerStats.h`): all metrics are `std::atomic` (safe cross-thread reads). MainWindow polls every 1s for on-screen label, `writeCsvRow()` every 5s.
- **VideoFrameQueue** slot lifecycle: `FREE → WRITING → READY → DISPLAYING` (`src/VideoFrameQueue.h:10`). UI reads `displayHead()` lock-free via `std::atomic<PublishedFrame>`. Decoded frames are NOT `av_frame_move_ref`'d (despite the spec) — slots own heap-allocated `AVFrame*`.
- **RenderScheduler does no OpenGL**; it only calls `QMetaObject::invokeMethod(m_widget, "update", Qt::QueuedConnection)` with an `m_updatePending` guard.

## FFmpeg

- Bundled in `3rd/FFmpeg4.2.9/` (alternate `FFmpeg-n4.2.10` exists but `CMakeLists.txt:11` pins 4.2.9).
- `CMakeLists.txt` links `avformat / avcodec / avutil / swresample` and `POST_BUILD`-copies `avcodec-58.dll`, `avformat-58.dll`, `avutil-56.dll`, `swresample-3.dll`.
- FFmpeg headers **must** be in `extern "C" { }` blocks — see `src/Common.h:5` and `src/StreamLifecycleManager.h:10` for the canonical pattern.
- FFmpeg log callback `ffmpegLogCallback` is registered in `main.cpp:46` and routes to the project logger (filters `> AV_LOG_WARNING`).

## Logging

- Singleton `logger::Logger` (`src/logger/Logger.h:12`). Macros `LOG_DEBUG/LOG_INFO/LOG_WARN/LOG_ERROR` are printf-style, defined at file scope in the header.
- **Order matters**: `instance().initLogFile(path)` before first `LOG_*`; `closeLogFile()` at exit (registered via `atexit` in `main.cpp:41`).
- Thread-safe (mutex-protected writes). Level filter defaults to `Debug`.

## Conventions

- C++17, `#pragma once`, members prefixed `m_`, Qt `signals/slots` for cross-thread events, `Qt::QueuedConnection` for cross-thread widget access.
- Indent: 4 spaces. Source encoding: UTF-8 with BOM — enforced by `target_compile_options(... /utf-8)` in `CMakeLists.txt:47`.
- `src/test_qt.cpp` is a standalone smoke test, **NOT** in `CMakeLists.txt` SOURCES — don't add it to the build; don't remove it (it documents Qt bring-up).
- Design docs live in `docs/superpowers/specs/`; phase plans in `docs/superpowers/plans/`. The lock-free design doc (`2026-06-08-lock-free-render-pipeline-design.md`) and the `2026-06-08-rtsp-player-design.md` spec describe intent — code in `src/` is the source of truth where they disagree (e.g. 4 slots vs spec's 3).

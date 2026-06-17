#define SDL_MAIN_HANDLED
#include <SDL.h>

extern "C" {
__declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
__declspec(dllimport) unsigned int __stdcall timeEndPeriod(unsigned int);
}

#include "RTSPlayer.h"
#include "SDLRenderer.h"
#include "PlayerStats.h"
#include "AVClock.h"
#include "StreamLifecycleManager.h"
#include "logger/Logger.h"
#include <time.h>

extern "C" {
#include <libavutil/log.h>
#include <libavutil/time.h>
#include <libavformat/avformat.h>
}

static void ffmpegLogCallback(void*, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    logger::Logger::instance().log(logger::Level::Debug, "ffmpeg", 0, "%s", buf);
}

static Uint32 onStatsTimer(Uint32 interval, void* param) {
    SDL_Event event;
    SDL_zero(event);
    event.type = SDL_USEREVENT;
    event.user.code = EVENT_STATS;
    event.user.data1 = param;
    SDL_PushEvent(&event);
    return interval; // repeating timer
}

int main(int argc, char* argv[]) {
    const char* logPath = "rtsp_player.log";
    if (argc > 2) logPath = argv[2];
    logger::Logger::instance().initLogFile(logPath);
    atexit([]() { logger::Logger::instance().closeLogFile(); });

    LOG_INFO("RTSP Player v2 (SDL2) start");

    av_log_set_level(AV_LOG_DEBUG);
    av_log_set_callback(ffmpegLogCallback);

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) < 0) {
        LOG_ERROR("SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    timeBeginPeriod(1);

    SDLRenderer renderer("RTSP Player", 1280, 720);

    RTSPlayer player;
    player.setRenderer(&renderer);
    player.setStateCallback([](PlayerState state) {
        const char* names[] = {"Stopped","Connecting","Playing","Recovering","Reconnecting","Error","Closing"};
        LOG_INFO("State: %s", names[(int)state]);
    });
    player.setErrorCallback([](const char* msg) {
        LOG_ERROR("Error: %s", msg);
    });
    
    // 获取当前时间戳（自1970年1月1日以来的秒数）
    time_t now = time(0);
    // 使用strftime函数+localtime 将时间戳转换为自定义格式的日期字符串
    char format_time[256];
    strftime(format_time, sizeof(format_time), "%Y_%m_%d", localtime(&now));
    LOG_INFO("Current date and time: %s", format_time);

    const char* baseUrl = "rtsp://192.168.42.116:25544/";
    // 创建足够大的字符数组存储最终URL
    char defaultUrl[512];  // 足够大的缓冲区
    // 先复制基础URL
    strcpy(defaultUrl, baseUrl);
    // 将日期追加到URL后面
    strcat(defaultUrl, format_time);

    LOG_INFO("Full URL: %s", defaultUrl);  // 输出: rtsp://192.168.42.116:25544/2026_06_13

    if (argc > 1) {
        strcpy(defaultUrl, argv[1]);
    }

    LOG_INFO("Open: %s", defaultUrl);
    player.open(defaultUrl);

    // Stats CSV timer: writes every 5 seconds via SDL_USEREVENT
    SDL_AddTimer(5000, onStatsTimer, player.stats());

    bool running = true;
    while (running) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            switch (event.type) {
            case SDL_QUIT:
                running = false;
                break;
            case SDL_KEYDOWN:
                if (event.key.keysym.sym == SDLK_ESCAPE || event.key.keysym.sym == SDLK_q)
                    running = false;
                else if (event.key.keysym.sym == SDLK_f) {
                    Uint32 flags = SDL_GetWindowFlags(renderer.window());
                    SDL_SetWindowFullscreen(renderer.window(),
                        (flags & SDL_WINDOW_FULLSCREEN) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
                }
                break;
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED)
                    renderer.setWindowSize(event.window.data1, event.window.data2);
                break;
            case SDL_USEREVENT:
                switch (event.user.code) {
                case EVENT_RECONNECT:
                    static_cast<StreamLifecycleManager*>(event.user.data1)->doReconnect();
                    break;
                case EVENT_STATS:
                    static_cast<PlayerStats*>(event.user.data1)->writeCsvRow();
                    break;
                }
                break;
            }
        }

        int64_t beforeUs = av_gettime_relative();
        player.videoRefresh();
        int64_t elapsedUs = av_gettime_relative() - beforeUs;
        int64_t sleepUs = 1000 - elapsedUs;
        if (sleepUs > 100) {
            av_usleep((unsigned)sleepUs);
        }
    }

    player.close();
    timeEndPeriod(1);
    SDL_Quit();
    return 0;
}

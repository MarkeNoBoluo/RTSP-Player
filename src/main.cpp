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
#include "AVClock.h"
#include "logger/Logger.h"

extern "C" {
#include <libavutil/log.h>
#include <libavformat/avformat.h>
}

static void ffmpegLogCallback(void*, int level, const char* fmt, va_list vl) {
    if (level > AV_LOG_WARNING) return;
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, vl);
    logger::Logger::instance().log(logger::Level::Debug, "ffmpeg", 0, "%s", buf);
}

static void dumpProtocols() {
    void* opaque = nullptr;
    const char* name;
    LOG_INFO("Supported FFmpeg input protocols:");
    while ((name = avio_enum_protocols(&opaque, 0)) != nullptr) {
        LOG_INFO("  %s", name);
    }
}

int main(int argc, char* argv[]) {
    // Initialize log file first, before any LOG calls
    const char* logPath = "rtsp_player.log";
    if (argc > 2) logPath = argv[2];
    logger::Logger::instance().initLogFile(logPath);
    atexit([]() { logger::Logger::instance().closeLogFile(); });

    LOG_INFO("===== RTSP Player starting =====");

    // Register FFmpeg log callback to see internal FFmpeg messages
    av_log_set_level(AV_LOG_DEBUG);
    av_log_set_callback(ffmpegLogCallback);
    LOG_INFO("FFmpeg log callback registered");
    LOG_INFO("FFmpeg version: %s", av_version_info());
    dumpProtocols();

    LOG_INFO("Creating QApplication...");
    QApplication app(argc, argv);
    LOG_INFO("QApplication created");

    QMainWindow window;
    window.setWindowTitle("RTSP Player - Phase 3");

    auto* centralWidget = new QWidget(&window);
    auto* layout = new QVBoxLayout(centralWidget);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* statsLabel = new QLabel("Waiting...");
    layout->addWidget(statsLabel);
    LOG_INFO("UI layout created");

    LOG_INFO("Creating RTSPlayer...");
    RTSPlayer player;
    LOG_INFO("RTSPlayer created");

    layout->addWidget(player.videoWidget(), 1);

    QObject::connect(&player, &RTSPlayer::stateChanged, [&](PlayerState state) {
        const char* names[] = {"Stopped","Connecting","Playing","Recovering","Reconnecting","Error","Closing"};
        LOG_INFO("State changed: %s (%d)", names[(int)state], (int)state);
        qDebug() << "State:" << static_cast<int>(state);
    });

    QObject::connect(&player, &RTSPlayer::errorOccurred, [&](const QString& msg) {
        LOG_ERROR("RTSPlayer error: %s", msg.toUtf8().constData());
        qDebug() << "Error:" << msg;
        statsLabel->setText("Error: " + msg);
    });

    auto* timer = new QTimer(&window);
    QObject::connect(timer, &QTimer::timeout, [&]() {
        auto* stats = player.stats();
        int64_t latenessMs = stats->lastLatenessUs.load() / 1000;
        statsLabel->setText(QString("Decoded: %1 | Rendered: %2 | Dropped: %3 | Lateness: %4ms")
            .arg(stats->framesDecoded.load())
            .arg(stats->framesRendered.load())
            .arg(stats->framesDropped.load())
            .arg(latenessMs));
    });
    timer->start(1000);

    auto* renderTimer = new QTimer(&window);
    QObject::connect(renderTimer, &QTimer::timeout, [&]() {
        if (player.state() == PlayerState::Playing) {
            player.videoWidget()->update();
        }
    });
    renderTimer->start(16);

    window.setCentralWidget(centralWidget);
    window.resize(1280, 720);
    LOG_INFO("Showing window...");
    window.show();
    LOG_INFO("Window shown, entering event loop");

    const char* url = "rtsp://192.168.42.116:25544/2026_06_08";
    if (argc > 1) url = argv[1];

    LOG_INFO("Opening stream: %s", url);
    bool ok = player.open(url);
    LOG_INFO("player.open() returned: %s", ok ? "true" : "false");
    LOG_INFO("Current state: %d", (int)player.state());

    LOG_INFO("Entering Qt event loop...");
    int ret = app.exec();
    LOG_INFO("Qt event loop exited with code: %d", ret);
    return ret;
}

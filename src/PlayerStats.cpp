#include "PlayerStats.h"
#include <ctime>
#include <sstream>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

void PlayerStats::initCsv(const std::string& path) {
    std::lock_guard<std::mutex> lock(m_csvMutex);
    m_csvFile.open(path, std::ios::app);
    if (!m_csvFile.is_open()) return;

    // session ID: YYYYMMDD_HHMMSS
    time_t now = time(nullptr);
    struct tm t;
#ifdef _WIN32
    localtime_s(&t, &now);
#else
    localtime_r(&now, &t);
#endif
    char buf[32];
    strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", &t);
    m_sessionId = buf;
}

void PlayerStats::writeCsvRow() {
    std::lock_guard<std::mutex> lock(m_csvMutex);
    if (!m_csvFile.is_open()) return;

    if (!m_csvHeaderWritten) {
        csvWrite("session,elapsed_s,decoded,rendered,dropped,"
                 "latenessAvg,latenessMax,skipBurst,reconnects,reconnectMs,"
                 "audioUnderrun,audioOverrun,vQueuePeak,aQueuePeak,"
                 "paintIntvAvg,paintIntvMax,paintLatAvg,paintLatMax,frameId");
        m_csvHeaderWritten = true;
    }

    int64_t dec = framesDecoded.load();
    int64_t ren = framesRendered.load();
    int64_t drp = framesDropped.load();
    int64_t lat = lastLatenessUs.load() / 1000;
    int64_t latMax = maxLatenessUs.load() / 1000;
    int64_t burst = renderSkipBurst.load();
    int rec = reconnectCount.load();
    int64_t recMs = totalReconnectMs.load();
    int au = audioUnderruns.load();
    int ao = audioOverruns.load();
    int vq = videoQueuePeakMs.load();
    int aq = audioQueuePeakMs.load();
    uint64_t fid = frameId.load();

    int64_t pivAvg = 0, pivMax = 0;
    int pc = paintIntervalCount.load();
    if (pc > 0) {
        pivAvg = paintIntervalSumUs.load() / pc / 1000;
        pivMax = paintIntervalMaxUs.load() / 1000;
    }
    int64_t plAvg = 0, plMax = 0;
    int pl = paintLatencyCount.load();
    if (pl > 0) {
        plAvg = paintLatencySumUs.load() / pl / 1000;
        plMax = paintLatencyMaxUs.load() / 1000;
    }

    // elapsed seconds since epoch (for relative time computation)
    int64_t elapsed = static_cast<int64_t>(time(nullptr));

    std::ostringstream ss;
    ss << m_sessionId << ',' << elapsed << ','
       << dec << ',' << ren << ',' << drp << ','
       << lat << ',' << latMax << ',' << burst << ','
       << rec << ',' << recMs << ','
       << au << ',' << ao << ','
       << vq << ',' << aq << ','
       << pivAvg << ',' << pivMax << ','
       << plAvg << ',' << plMax << ','
       << fid;

    csvWrite(ss.str());

    // Reset spikes for next interval
    maxLatenessUs = 0;
    renderSkipBurst = 0;
    videoQueuePeakMs = 0;
    audioQueuePeakMs = 0;
    audioUnderruns = 0;
    audioOverruns = 0;
    paintIntervalMinUs.store(0);
    paintIntervalMaxUs.store(0);
    paintIntervalSumUs.store(0);
    paintIntervalCount.store(0);
    paintLatencyMinUs.store(0);
    paintLatencyMaxUs.store(0);
    paintLatencySumUs.store(0);
    paintLatencyCount.store(0);
}

void PlayerStats::closeCsv() {
    std::lock_guard<std::mutex> lock(m_csvMutex);
    if (m_csvFile.is_open()) {
        m_csvFile.close();
    }
}

void PlayerStats::csvWrite(const std::string& line) {
    m_csvFile << line << '\n';
    m_csvFile.flush();
}

void PlayerStats::recordPaintInterval(int64_t us) {
    int cnt = paintIntervalCount.fetch_add(1) + 1;
    paintIntervalSumUs.fetch_add(us);
    int64_t cur = paintIntervalMinUs.load();
    if (cur == 0 || us < cur) paintIntervalMinUs.store(us);
    cur = paintIntervalMaxUs.load();
    if (us > cur) paintIntervalMaxUs.store(us);
}

void PlayerStats::recordPaintLatency(int64_t us) {
    int cnt = paintLatencyCount.fetch_add(1) + 1;
    paintLatencySumUs.fetch_add(us);
    int64_t cur = paintLatencyMinUs.load();
    if (cur == 0 || us < cur) paintLatencyMinUs.store(us);
    cur = paintLatencyMaxUs.load();
    if (us > cur) paintLatencyMaxUs.store(us);
}

void PlayerStats::recordQueueDepth(int videoMs, int audioMs) {
    int cur = videoQueuePeakMs.load();
    if (videoMs > cur) videoQueuePeakMs.store(videoMs);
    cur = audioQueuePeakMs.load();
    if (audioMs > cur) audioQueuePeakMs.store(audioMs);
}

void PlayerStats::recordSkipBurstEnd() {
    renderSkipBurst.store(0);
}

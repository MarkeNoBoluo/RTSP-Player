#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <mutex>
#include <fstream>

class PlayerStats {
public:
    // Frame counters
    std::atomic<int64_t> framesDecoded{0};
    std::atomic<int64_t> framesRendered{0};
    std::atomic<int64_t> framesDropped{0};
    std::atomic<int>     reconnectCount{0};
    std::atomic<int>     queueVideoDurationMs{0};

    // Timing (all in microseconds)
    std::atomic<int64_t> lastLatenessUs{0};
    std::atomic<int64_t> maxLatenessUs{0};
    std::atomic<int64_t> renderSkipBurst{0};
    std::atomic<int64_t> totalReconnectMs{0};
    std::atomic<int>     audioUnderruns{0};
    std::atomic<int>     audioOverruns{0};
    std::atomic<int>     videoQueuePeakMs{0};
    std::atomic<int>     audioQueuePeakMs{0};

    // Pacing (all in microseconds)
    std::atomic<int64_t> paintIntervalMinUs{0};
    std::atomic<int64_t> paintIntervalMaxUs{0};
    std::atomic<int64_t> paintIntervalSumUs{0};
    std::atomic<int>     paintIntervalCount{0};
    std::atomic<int64_t> paintLatencyMinUs{0};
    std::atomic<int64_t> paintLatencyMaxUs{0};
    std::atomic<int64_t> paintLatencySumUs{0};
    std::atomic<int>     paintLatencyCount{0};

    // Monotonic frame ID
    std::atomic<uint64_t> frameId{1};

    // Cross-thread timing
    std::atomic<int64_t> lastCommitUs{0};
    std::atomic<int64_t> reconnectStartUs{0};

    // CSV
    void initCsv(const std::string& path);
    void writeCsvRow();
    void closeCsv();

    // Recording helpers (called from worker threads)
    void recordPaintInterval(int64_t us);
    void recordPaintLatency(int64_t us);
    void recordQueueDepth(int videoMs, int audioMs);
    void recordSkipBurstEnd();

private:
    void csvWrite(const std::string& line);

    std::mutex  m_csvMutex;
    std::ofstream m_csvFile;
    std::string m_sessionId;
    bool m_csvHeaderWritten = false;
};

#include "PacketQueue.h"
#include <cassert>
#include "logger/Logger.h"

PacketQueue::PacketQueue() {}

PacketQueue::~PacketQueue() {
    flush();
}

void PacketQueue::init(AVRational timeBase, int capacityMs, const char* name) {
    assert(!m_initialized);
    m_initialized = true;
    m_capacityMs  = capacityMs;
    m_timeBaseUs  = av_q2d(timeBase) * 1000000.0;
    m_name        = name ? name : "";
    LOG_DEBUG("PacketQueue[%s] initialized: capacity=%dms, timeBase=%f", m_name.c_str(), capacityMs, m_timeBaseUs);
}

bool PacketQueue::push(AVPacket* pkt, int serial) {
    std::unique_lock<std::mutex> lock(m_mutex);

    int64_t durUs = 0;
    if (pkt->duration > 0) {
        durUs = static_cast<int64_t>(pkt->duration * m_timeBaseUs);
    }
    bool lowLatency = (m_capacityMs <= 66);

    while (!m_queue.empty() && m_totalDurationUs + durUs > m_capacityMs * 1000LL) {
        if (!m_abort && !lowLatency) {
            bool freed = m_cond.wait_for(lock, std::chrono::milliseconds(100),
                [this, durUs] { return m_totalDurationUs + durUs <= m_capacityMs * 1000LL || m_abort; });
            if (m_abort) return false;
            if (freed) break;
        } else if (m_abort) {
            return false;
        }

        auto it = m_queue.begin();
        bool dropped = false;

        for (auto iter = m_queue.begin(); iter != m_queue.end(); ++iter) {
            if (!(iter->pkt->flags & AV_PKT_FLAG_KEY)) {
                m_totalDurationUs -= iter->durationUs;
                av_packet_free(&iter->pkt);
                m_queue.erase(iter);
                dropped = true;
                LOG_WARN("PacketQueue[%s] dropping non-key frame, queue=%dms", m_name.c_str(), m_totalDurationUs / 1000);
                break;
            }
        }

        if (!dropped) {
            m_totalDurationUs -= m_queue.front().durationUs;
            av_packet_free(&m_queue.front().pkt);
            m_queue.pop_front();
            m_keyFrameDropped.store(true, std::memory_order_release);
            LOG_WARN("PacketQueue[%s] dropping oldest key frame, queue=%dms", m_name.c_str(), m_totalDurationUs / 1000);
        }
    }

    AVPacket* copy = av_packet_alloc();
    av_packet_move_ref(copy, pkt);

    m_queue.push_back({copy, durUs, serial});
    m_totalDurationUs += durUs;

    int curDurationMs = static_cast<int>(m_totalDurationUs / 1000);
    if (curDurationMs > m_peakDurationMs) m_peakDurationMs = curDurationMs;
    int curSize = static_cast<int>(m_queue.size());
    if (curSize > m_peakSize) m_peakSize = curSize;

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
    m_cond.notify_one();
    return true;
}

void PacketQueue::flush() {
    std::unique_lock<std::mutex> lock(m_mutex);
    LOG_INFO("PacketQueue[%s] flush: %d packets cleared", m_name.c_str(), (int)m_queue.size());
    while (!m_queue.empty()) {
        av_packet_free(&m_queue.front().pkt);
        m_queue.pop_front();
    }
    m_totalDurationUs = 0;
    m_peakDurationMs = 0;
    m_peakSize = 0;
    m_initialized = false;
    m_abort = false;
    m_cond.notify_all();
}

void PacketQueue::abort() {
    m_abort = true;
    m_cond.notify_all();
    LOG_INFO("PacketQueue[%s] aborted", m_name.c_str());
}

int PacketQueue::size() {
    std::unique_lock<std::mutex> lock(m_mutex);
    return static_cast<int>(m_queue.size());
}

int PacketQueue::durationMs() const {
    return m_totalDurationUs / 1000;
}

bool PacketQueue::checkKeyFrameDropped() {
    return m_keyFrameDropped.exchange(false, std::memory_order_acq_rel);
}

void PacketQueue::drainPeak(int& outDurationMs, int& outSize) {
    std::lock_guard<std::mutex> lock(m_mutex);
    outDurationMs = m_peakDurationMs;
    outSize = m_peakSize;
    m_peakDurationMs = 0;
    m_peakSize = 0;
}

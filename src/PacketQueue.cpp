#include "PacketQueue.h"
#include <cassert>
#include "logger/Logger.h"

PacketQueue::PacketQueue() {}

PacketQueue::~PacketQueue() {
    flush();
}

void PacketQueue::init(AVRational timeBase, int capacityMs) {
    assert(!m_initialized);
    m_initialized = true;
    m_capacityMs  = capacityMs;
    m_timeBaseUs  = av_q2d(timeBase) * 1000000.0;
    LOG_DEBUG("PacketQueue initialized: capacity=%dms, timeBase=%f", capacityMs, m_timeBaseUs);
}

bool PacketQueue::push(AVPacket* pkt, int serial) {
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
                LOG_WARN("PacketQueue dropping non-key frame, queue=%dms", m_totalDurationUs / 1000);
                break;
            }
        }

        if (!dropped) {
            m_totalDurationUs -= m_queue.front().durationUs;
            av_packet_free(&m_queue.front().pkt);
            m_queue.pop_front();
            LOG_WARN("PacketQueue dropping oldest key frame, queue=%dms", m_totalDurationUs / 1000);
        }
    }

    AVPacket* copy = av_packet_alloc();
    av_packet_move_ref(copy, pkt);

    m_queue.push_back({copy, durUs, serial});
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
    LOG_INFO("PacketQueue flush: %d packets cleared", (int)m_queue.size());
    while (!m_queue.empty()) {
        av_packet_free(&m_queue.front().pkt);
        m_queue.pop_front();
    }
    m_totalDurationUs = 0;
    m_initialized = false;
    m_abort = false;
    m_cond.notify_all();
}

void PacketQueue::abort() {
    m_abort = true;
    m_cond.notify_all();
    LOG_INFO("PacketQueue aborted");
}

int PacketQueue::size() {
    std::unique_lock<std::mutex> lock(m_mutex);
    return static_cast<int>(m_queue.size());
}

int PacketQueue::durationMs() const {
    return m_totalDurationUs / 1000;
}

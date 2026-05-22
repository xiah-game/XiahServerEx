#pragma once
#include <winsock2.h>
#include <mutex>

class SeqWindow {
public:
    SeqWindow() : m_maxSeq(0), m_window(0) {}

    bool Accept(uint32_t seq) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (seq == 0) return false;
        
        if (seq > m_maxSeq) {
            uint32_t diff = seq - m_maxSeq;
            if (diff >= 64) {
                m_window = 1; // Slide beyond window capacity
            } else {
                m_window <<= diff;
                m_window |= 1;
            }
            m_maxSeq = seq;
            return true;
        }

        uint32_t diff = m_maxSeq - seq;
        if (diff >= 64) return false; // Sequence too old

        uint64_t mask = (1ULL << diff);
        if (m_window & mask) return false; // Sequence already consumed (replay)

        m_window |= mask;
        return true;
    }

private:
    std::mutex m_mutex;
    uint32_t m_maxSeq;
    uint64_t m_window; // 64-bit sliding window bitmap
};

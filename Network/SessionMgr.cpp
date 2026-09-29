#include "SessionMgr.h"
#include "../ServerCore.h"

void SessionMgr::SetAccount(SOCKET s, const std::string& account) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_socketToAccount[s] = account;
}

std::string SessionMgr::GetAccount(SOCKET s) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_socketToAccount.count(s) ? m_socketToAccount[s] : "dustwj";
}

SOCKET SessionMgr::GetSocketByAccount(const std::string& account) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pair : m_socketToAccount) {
        if (pair.second == account) return pair.first;
    }
    return INVALID_SOCKET;
}

void SessionMgr::SetCharID(SOCKET s, DWORD charID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_socketToChar[s] = charID;
}

DWORD SessionMgr::GetCharID(SOCKET s) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_socketToChar.count(s) ? m_socketToChar[s] : 0;
}

SOCKET SessionMgr::GetSocketByCharID(DWORD charID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    for (auto& pair : m_socketToChar) {
        if (pair.second == charID) return pair.first;
    }
    return INVALID_SOCKET;
}

void SessionMgr::SetMapID(SOCKET s, DWORD mapID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_socketToMap[s] = mapID;
}

DWORD SessionMgr::GetMapID(SOCKET s) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_socketToMap.count(s) ? m_socketToMap[s] : 0;
}

void SessionMgr::AddConnection(SOCKET s) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sockets.push_back(s);
}

void SessionMgr::RemoveConnection(SOCKET s) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_sockets.erase(std::remove(m_sockets.begin(), m_sockets.end(), s), m_sockets.end());
    m_socketToChar.erase(s);
    m_socketToMap.erase(s);
    m_socketToAccount.erase(s);
    m_socketSecurity.erase(s);
}

void SessionMgr::ForEachSocket(std::function<void(SOCKET)> fn) {
    std::vector<SOCKET> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_sockets;
    }
    for (SOCKET s : snapshot) {
        fn(s);
    }
}

void SessionMgr::ForEachSocketInMap(DWORD mapID, std::function<void(SOCKET, DWORD charID)> fn) {
    // Build snapshot under lock, then call fn outside lock to avoid deadlock
    std::vector<std::pair<SOCKET, DWORD>> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (SOCKET s : m_sockets) {
            if (m_socketToMap.count(s) && m_socketToMap[s] == mapID) {
                DWORD charID = m_socketToChar.count(s) ? m_socketToChar[s] : 0;
                targets.push_back({s, charID});
            }
        }
    }
    for (auto& p : targets) {
        fn(p.first, p.second);
    }
}

void SessionMgr::BroadcastToMap(DWORD mapID, const std::vector<BYTE>& packet) {
    std::vector<SOCKET> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (SOCKET s : m_sockets) {
            if (m_socketToMap.count(s) && m_socketToMap[s] == mapID) {
                targets.push_back(s);
            }
        }
    }
    for (SOCKET s : targets) {
        SafeSend(s, (const char*)packet.data(), (int)packet.size(), 0);
    }
}

void SessionMgr::SendToObjectIDs(const std::unordered_set<DWORD>& objectIDs, DWORD mapID,
                                  const std::vector<BYTE>& packet, SOCKET excludeSocket) {
    std::vector<SOCKET> targets;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (SOCKET s : m_sockets) {
            if (s == excludeSocket) continue;
            if (m_socketToMap.count(s) && m_socketToMap[s] == mapID) {
                DWORD charID = m_socketToChar.count(s) ? m_socketToChar[s] : 0;
                DWORD objID = charID + 400000000;
                if (objectIDs.count(objID)) {
                    targets.push_back(s);
                }
            }
        }
    }
    for (SOCKET s : targets) {
        SafeSend(s, (const char*)packet.data(), (int)packet.size(), 0);
    }
}

void SessionMgr::BroadcastToAll(const std::vector<BYTE>& packet) {
    std::vector<SOCKET> snapshot;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        snapshot = m_sockets;
    }
    for (SOCKET s : snapshot) {
        SafeSend(s, (const char*)packet.data(), (int)packet.size(), 0);
    }
}

std::vector<SOCKET> SessionMgr::GetAllSockets() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_sockets;
}

void SessionMgr::SetTicketId(SOCKET s, DWORD ticketId) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_socketSecurity[s].ticketId = ticketId;
}

DWORD SessionMgr::GetTicketId(SOCKET s) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_socketSecurity.count(s) ? m_socketSecurity[s].ticketId : 0;
}

bool SessionMgr::AcceptSequence(SOCKET s, uint32_t seq) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (seq == 0) return false;
    
    auto& sec = m_socketSecurity[s];
    if (sec.maxSeq == 0) {
        // Initialize sequence tracking on first accepted sequence
        sec.maxSeq = seq;
        sec.seqWindow = 1;
        return true;
    }
    
    if (seq > sec.maxSeq) {
        uint32_t diff = seq - sec.maxSeq;
        if (diff >= 64) {
            sec.seqWindow = 1; // Slide beyond window capacity
        } else {
            sec.seqWindow <<= diff;
            sec.seqWindow |= 1;
        }
        sec.maxSeq = seq;
        return true;
    }
    
    uint32_t diff = sec.maxSeq - seq;
    if (diff >= 64) return false; // Sequence too old
    
    uint64_t mask = (1ULL << diff);
    if (sec.seqWindow & mask) return false; // Replay!
    
    sec.seqWindow |= mask;
    return true;
}

bool SessionMgr::ValidateTimestamp(SOCKET s, uint32_t timestamp, uint32_t now) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto& sec = m_socketSecurity[s];
    if (!sec.offsetInitialized) {
        sec.clockOffset = (int64_t)now - (int64_t)timestamp;
        sec.offsetInitialized = true;
        return true;
    }
    int64_t expectedNow = (int64_t)timestamp + sec.clockOffset;
    int64_t drift = expectedNow - (int64_t)now;
    if (drift < -300 || drift > 300) {
        return false;
    }
    return true;
}


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

#include "SessionMgr.h"
#include "PacketRouter.h"

void SessionMgr::SetAccount(SOCKET s, const std::string& account) {
    std::lock_guard<std::mutex> lock(m_mutex);
    socketToAccount[s] = account;
}
std::string SessionMgr::GetAccount(SOCKET s) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return socketToAccount.count(s) ? socketToAccount[s] : "dustwj";
}

void SessionMgr::SetCharID(SOCKET s, DWORD charID) {
    std::lock_guard<std::mutex> lock(g_SocketsMutex);
    g_SocketToChar[s] = charID;
}
DWORD SessionMgr::GetCharID(SOCKET s) {
    std::lock_guard<std::mutex> lock(g_SocketsMutex);
    return g_SocketToChar.count(s) ? g_SocketToChar[s] : 0;
}
SOCKET SessionMgr::GetSocketByCharID(DWORD charID) {
    std::lock_guard<std::mutex> lock(g_SocketsMutex);
    for (auto& pair : g_SocketToChar) {
        if (pair.second == charID) return pair.first;
    }
    return INVALID_SOCKET;
}

void SessionMgr::SetMapID(SOCKET s, DWORD mapID) {
    std::lock_guard<std::mutex> lock(g_SocketsMutex);
    g_SocketToMap[s] = mapID;
}
DWORD SessionMgr::GetMapID(SOCKET s) {
    std::lock_guard<std::mutex> lock(g_SocketsMutex);
    return g_SocketToMap.count(s) ? g_SocketToMap[s] : 0;
}

void SessionMgr::RemoveSession(SOCKET s) {
    std::lock_guard<std::mutex> lock(m_mutex);
    socketToAccount.erase(s);
    // Note: The global map erasures are still handled in UnitServer.cpp's disconnect handler for now.
}

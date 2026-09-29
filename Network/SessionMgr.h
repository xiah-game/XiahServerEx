#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <map>
#include <vector>
#include <mutex>
#include <functional>
#include <unordered_set>

class SessionMgr {
public:
    static SessionMgr& GetInstance() { static SessionMgr instance; return instance; }

    // --- Account ---
    void SetAccount(SOCKET s, const std::string& account);
    std::string GetAccount(SOCKET s);
    SOCKET GetSocketByAccount(const std::string& account);

    // --- CharID ---
    void SetCharID(SOCKET s, DWORD charID);
    DWORD GetCharID(SOCKET s);
    SOCKET GetSocketByCharID(DWORD charID);

    // --- MapID ---
    void SetMapID(SOCKET s, DWORD mapID);
    DWORD GetMapID(SOCKET s);

    // --- Connection lifecycle (replaces direct g_UnitSockets manipulation) ---
    void AddConnection(SOCKET s);
    void RemoveConnection(SOCKET s);  // Removes socket + charID + mapID + account

    // --- Iteration helpers (hold lock internally, callback must NOT call SessionMgr) ---
    // Calls fn for each connected socket. Safe snapshot-based iteration.
    void ForEachSocket(std::function<void(SOCKET)> fn);

    // Calls fn for each socket on the given map, passing socket and its charID.
    void ForEachSocketInMap(DWORD mapID, std::function<void(SOCKET, DWORD charID)> fn);

    // --- Broadcast helpers ---
    // Send packet to all sockets on a given map
    void BroadcastToMap(DWORD mapID, const std::vector<BYTE>& packet);

    // Send packet to sockets whose ObjectID (charID+400M) is in the set, on a given map
    void SendToObjectIDs(const std::unordered_set<DWORD>& objectIDs, DWORD mapID,
                         const std::vector<BYTE>& packet, SOCKET excludeSocket = INVALID_SOCKET);

    // Send packet to ALL connected sockets
    void BroadcastToAll(const std::vector<BYTE>& packet);

    // --- Snapshot ---
    // Returns a copy of all sockets (for use outside lock)
    std::vector<SOCKET> GetAllSockets();

    // --- Security Verification ---
    void SetTicketId(SOCKET s, DWORD ticketId);
    DWORD GetTicketId(SOCKET s);
    bool AcceptSequence(SOCKET s, uint32_t seq);
    bool ValidateTimestamp(SOCKET s, uint32_t timestamp, uint32_t now);

private:
    SessionMgr() {}

    struct SocketSecurityState {
        DWORD ticketId = 0;
        uint32_t maxSeq = 0;
        uint64_t seqWindow = 0;
        int64_t clockOffset = 0;
        bool offsetInitialized = false;
    };

    std::mutex m_mutex;
    std::vector<SOCKET> m_sockets;              // replaces g_UnitSockets
    std::map<SOCKET, DWORD> m_socketToChar;     // replaces g_SocketToChar
    std::map<SOCKET, DWORD> m_socketToMap;      // replaces g_SocketToMap
    std::map<SOCKET, std::string> m_socketToAccount;
    std::map<SOCKET, SocketSecurityState> m_socketSecurity;
};

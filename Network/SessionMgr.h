#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <map>
#include <mutex>

class SessionMgr {
public:
    static SessionMgr& GetInstance() { static SessionMgr instance; return instance; }
    
    void SetAccount(SOCKET s, const std::string& account);
    std::string GetAccount(SOCKET s);
    
    void SetCharID(SOCKET s, DWORD charID);
    DWORD GetCharID(SOCKET s);
    SOCKET GetSocketByCharID(DWORD charID);
    
    void SetMapID(SOCKET s, DWORD mapID);
    DWORD GetMapID(SOCKET s);
    
    void RemoveSession(SOCKET s);

private:
    std::mutex m_mutex;
    std::map<SOCKET, std::string> socketToAccount;
};

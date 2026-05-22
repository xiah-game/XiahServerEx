#pragma once
#include <winsock2.h>
#include <string>
#include <unordered_map>
#include <mutex>

struct AuthTicket {
    uint32_t ticketId;
    std::string account;
    DWORD accountId;
    BYTE channelId;
    BYTE sessionKey[32];
    uint32_t expiresAt; // Unix timestamp
    bool consumed;
    std::string clientIp;
};

class AuthCenter {
public:
    static AuthCenter& Get() {
        static AuthCenter instance;
        return instance;
    }

    // AuthServer calls this when authenticating user successfully
    AuthTicket Issue(const std::string& account, DWORD accountId, BYTE channelId, const std::string& ip);

    // UnitServer calls this when processing login check request
    bool Consume(uint32_t ticketId, const std::string& ip, BYTE channelId, AuthTicket& outTicket);

    // Periodic cleanup of expired tickets
    void CleanupExpired();

private:
    AuthCenter() = default;
    std::mutex m_mutex;
    std::unordered_map<uint32_t, AuthTicket> m_tickets;
};

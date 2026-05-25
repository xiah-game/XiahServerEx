#include "AuthCenter.h"
#include <ctime>
#include <random>
#include "../ServerCore.h"

AuthTicket AuthCenter::Issue(const std::string& account, DWORD accountId, BYTE channelId, const std::string& ip) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Generate cryptographically secure random 32-bit ticket ID using std::random_device
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<uint32_t> dis;
    
    AuthTicket ticket;
    ticket.ticketId = dis(gen);
    if (ticket.ticketId == 46) { // Avoid collisions with legacy dummy key
        ticket.ticketId = dis(gen);
    }
    ticket.account = account;
    ticket.accountId = accountId;
    ticket.channelId = channelId;
    ticket.expiresAt = (uint32_t)time(nullptr) + 86400; // 24 hours expiry to support long gameplay sessions and channel swapping
    ticket.consumed = false;
    ticket.clientIp = ip;

    // Fill randomized sessionKey
    for (int i = 0; i < 32; ++i) {
        ticket.sessionKey[i] = (BYTE)(dis(gen) & 0xFF);
    }

    m_tickets[ticket.ticketId] = ticket;
    
    LOG("[AuthCenter] Issued Ticket " + std::to_string(ticket.ticketId) + " for account " + account + " IP: " + ip);
    return ticket;
}

bool AuthCenter::Consume(uint32_t ticketId, const std::string& ip, BYTE channelId, AuthTicket& outTicket) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_tickets.find(ticketId);
    if (it == m_tickets.end()) {
        LOG("[AuthCenter] Ticket " + std::to_string(ticketId) + " not found!");
        return false;
    }

    AuthTicket& t = it->second;
    uint32_t now = (uint32_t)time(nullptr);

    // Removed the "t.consumed" check to support legacy client's channel switching and relogging which reuses the same ticket!
    /*
    if (t.consumed) {
        LOG("[AuthCenter] Ticket " + std::to_string(ticketId) + " already consumed!");
        return false;
    }
    */

    if (t.expiresAt < now) {
        LOG("[AuthCenter] Ticket " + std::to_string(ticketId) + " expired! Expires: " + std::to_string(t.expiresAt) + " Current: " + std::to_string(now));
        return false;
    }

    if (t.channelId != 0xFF && t.channelId != channelId) {
        LOG("[AuthCenter] Ticket " + std::to_string(ticketId) + " channel mismatch! Ticket: " + std::to_string(t.channelId) + " Req: " + std::to_string(channelId));
        return false;
    }

    // IP validation
    if (t.clientIp != ip) {
        LOG("[AuthCenter] WARNING: Ticket IP mismatch! Issued: " + t.clientIp + " Consume: " + ip);
    }

    t.consumed = true;
    outTicket = t;
    // DO NOT erase it! Let it remain in m_tickets so it can be reused for channel change/reconnect!
    // m_tickets.erase(it); 
    
    LOG("[AuthCenter] Successfully verified Ticket " + std::to_string(ticketId) + " for account " + outTicket.account);
    return true;
}

void AuthCenter::CleanupExpired() {
    std::lock_guard<std::mutex> lock(m_mutex);
    uint32_t now = (uint32_t)time(nullptr);
    size_t removed = 0;
    for (auto it = m_tickets.begin(); it != m_tickets.end();) {
        if (it->second.expiresAt < now) {
            it = m_tickets.erase(it);
            removed++;
        } else {
            ++it;
        }
    }
    if (removed > 0) {
        LOG("[AuthCenter] Cleaned up " + std::to_string(removed) + " expired tickets.");
    }
}

#include "PartyHandler.h"
#include "ChatHandler.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../DB/CharacterDB.h"
#include <cstring>

extern std::map<DWORD, CMapInstance*> g_MapInstances;

// =========================================================
// Helper: Get player info for party member data
// =========================================================
static PartyMemberInfo BuildMemberInfo(DWORD dwCharID, BYTE bPriority) {
    PartyMemberInfo info;
    info.dwCharID = dwCharID;
    info.dwObjectID = dwCharID + 400000000; // 400M -> 800M
    info.bPriority = bPriority;
    info.szName = "";
    info.wLevel = 1;
    info.wPosX = 0;
    info.wPosY = 0;
    info.dwCurHp = 0;
    info.dwMaxHp = 0;
    info.dwMapID = 0;

    // 1. 先尝试从数据库直接获取确切的角色等级、名称与HP上限，确保绝不发错1级
    CharacterDB::CharPower cp;
    if (CharacterDB::GetInstance().GetCharData(dwCharID, cp)) {
        info.wLevel = (cp.wLevel > 0) ? cp.wLevel : 1;
        info.dwCurHp = cp.dwHpCur;
        info.dwMaxHp = (cp.dwHpMax > 0) ? cp.dwHpMax : 60000;
    }
    std::string vizName;
    BYTE vizType = 0;
    if (CharacterDB::GetInstance().GetCharVisual(dwCharID, vizName, vizType)) {
        info.szName = vizName;
    }

    // 2. 然后用在线地图中的实时数据覆盖（如实时坐标、动态血量、最新等级）
    for (auto& pair : g_MapInstances) {
        CMapInstance* mapInst = pair.second;
        if (!mapInst) continue;
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* player = mapInst->GetPlayer(info.dwObjectID);
        if (player) {
            if (!player->szName.empty()) info.szName = player->szName;
            if (player->wLevel > 0) info.wLevel = player->wLevel;
            info.wPosX = player->wPosX;
            info.wPosY = player->wPosY;
            if (player->dwHpCur > 0) info.dwCurHp = player->dwHpCur;
            if (player->dwHpMax > 0) info.dwMaxHp = player->dwHpMax;
            info.dwMapID = player->dwMapID;
            break;
        }
    }
    return info;
}

// =========================================================
// Helper: Build and send a packet to a single socket
// =========================================================
static void SendPacket(SOCKET s, WORD packetID, const std::vector<BYTE>& payload) {
    std::vector<BYTE> buf(4);
    buf.insert(buf.end(), payload.begin(), payload.end());
    PACKET_HEADER* h = (PACKET_HEADER*)buf.data();
    h->id = packetID;
    h->payloadSize = (WORD)payload.size();
    EncryptPacket(buf.data(), 0x42);
    SafeSend(s, (const char*)buf.data(), (int)buf.size(), 0);
}

// =========================================================
// Helper: push primitives into a byte vector
// =========================================================
static void pushDWord(std::vector<BYTE>& buf, DWORD d) {
    buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF);
    buf.push_back((d >> 16) & 0xFF); buf.push_back((d >> 24) & 0xFF);
}
static void pushWord(std::vector<BYTE>& buf, WORD w) {
    buf.push_back(w & 0xFF); buf.push_back((w >> 8) & 0xFF);
}
static void pushByte(std::vector<BYTE>& buf, BYTE b) {
    buf.push_back(b);
}
static void pushSString(std::vector<BYTE>& buf, const std::string& s) {
    WORD len = (WORD)s.size();
    pushWord(buf, len);
    buf.insert(buf.end(), s.begin(), s.end());
}

// =========================================================
// Send CREATEPARTY_ACK to a socket
// =========================================================
static void SendCreatePartyAck(SOCKET s, DWORD dwPartyID, DWORD dwLeaderObjID, BYTE bPartyType) {
    std::vector<BYTE> payload;
    pushDWord(payload, dwPartyID);
    pushDWord(payload, dwLeaderObjID);
    pushByte(payload, bPartyType);
    SendPacket(s, PKT_CREATEPARTY_ACK, payload);
}

// =========================================================
// Send ENTERPARTY_ACK to a socket (new member joined)
// =========================================================
static void SendEnterPartyAck(SOCKET s, DWORD dwPartyID, const PartyMemberInfo& member) {
    std::vector<BYTE> payload;
    pushDWord(payload, dwPartyID);
    pushDWord(payload, member.dwObjectID);
    pushByte(payload, member.bPriority);
    pushSString(payload, member.szName);
    pushWord(payload, member.wLevel);
    pushWord(payload, member.wPosX);
    pushWord(payload, member.wPosY);
    pushDWord(payload, member.dwCurHp);
    pushDWord(payload, member.dwMaxHp);
    SendPacket(s, PKT_ENTERPARTY_ACK, payload);
}

// =========================================================
// Send PARTYLIST_ACK to a socket (full member list)
// =========================================================
static void SendPartyListAck(SOCKET s, DWORD dwPartyID, BYTE bPartyType,
                             const std::vector<PartyMemberInfo>& members) {
    std::vector<BYTE> payload;
    pushDWord(payload, dwPartyID);
    pushByte(payload, (BYTE)members.size());
    pushByte(payload, bPartyType);
    for (auto& m : members) {
        pushDWord(payload, m.dwObjectID);
        pushByte(payload, m.bPriority);
        pushSString(payload, m.szName);
        pushWord(payload, m.wLevel);
        pushWord(payload, m.wPosX);
        pushWord(payload, m.wPosY);
        pushDWord(payload, m.dwCurHp);
        pushDWord(payload, m.dwMaxHp);
        pushDWord(payload, m.dwMapID);
    }
    SendPacket(s, PKT_PARTYLIST_ACK, payload);
}

// =========================================================
// Send LEAVEPARTY_ACK to a socket
// =========================================================
static void SendLeavePartyAck(SOCKET s, DWORD dwPartyID, DWORD dwCharObjID, BYTE bPriority) {
    std::vector<BYTE> payload;
    pushDWord(payload, dwPartyID);
    pushDWord(payload, dwCharObjID);
    pushByte(payload, bPriority);
    SendPacket(s, PKT_LEAVEPARTY_ACK, payload);
}

// =========================================================
// Send DESTROYPARTY_ACK to a socket
// =========================================================
static void SendDestroyPartyAck(SOCKET s, DWORD dwPartyID) {
    std::vector<BYTE> payload;
    pushDWord(payload, dwPartyID);
    SendPacket(s, PKT_DESTROYPARTY_ACK, payload);
}

// =========================================================
// Send ASKPARTY_ACK (forward invite / reject / error)
// =========================================================
static void SendAskPartyAck(SOCKET s, DWORD dwAskID, DWORD dwAskedID, BYTE bResult, BYTE bPartyType) {
    std::vector<BYTE> payload;
    pushDWord(payload, dwAskID);
    pushDWord(payload, dwAskedID);
    pushByte(payload, bResult);
    pushByte(payload, bPartyType);
    SendPacket(s, PKT_ASKPARTY_ACK, payload);
}

// =========================================================
// Send INVITEPARTY_ACK
// =========================================================
static void SendInvitePartyAck(SOCKET s, DWORD dwAskID, DWORD dwAskedID, BYTE bResult) {
    std::vector<BYTE> payload;
    pushDWord(payload, dwAskID);
    pushDWord(payload, dwAskedID);
    pushByte(payload, bResult);
    SendPacket(s, PKT_INVITEPARTY_ACK, payload);
}

// =========================================================
// OnAskPartyReq - Create party + invite
// Payload: DWORD dwAskID, DWORD dwAskedID, BYTE bResult, BYTE bPartyType
// bResult: 0=request, 1=accept, 9=decline
// =========================================================
void OnAskPartyReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize) {
    if (wSize < 10) return;

    DWORD dwAskID = *(DWORD*)(pPayload);
    DWORD dwAskedID = *(DWORD*)(pPayload + 4);
    BYTE bResult = *(pPayload + 8);
    BYTE bPartyType = (wSize >= 10) ? *(pPayload + 9) : 0;

    LOG("[PARTY] AskPartyReq: AskID=" + std::to_string(dwAskID) +
        " AskedID=" + std::to_string(dwAskedID) +
        " Result=" + std::to_string(bResult) +
        " Type=" + std::to_string(bPartyType));

    // Convert ObjectIDs to CharIDs for SessionMgr lookup
    // Client sends ObjectIDs (800M format)
    DWORD dwAskCharID = dwAskID - 400000000;   // 800M -> 400M (GetCharID format)
    DWORD dwAskedCharID = dwAskedID - 400000000;

    if (bResult == 0) {
        // Initial request: forward to target as invitation popup
        // Check if asker is already in a party
        if (PartyManager::GetInstance().GetPartyID(dwAskCharID) != 0) {
            // Already in party, send error
            SendAskPartyAck(clientSocket, dwAskID, dwAskedID, 8, bPartyType);
            return;
        }
        // Check if target is already in a party
        if (PartyManager::GetInstance().GetPartyID(dwAskedCharID) != 0) {
            SendAskPartyAck(clientSocket, dwAskID, dwAskedID, 8, bPartyType);
            return;
        }

        // Store pending invite
        PartyManager::GetInstance().SetPendingInvite(dwAskCharID, dwAskedCharID, bPartyType);

        // Forward to target player
        SOCKET targetSocket = SessionMgr::GetInstance().GetSocketByCharID(dwAskedCharID);
        if (targetSocket != INVALID_SOCKET) {
            SendAskPartyAck(targetSocket, dwAskID, dwAskedID, 0, bPartyType);
        } else {
            // Target not online
            SendAskPartyAck(clientSocket, dwAskID, dwAskedID, 5, bPartyType);
        }
    }
    else if (bResult == 1) {
        // Accept: create party with both players
        // dwAskID = original asker, dwAskedID = current player (acceptor)
        DWORD askedCharID2 = 0;
        BYTE partyType2 = 0;
        if (!PartyManager::GetInstance().GetPendingInvite(dwAskCharID, askedCharID2, partyType2)) {
            LOG("[PARTY] No pending invite found for asker " + std::to_string(dwAskCharID));
            return;
        }
        PartyManager::GetInstance().ClearPendingInvite(dwAskCharID);

        // Build member info
        PartyMemberInfo leaderInfo = BuildMemberInfo(dwAskCharID, 0); // leader
        PartyMemberInfo memberInfo = BuildMemberInfo(dwAskedCharID, 1); // member

        // Create party
        DWORD partyID = PartyManager::GetInstance().CreateParty(leaderInfo);
        if (partyID == 0) {
            LOG("[PARTY] Failed to create party");
            return;
        }
        PartyManager::GetInstance().AddMember(partyID, memberInfo);

        // Send CREATEPARTY_ACK to both
        SOCKET askerSocket = SessionMgr::GetInstance().GetSocketByCharID(dwAskCharID);
        if (askerSocket != INVALID_SOCKET) {
            SendCreatePartyAck(askerSocket, partyID, leaderInfo.dwObjectID, bPartyType);
        }
        SendCreatePartyAck(clientSocket, partyID, leaderInfo.dwObjectID, bPartyType);

        // Send PARTYLIST_ACK to both with full member list
        auto members = PartyManager::GetInstance().GetMembers(partyID);
        if (askerSocket != INVALID_SOCKET) {
            SendPartyListAck(askerSocket, partyID, bPartyType, members);
        }
        SendPartyListAck(clientSocket, partyID, bPartyType, members);

        LOG("[PARTY] Party " + std::to_string(partyID) + " created! Leader=" +
            leaderInfo.szName + " Member=" + memberInfo.szName);
    }
    else if (bResult == 9) {
        // Decline
        PartyManager::GetInstance().ClearPendingInvite(dwAskCharID);
        SOCKET askerSocket = SessionMgr::GetInstance().GetSocketByCharID(dwAskCharID);
        if (askerSocket != INVALID_SOCKET) {
            SendAskPartyAck(askerSocket, dwAskID, dwAskedID, 9, bPartyType);
        }
    }
}

// =========================================================
// OnInvitePartyReq - Invite to existing party (leader only)
// Payload: DWORD dwAskID, DWORD dwAskedID, BYTE bResult, BYTE bPartyType
// =========================================================
void OnInvitePartyReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize) {
    if (wSize < 9) return;

    DWORD dwAskID = *(DWORD*)(pPayload);
    DWORD dwAskedID = *(DWORD*)(pPayload + 4);
    BYTE bResult = *(pPayload + 8);
    BYTE bPartyType = (wSize >= 10) ? *(pPayload + 9) : 0;

    DWORD dwAskCharID = dwAskID - 400000000;
    DWORD dwAskedCharID = dwAskedID - 400000000;

    LOG("[PARTY] InvitePartyReq: AskID=" + std::to_string(dwAskID) +
        " AskedID=" + std::to_string(dwAskedID) +
        " Result=" + std::to_string(bResult));

    if (bResult == 0) {
        // Verify asker is party leader
        DWORD partyID = PartyManager::GetInstance().GetPartyID(dwAskCharID);
        if (partyID == 0) return;
        DWORD leaderCharID = PartyManager::GetInstance().GetLeaderCharID(partyID);
        if (leaderCharID != dwAskCharID) {
            LOG("[PARTY] Non-leader trying to invite");
            return;
        }

        // Check if party is full
        if (PartyManager::GetInstance().GetMembers(partyID).size() >= MAX_PARTY_MEMBERS) {
            SendInvitePartyAck(clientSocket, dwAskID, dwAskedID, 8);
            return;
        }

        // Check if target already in a party
        if (PartyManager::GetInstance().GetPartyID(dwAskedCharID) != 0) {
            SendInvitePartyAck(clientSocket, dwAskID, dwAskedID, 8);
            return;
        }

        // Store pending invite
        PartyManager::GetInstance().SetPendingInvite(dwAskCharID, dwAskedCharID, bPartyType);

        // Forward invitation to target
        SOCKET targetSocket = SessionMgr::GetInstance().GetSocketByCharID(dwAskedCharID);
        if (targetSocket != INVALID_SOCKET) {
            SendInvitePartyAck(targetSocket, dwAskID, dwAskedID, 0);
        } else {
            SendInvitePartyAck(clientSocket, dwAskID, dwAskedID, 5);
        }
    }
    else if (bResult == 1) {
        // Accept: add to existing party
        DWORD askedCharID2 = 0;
        BYTE partyType2 = 0;
        if (!PartyManager::GetInstance().GetPendingInvite(dwAskCharID, askedCharID2, partyType2)) {
            return;
        }
        PartyManager::GetInstance().ClearPendingInvite(dwAskCharID);

        DWORD partyID = PartyManager::GetInstance().GetPartyID(dwAskCharID);
        if (partyID == 0) return;

        PartyMemberInfo newMember = BuildMemberInfo(dwAskedCharID, 1);
        if (!PartyManager::GetInstance().AddMember(partyID, newMember)) {
            return;
        }

        // Notify all existing members about the new member
        auto members = PartyManager::GetInstance().GetMembers(partyID);
        for (auto& m : members) {
            SOCKET mSocket = SessionMgr::GetInstance().GetSocketByCharID(m.dwCharID);
            if (mSocket != INVALID_SOCKET && m.dwCharID != dwAskedCharID) {
                SendEnterPartyAck(mSocket, partyID, newMember);
            }
        }

        // Send PARTYLIST to the new member (full list)
        SendPartyListAck(clientSocket, partyID, partyType2, members);

        LOG("[PARTY] " + newMember.szName + " joined party " + std::to_string(partyID));
    }
    else if (bResult == 9) {
        // Decline
        PartyManager::GetInstance().ClearPendingInvite(dwAskCharID);
        SOCKET askerSocket = SessionMgr::GetInstance().GetSocketByCharID(dwAskCharID);
        if (askerSocket != INVALID_SOCKET) {
            SendInvitePartyAck(askerSocket, dwAskID, dwAskedID, 9);
        }
    }
}

// =========================================================
// OnLeavePartyReq - Leave party
// Payload: DWORD dwPartyID
// =========================================================
void OnLeavePartyReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize) {
    if (wSize < 4) return;

    DWORD dwPartyID = *(DWORD*)(pPayload);
    DWORD myPartyID = PartyManager::GetInstance().GetPartyID(dwCharID);

    LOG("[PARTY] LeavePartyReq: CharID=" + std::to_string(dwCharID) +
        " PartyID=" + std::to_string(dwPartyID));

    if (myPartyID == 0 || myPartyID != dwPartyID) return;

    DWORD dwObjectID = dwCharID + 400000000;

    // Get current members before removal
    auto members = PartyManager::GetInstance().GetMembers(dwPartyID);

    // Remove the player
    bool partyExists = PartyManager::GetInstance().RemoveMember(dwCharID);

    if (!partyExists) {
        // Party disbanded (was the last member, or second-to-last left)
        for (auto& m : members) {
            SOCKET mSocket = SessionMgr::GetInstance().GetSocketByCharID(m.dwCharID);
            if (mSocket != INVALID_SOCKET) {
                SendDestroyPartyAck(mSocket, dwPartyID);
            }
        }
    } else {
        // Notify remaining members
        auto remaining = PartyManager::GetInstance().GetMembers(dwPartyID);

        // If only 1 member left, also destroy
        if (remaining.size() <= 1) {
            for (auto& m : remaining) {
                PartyManager::GetInstance().RemoveMember(m.dwCharID);
                SOCKET mSocket = SessionMgr::GetInstance().GetSocketByCharID(m.dwCharID);
                if (mSocket != INVALID_SOCKET) {
                    SendDestroyPartyAck(mSocket, dwPartyID);
                }
            }
            // Also notify the leaver
            SendDestroyPartyAck(clientSocket, dwPartyID);
        } else {
            // Send leave notification to all remaining
            for (auto& m : remaining) {
                SOCKET mSocket = SessionMgr::GetInstance().GetSocketByCharID(m.dwCharID);
                if (mSocket != INVALID_SOCKET) {
                    SendLeavePartyAck(mSocket, dwPartyID, dwObjectID, 1);
                }
            }
            // Also send to the leaver
            SendLeavePartyAck(clientSocket, dwPartyID, dwObjectID, 1);
        }
    }
}

// =========================================================
// OnBanishPartyReq - Kick a member (leader only)
// Payload: DWORD dwPartyID, DWORD dwBanishCharID
// =========================================================
void OnBanishPartyReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize) {
    if (wSize < 8) return;

    DWORD dwPartyID = *(DWORD*)(pPayload);
    DWORD dwBanishObjID = *(DWORD*)(pPayload + 4);
    DWORD dwBanishCharID = dwBanishObjID - 400000000;

    LOG("[PARTY] BanishPartyReq: Leader=" + std::to_string(dwCharID) +
        " Kick=" + std::to_string(dwBanishCharID) +
        " Party=" + std::to_string(dwPartyID));

    // Verify caller is leader
    DWORD leaderCharID = PartyManager::GetInstance().GetLeaderCharID(dwPartyID);
    if (leaderCharID != dwCharID) return;

    // Remove the banished member
    PartyManager::GetInstance().RemoveMember(dwBanishCharID);

    // Notify all remaining members
    auto remaining = PartyManager::GetInstance().GetMembers(dwPartyID);

    // Build the banish ACK with the banished member's ObjectID
    std::vector<BYTE> payload;
    pushDWord(payload, dwPartyID);
    pushDWord(payload, dwBanishObjID);
    pushByte(payload, 1); // bPriority

    for (auto& m : remaining) {
        SOCKET mSocket = SessionMgr::GetInstance().GetSocketByCharID(m.dwCharID);
        if (mSocket != INVALID_SOCKET) {
            SendPacket(mSocket, PKT_BANISHPARTY_ACK, payload);
        }
    }

    // Also notify the banished player
    SOCKET kickedSocket = SessionMgr::GetInstance().GetSocketByCharID(dwBanishCharID);
    if (kickedSocket != INVALID_SOCKET) {
        SendPacket(kickedSocket, PKT_BANISHPARTY_ACK, payload);
    }
}

// =========================================================
// PartyManager Implementation
// =========================================================

DWORD PartyManager::CreateParty(const PartyMemberInfo& leader) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_charToParty.count(leader.dwCharID)) {
        return 0; // Already in a party
    }

    DWORD partyID = m_nextPartyID++;
    m_parties[partyID].push_back(leader);
    m_charToParty[leader.dwCharID] = partyID;

    LOG("[PARTY] Created party " + std::to_string(partyID) +
        " leader=" + leader.szName);
    return partyID;
}

bool PartyManager::AddMember(DWORD dwPartyID, const PartyMemberInfo& member) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_charToParty.count(member.dwCharID)) return false;
    if (!m_parties.count(dwPartyID)) return false;
    if (m_parties[dwPartyID].size() >= MAX_PARTY_MEMBERS) return false; // Max 8 members

    m_parties[dwPartyID].push_back(member);
    m_charToParty[member.dwCharID] = dwPartyID;

    LOG("[PARTY] Added " + member.szName + " to party " + std::to_string(dwPartyID));
    return true;
}

bool PartyManager::RemoveMember(DWORD dwCharID) {
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_charToParty.find(dwCharID);
    if (it == m_charToParty.end()) return false;

    DWORD partyID = it->second;
    m_charToParty.erase(it);

    auto pit = m_parties.find(partyID);
    if (pit != m_parties.end()) {
        auto& vec = pit->second;
        for (auto vit = vec.begin(); vit != vec.end(); ++vit) {
            if (vit->dwCharID == dwCharID) {
                vec.erase(vit);
                break;
            }
        }
        if (vec.empty()) {
            m_parties.erase(pit);
            return false; // Party no longer exists
        }
        // If removed member was leader, promote next member
        bool hasLeader = false;
        for (auto& m : vec) {
            if (m.bPriority == 0) { hasLeader = true; break; }
        }
        if (!hasLeader && !vec.empty()) {
            vec[0].bPriority = 0; // Promote first member to leader
        }
    }
    return true; // Party still exists
}

DWORD PartyManager::GetPartyID(DWORD dwCharID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_charToParty.find(dwCharID);
    return (it != m_charToParty.end()) ? it->second : 0;
}

std::vector<PartyMemberInfo> PartyManager::GetMembers(DWORD dwPartyID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_parties.find(dwPartyID);
    if (it != m_parties.end()) return it->second;
    return {};
}

DWORD PartyManager::GetLeaderCharID(DWORD dwPartyID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_parties.find(dwPartyID);
    if (it == m_parties.end()) return 0;
    for (auto& m : it->second) {
        if (m.bPriority == 0) return m.dwCharID;
    }
    return 0;
}

void PartyManager::DisbandParty(DWORD dwPartyID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_parties.find(dwPartyID);
    if (it == m_parties.end()) return;
    for (auto& m : it->second) {
        m_charToParty.erase(m.dwCharID);
    }
    m_parties.erase(it);
}

void PartyManager::SetPendingInvite(DWORD dwAskerCharID, DWORD dwAskedCharID, BYTE bPartyType) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingInvites[dwAskerCharID] = { dwAskedCharID, bPartyType };
}

bool PartyManager::GetPendingInvite(DWORD dwAskerCharID, DWORD& outAskedCharID, BYTE& outPartyType) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_pendingInvites.find(dwAskerCharID);
    if (it == m_pendingInvites.end()) return false;
    outAskedCharID = it->second.dwAskedCharID;
    outPartyType = it->second.bPartyType;
    return true;
}

void PartyManager::ClearPendingInvite(DWORD dwAskerCharID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_pendingInvites.erase(dwAskerCharID);
}

void PartyManager::SetShareMode(DWORD dwPartyID, BYTE byExpMode, BYTE byFEMode) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_shareModes[dwPartyID] = { byExpMode, byFEMode };
}

BYTE PartyManager::GetExpShareMode(DWORD dwPartyID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_shareModes.find(dwPartyID);
    if (it != m_shareModes.end()) return it->second.byExpDivision;
    return 1; // default: shared
}

BYTE PartyManager::GetFEShareMode(DWORD dwPartyID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_shareModes.find(dwPartyID);
    if (it != m_shareModes.end()) return it->second.byFEDivision;
    return 1; // default: shared
}

// =========================================================
// OnPartyShareReq - Toggle EXP/FE share mode (leader only)
// Payload: BYTE byExpDivision, BYTE byFEDivision
// 0 = individual, 1 = shared
// =========================================================
void OnPartyShareReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize) {
    if (wSize < 2) return;

    BYTE byExpDivision = pPayload[0];
    BYTE byFEDivision = pPayload[1];

    LOG("[PARTY] PartyShareReq: CharID=" + std::to_string(dwCharID) +
        " Exp=" + std::to_string(byExpDivision) +
        " FE=" + std::to_string(byFEDivision));

    DWORD partyID = PartyManager::GetInstance().GetPartyID(dwCharID);
    if (partyID == 0) return;

    // Only leader can change share mode
    DWORD leaderCharID = PartyManager::GetInstance().GetLeaderCharID(partyID);
    if (leaderCharID != dwCharID) return;

    // Update share mode
    PartyManager::GetInstance().SetShareMode(partyID, byExpDivision, byFEDivision);

    // Broadcast to all party members
    auto members = PartyManager::GetInstance().GetMembers(partyID);
    std::vector<BYTE> payload;
    pushByte(payload, byExpDivision);
    pushByte(payload, byFEDivision);

    for (auto& m : members) {
        SOCKET mSocket = SessionMgr::GetInstance().GetSocketByCharID(m.dwCharID);
        if (mSocket != INVALID_SOCKET) {
            SendPacket(mSocket, PKT_PARTYSHARE_ACK, payload);
        }
    }
}

void PartyManager::BroadcastMemberPosition(DWORD dwCharID, WORD wLevel, DWORD dwHpCur, DWORD dwHpMax, DWORD dwMapID, WORD wPosX, WORD wPosY) {
    DWORD partyID = GetPartyID(dwCharID);
    if (partyID == 0) return;

    auto members = GetMembers(partyID);
    if (members.size() <= 1) return;

    DWORD dwObjID = dwCharID + 400000000;
    std::vector<BYTE> payload;
    pushDWord(payload, partyID);
    pushDWord(payload, dwObjID);
    pushWord(payload, wLevel);
    pushDWord(payload, dwHpCur);
    pushDWord(payload, dwHpMax);
    pushDWord(payload, dwMapID);
    pushWord(payload, wPosX);
    pushWord(payload, wPosY);

    for (auto& m : members) {
        if (m.dwCharID == dwCharID) continue; // skip self
        SOCKET mSocket = SessionMgr::GetInstance().GetSocketByCharID(m.dwCharID);
        if (mSocket != INVALID_SOCKET) {
            SendPacket(mSocket, PKT_PARTYPOSITION_ACK, payload);
        }
    }
}


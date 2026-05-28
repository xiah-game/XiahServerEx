#include "ChatHandler.h"
#include "PartyHandler.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../DB/CharacterDB.h"
#include <cstring>

// =========================================================
// sString Helper Functions
// =========================================================
static std::string ReadSString(BYTE*& ptr, WORD& remaining) {
    if (remaining < 2) return "";
    WORD len = *(WORD*)ptr;
    ptr += 2;
    remaining -= 2;
    if (len == 0 || len > remaining) {
        if (len > remaining) ptr += remaining;
        remaining = 0;
        return "";
    }
    std::string s((char*)ptr, len);
    ptr += len;
    remaining -= len;
    return s;
}

static void WriteSString(std::vector<BYTE>& buf, const std::string& s) {
    WORD len = (WORD)s.size();
    buf.push_back(len & 0xFF);
    buf.push_back((len >> 8) & 0xFF);
    buf.insert(buf.end(), s.begin(), s.end());
}

// =========================================================
// Build CS_CH_CHAT_ACK packet
// =========================================================
static std::vector<BYTE> BuildChatAck(
    DWORD dwSenderObjectID,
    BYTE bType,
    const std::string& content,
    const std::string& senderName,
    DWORD dwListenerObjectID = 0,
    const std::string& listenerName = "")
{
    std::vector<BYTE> buf;
    // Header: ID (2) + PayloadSize (2)
    buf.resize(4, 0);

    // sender (DWORD)
    DWORD val = dwSenderObjectID;
    buf.push_back(val & 0xFF); buf.push_back((val >> 8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF); buf.push_back((val >> 24) & 0xFF);

    // type (BYTE)
    buf.push_back(bType);

    // For types other than CT_BATTLE and CT_TIMEMESSAGE, append content + senderName
    if (bType != CT_BATTLE && bType != CT_TIMEMESSAGE) {
        WriteSString(buf, content);
        WriteSString(buf, senderName);
    }

    // For CT_WHISPER, append listener ObjectID + listenerName
    if (bType == CT_WHISPER) {
        val = dwListenerObjectID;
        buf.push_back(val & 0xFF); buf.push_back((val >> 8) & 0xFF);
        buf.push_back((val >> 16) & 0xFF); buf.push_back((val >> 24) & 0xFF);
        WriteSString(buf, listenerName);
    }

    // Fill header
    WORD packetID = CS_CH_CHAT_ACK_ID;
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);

    return buf;
}

// =========================================================
// Get player name from sServerObject
// =========================================================
static std::string GetPlayerName(DWORD dwCharID) {
    for (auto& pair : g_MapInstances) {
        CMapInstance* mapInst = pair.second;
        if (!mapInst) continue;
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        DWORD objectID = dwCharID + 400000000;
        sServerObject* player = mapInst->GetPlayer(objectID);
        if (player) {
            return player->szName;
        }
    }
    return "";
}

// =========================================================
// Find CharID by character name (search online players)
// =========================================================
static DWORD FindCharIDByName(const std::string& name) {
    for (auto& pair : g_MapInstances) {
        CMapInstance* mapInst = pair.second;
        if (!mapInst) continue;
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        auto& players = mapInst->GetPlayers();
        for (auto& pp : players) {
            if (pp.second.szName == name) {
                return pp.second.dwObjectID - 400000000;
            }
        }
    }
    return 0;
}

// =========================================================
// OnChatReq - Main chat packet handler
// =========================================================
void OnChatReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize) {
    if (wSize < 5) return; // minimum: type(1) + listener(4)

    BYTE* ptr = pPayload;
    WORD remaining = wSize;

    // Read type (BYTE)
    BYTE bType = *ptr; ptr++; remaining--;

    // Read listener (DWORD)
    DWORD dwListener = *(DWORD*)ptr; ptr += 4; remaining -= 4;

    // Read content (sString)
    std::string content = ReadSString(ptr, remaining);

    // Read nickname (sString) - used for whisper-by-name
    std::string szNickName = ReadSString(ptr, remaining);

    // Sender info
    DWORD dwSenderObjectID = dwCharID + 400000000;
    std::string senderName = GetPlayerName(dwCharID);
    


    LOG("[CHAT] Type=" + std::to_string(bType) +
        " Sender=" + senderName +
        " (CharID=" + std::to_string(dwCharID) + ")" +
        " Content=" + content);

    switch (bType) {
    // =====================================================
    // CT_NORMAL (0) - Broadcast to all in same map
    // =====================================================
    case CT_NORMAL:
    {
        std::vector<BYTE> ackBuf = BuildChatAck(
            dwSenderObjectID, CT_NORMAL, content, senderName);

        DWORD mapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        auto it = g_MapInstances.find(mapID);
        if (it != g_MapInstances.end() && it->second) {
            CMapInstance* mapInst = it->second;
            // Encrypt before broadcast (BroadcastPacket sends raw)
            EncryptPacket(ackBuf.data(), 0x42);
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* player = mapInst->GetPlayer(dwSenderObjectID);
            if (player) {
                mapInst->BroadcastPacket(ackBuf);
            }
        }
        break;
    }

    // =====================================================
    // CT_WHISPER (1) - Private message to a specific player
    // =====================================================
    case CT_WHISPER:
    {
        DWORD dwTargetCharID = 0;
        std::string targetName;

        if (dwListener != 0) {
            dwTargetCharID = dwListener;
            targetName = GetPlayerName(dwTargetCharID);
        }
        else if (!szNickName.empty()) {
            dwTargetCharID = FindCharIDByName(szNickName);
            targetName = szNickName;
        }

        if (dwTargetCharID == 0) {
            LOG("[CHAT] Whisper target not found: " + szNickName);
            break;
        }

        DWORD dwTargetObjectID = dwTargetCharID + 400000000;

        // Build ACK with whisper fields
        std::vector<BYTE> ackBuf = BuildChatAck(
            dwSenderObjectID, CT_WHISPER, content, senderName,
            dwTargetObjectID, targetName);

        // Send to the target
        SOCKET targetSocket = SessionMgr::GetInstance().GetSocketByCharID(dwTargetCharID);
        if (targetSocket != INVALID_SOCKET) {
            std::vector<BYTE> targetCopy = ackBuf;
            EncryptPacket(targetCopy.data(), 0x42);
            SafeSend(targetSocket, (const char*)targetCopy.data(), (int)targetCopy.size(), 0);
        }

        // Also send back to the sender (so they see their own whisper)
        std::vector<BYTE> senderAck = ackBuf;
        EncryptPacket(senderAck.data(), 0x42);
        SafeSend(clientSocket, (const char*)senderAck.data(), (int)senderAck.size(), 0);

        break;
    }

    // =====================================================
    // CT_DAN (5) - Party/Group chat
    // =====================================================
    case CT_DAN:
    {
        DWORD dwPartyID = PartyManager::GetInstance().GetPartyID(dwCharID);
        if (dwPartyID == 0) {
            LOG("[CHAT] Player not in party, CharID=" + std::to_string(dwCharID));
            break;
        }

        auto members = PartyManager::GetInstance().GetMembers(dwPartyID);

        std::vector<BYTE> ackBuf = BuildChatAck(
            dwSenderObjectID, CT_DAN, content, senderName);

        // Send to all party members (including sender)
        for (auto& member : members) {
            SOCKET memberSocket = SessionMgr::GetInstance().GetSocketByCharID(member.dwCharID);
            if (memberSocket != INVALID_SOCKET) {
                std::vector<BYTE> sendBuf = ackBuf;
                EncryptPacket(sendBuf.data(), 0x42);
                SafeSend(memberSocket, (const char*)sendBuf.data(), (int)sendBuf.size(), 0);
            }
        }
        break;
    }

    default:
        LOG("[CHAT] Unhandled chat type: " + std::to_string(bType));
        break;
    }
}


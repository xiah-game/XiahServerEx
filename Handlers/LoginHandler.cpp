#include "LoginHandler.h"
#include "../DB/CharacterDB.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/PlayerManager.h"
#include "SlotHandler.h"
#include "../GameObjects/PlayerManager.h"

void OnLoginCheckReq(SOCKET clientSocket, std::string& clientAccountName, BYTE* payload, WORD totalSize) {
    LOG("[LoginHandler] Received CS_IT_LOGINCHECK_REQ!");
    WORD nameLen = *(WORD*)(payload);
    if (nameLen > 1 && nameLen <= 256) {
        clientAccountName = std::string((char*)payload + 2, nameLen - 1);
        LOG("[LoginHandler] Login Account: " + clientAccountName);
        SessionMgr::GetInstance().SetAccount(clientSocket, clientAccountName);
    }

    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); 
    ackBuf.push_back(1); 
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    ackHead->id = CS_IT_LOGINCHECK_ACK; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[LoginHandler] Sent CS_IT_LOGINCHECK_ACK!");
}

void OnCharacterListReq(SOCKET clientSocket, const std::string& clientAccountName, BYTE* payload, WORD totalSize) {
    LOG("[LoginHandler] Received CS_IT_CHARACTERLIST_REQ!");
    std::vector<BYTE> charBuf; charBuf.resize(4); 

    auto pushStr = [&](const std::string& s) { WORD len = (WORD)(s.length() + 1); charBuf.push_back((BYTE)(len & 0xFF)); charBuf.push_back((BYTE)(len >> 8)); for (char c : s) charBuf.push_back((BYTE)c); charBuf.push_back(0); };
    auto pushByte = [&](BYTE b) { charBuf.push_back(b); };
    auto pushWord = [&](WORD w) { charBuf.push_back((BYTE)(w & 0xFF)); charBuf.push_back((BYTE)(w >> 8)); };
    auto pushDWord = [&](DWORD d) { charBuf.push_back((BYTE)(d & 0xFF)); charBuf.push_back((BYTE)((d >> 8) & 0xFF)); charBuf.push_back((BYTE)((d >> 16) & 0xFF)); charBuf.push_back((BYTE)(d >> 24)); };

    std::vector<CharacterDB::CharSelectEntry> chars;

    if (CharacterDB::GetInstance().GetCharSelectList(clientAccountName, chars)) {
        
        LOG("[LoginHandler] DB Character Count: " + std::to_string(chars.size()));
        charBuf.push_back((BYTE)chars.size()); // bCount
        for (auto& ch : chars) {
            pushDWord(ch.dwMapID); 
            pushDWord(400000000 + ch.dwCharID); // ObjectID
            pushStr(std::string(ch.szNickName)); 
            pushByte(ch.bCharType); 
            pushWord(ch.wLevel); 
            pushDWord(ch.dwHpCur); 
            pushDWord(ch.dwHpMax); 
            pushWord(ch.wIpCur); 
            pushWord(ch.wIpMax); 
            pushWord(ch.wVit); 
            pushWord(ch.wStr); 
            pushWord(ch.wSus); 
            pushWord(ch.wDex); 
            pushDWord(ch.dwBirthDate); 
            pushStr(""); // MunpaName
            pushStr(""); // OrderName

            for (int i = 0; i < 9; i++) {
                pushWord(ch.items[i].wVis); 
                pushByte(ch.items[i].bRar); 
                pushByte(ch.items[i].bStx); 
            }
            pushStr(""); // szMapName
            pushByte(1); // m_bAuction (Must be 1, otherwise client Start Game button ignores clicks)
            pushByte(ch.bRebirth); // Rebirth
        }
    } else {
        LOG("[LoginHandler] DB Fetch Failed! Pushed 0 chars.");
        charBuf.push_back(0); 
    }

    PACKET_HEADER* charHead = (PACKET_HEADER*)charBuf.data();
    charHead->id = CS_IT_CHARACTERLIST_ACK; 
    charHead->payloadSize = (WORD)(charBuf.size() - sizeof(PACKET_HEADER));
    EncryptPacket(charBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)charBuf.data(), charBuf.size(), 0);
    LOG("[LoginHandler] Pushed DB Character List successfully!");
}

void OnStartGameReq(SOCKET clientSocket, std::string& clientAccountName, BYTE* payload, WORD totalSize) {
    DWORD dwObjectID = *(DWORD*)(payload);
    DWORD dwCharID = dwObjectID - 400000000;
    LOG("[LoginHandler] Received CS_NV_STARTGAME_REQ! dwCharID: " + std::to_string(dwCharID));

    SessionMgr::GetInstance().SetCharID(clientSocket, dwCharID);

    DWORD dwMapID = CharacterDB::GetInstance().GetCharMapID(dwCharID);
    LOG("[LoginHandler] Found real Map ID: " + std::to_string(dwMapID) + " for Character: " + std::to_string(dwCharID));

    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); // Success
    ackBuf.push_back((BYTE)(dwMapID & 0xFF)); ackBuf.push_back((BYTE)((dwMapID >> 8) & 0xFF)); ackBuf.push_back((BYTE)((dwMapID >> 16) & 0xFF)); ackBuf.push_back((BYTE)(dwMapID >> 24));
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    ackHead->id = CS_NV_STARTGAME_ACK; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[LoginHandler] Sent CS_NV_STARTGAME_ACK!");

    UpdatePlayerStatsAndSend(clientSocket, dwCharID);
}

void OnEndGameReq(SOCKET clientSocket, DWORD dwCharID, BYTE* payload, WORD totalSize) {
    LOG("[LoginHandler] Received CS_NV_ENDGAME_REQ for dwCharID: " + std::to_string(dwCharID));

    // Save player data
    PlayerManager::GetInstance().SavePlayer(dwCharID);

    // Send ACK back to client
    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); // Success
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    ackHead->id = CS_NV_ENDGAME_ACK; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[LoginHandler] Sent CS_NV_ENDGAME_ACK!");
}


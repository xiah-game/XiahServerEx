#include "StatHandler.h"
#include "../Network/SessionMgr.h"
#include "../DB/CharacterDB.h"
#include "../GameObjects/PlayerManager.h"
#include "../ServerCore.h"
#include <vector>
#include <string>

// SP_DEX=1, SP_STR=2, SP_SUS=3, SP_VIT=4

namespace StatHandler {

    void OnExecSpReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
        if (totalSize < 2) return;
        BYTE bSpType = payload[0];
        BYTE bSpValue = payload[1];
        
        if (bSpValue == 0) return;
        
        // 1. Fetch current stats via CharacterDB
        CharacterDB::CharPower stats;
        bool bFound = CharacterDB::GetInstance().GetCharData(charID, stats);
        
        WORD wStr = stats.wStr, wSus = stats.wSus, wDex = stats.wDex, wVit = stats.wVit;
        WORD wRemainSp = stats.wRemainSp;
        
        auto sendFail = [&]() {
            std::vector<BYTE> ackBuf(7, 0);
            ackBuf[0] = 1; // bResult=1 (fail)
            std::vector<BYTE> fullBuf(ackBuf.size() + 4);
            memcpy(fullBuf.data() + 4, ackBuf.data(), ackBuf.size());
            PACKET_HEADER* head = (PACKET_HEADER*)fullBuf.data();
            head->id = 0x401E; 
            head->payloadSize = (WORD)ackBuf.size();
            EncryptPacket(fullBuf.data(), 0x42);
            SafeSend(clientSocket, (const char*)fullBuf.data(), fullBuf.size(), 0);
        };

        if (!bFound || wRemainSp < bSpValue) {
            sendFail();
            return;
        }
        
        // 2. Apply stat change
        WORD newValue = 0;
        if (bSpType == 1) { wDex += bSpValue; newValue = wDex; }
        else if (bSpType == 2) { wStr += bSpValue; newValue = wStr; }
        else if (bSpType == 3) { wSus += bSpValue; newValue = wSus; }
        else if (bSpType == 4) { wVit += bSpValue; newValue = wVit; }
        else {
            sendFail();
            return;
        }
        
        wRemainSp -= bSpValue;
        
        // 3. Persist via CharacterDB
        CharacterDB::GetInstance().UpdateStatPoints(charID, wStr, wSus, wDex, wVit, wRemainSp);
        
        // 4. Send ACK
        std::vector<BYTE> ackBuf;
        ackBuf.push_back(0); // bResult = success
        ackBuf.push_back(bSpType);
        ackBuf.push_back(bSpValue);
        ackBuf.push_back(wRemainSp & 0xFF); ackBuf.push_back(wRemainSp >> 8);
        ackBuf.push_back(newValue & 0xFF); ackBuf.push_back(newValue >> 8);
        
        std::vector<BYTE> fullBuf(ackBuf.size() + 4);
        memcpy(fullBuf.data() + 4, ackBuf.data(), ackBuf.size());
        PACKET_HEADER* head = (PACKET_HEADER*)fullBuf.data();
        head->id = 0x401E; // CS_BT_EXECSP_ACK
        head->payloadSize = ackBuf.size();
        EncryptPacket(fullBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)fullBuf.data(), fullBuf.size(), 0);
        
        // 5. Recalculate stats and send updates
        UpdatePlayerStatsAndSend(clientSocket, charID);
    }
}

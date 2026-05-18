#include "StatHandler.h"
#include "../Network/SessionMgr.h"
#include "../DBHelper.h"
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
        
        // 1. Fetch current stats
        std::string q = "SELECT wStr, wSus, wDex, wVit, wRemainSp FROM CHAR_POWER WHERE dwCharID = " + std::to_string(charID);
        WORD wStr = 0, wSus = 0, wDex = 0, wVit = 0, wRemainSp = 0;
        bool bFound = false;
        
        DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
            bFound = true;
            SQLLEN cb;
            SQLGetData(hStmt, 1, SQL_C_USHORT, &wStr, 0, &cb);
            SQLGetData(hStmt, 2, SQL_C_USHORT, &wSus, 0, &cb);
            SQLGetData(hStmt, 3, SQL_C_USHORT, &wDex, 0, &cb);
            SQLGetData(hStmt, 4, SQL_C_USHORT, &wVit, 0, &cb);
            SQLGetData(hStmt, 5, SQL_C_USHORT, &wRemainSp, 0, &cb);
        });
        
        auto sendFail = [&]() {
            std::vector<BYTE> ackBuf(7, 0);
            ackBuf[0] = 1; // bResult=1 (fail)
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = 0x401E; 
            head->payloadSize = 3; // minimal size
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
        };

        if (!bFound || wRemainSp < bSpValue) {
            sendFail();
            return;
        }
        
        // 2. Update stats
        std::string colName = "";
        WORD newValue = 0;
        if (bSpType == 1) { colName = "wDex"; newValue = wDex + bSpValue; }
        else if (bSpType == 2) { colName = "wStr"; newValue = wStr + bSpValue; }
        else if (bSpType == 3) { colName = "wSus"; newValue = wSus + bSpValue; }
        else if (bSpType == 4) { colName = "wVit"; newValue = wVit + bSpValue; }
        else {
            sendFail();
            return;
        }
        
        wRemainSp -= bSpValue;
        
        std::string upd = "UPDATE CHAR_POWER SET " + colName + " = " + std::to_string(newValue) + 
                          ", wRemainSp = " + std::to_string(wRemainSp) + " WHERE dwCharID = " + std::to_string(charID);
                          
        if (DBHelper::GetInstance().ExecuteUpdate(upd)) {
            // 3. Send ACK
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
            
            // 4. Recalculate stats and send updates
            UpdatePlayerStatsAndSend(clientSocket, charID);
        } else {
            sendFail();
        }
    }
}

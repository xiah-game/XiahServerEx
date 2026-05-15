#include <vector>
#include <string>
#include "RebuildItemHandler.h"
#include "../Network/SessionMgr.h"
#include "../DBHelper.h"
#include "../Network/SystemMessage.h"
#include "../Network/PacketRouter.h"

// Helper to push integers safely
static void pushDWord(std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); }
static void pushWord(std::vector<BYTE>& buf, WORD w) { buf.push_back(w&0xFF); buf.push_back(w>>8); }
static void pushString(std::vector<BYTE>& buf, const std::string& str) {
    pushWord(buf, str.length());
    for(char c : str) buf.push_back(c);
}

void OnRebuildItemTermReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 6) return;
    
    DWORD dwItemID = *(DWORD*)(payload + 0);
    BYTE bSackID = payload[4];
    BYTE bSackPos = payload[5];
    
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT TOP 1 1 FROM SACKITEM WHERE dwCharID = " + std::to_string(charID) + " AND dwItemID = " + std::to_string(dwItemID),
        [&](SQLHSTMT hStmt) { found = true; }
    );
    
    std::vector<BYTE> ackBuf(5);
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4242; // CS_IM_REBUILDITEMTERM_ACK
    head->payloadSize = 1;
    ackBuf[4] = found ? 0 : 1; // 0 = SUCCESS, 1 = NOTFOUNDITEM
    
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
}

void OnRebuildItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 28) return;
    
    DWORD dwShopID = *(DWORD*)(payload + 0);
    DWORD dwItemID = *(DWORD*)(payload + 4);
    BYTE bSackID = payload[8];
    BYTE bSackPos = payload[9];
    
    DWORD dwResourceID[3];
    BYTE bResourceSackID[3];
    BYTE bResourcePos[3];
    
    for(int i=0; i<3; i++) {
        dwResourceID[i] = *(DWORD*)(payload + 10 + i*6);
        bResourceSackID[i] = payload[14 + i*6];
        bResourcePos[i] = payload[15 + i*6];
    }
    
    // Validate target item
    bool itemValid = false;
    std::string itemName = "";
    int currentRebuild = 0;
    
    std::string qItem = "SELECT I.szName, ISNULL(D.nData20, 0) FROM SACKITEM S JOIN ITEM I ON S.dwItemID = I.dwItemID LEFT JOIN ITEMDATA D ON S.dwItemID = D.dwItemID WHERE S.dwCharID = " + std::to_string(charID) + " AND S.dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteQuery(qItem, [&](SQLHSTMT hStmt) {
        char szName[128] = {0};
        SQLLEN c[2];
        SQLGetData(hStmt, 1, SQL_C_CHAR, szName, sizeof(szName), &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &currentRebuild, 0, &c[1]);
        if(c[0] != SQL_NULL_DATA) itemName = szName;
        itemValid = true;
    });
    
    if (!itemValid) {
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4244; // CS_IM_REBUILDITEM_ACK
        head->payloadSize = 1;
        ackBuf[4] = 2; // NOTFOUNDITEM
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
        return;
    }
    
    // Optional: consume money
    DWORD rebuildCost = 10000;
    bool hasMoney = false;
    DBHelper::GetInstance().ExecuteQuery("SELECT dwMoney FROM CHAR_DATA WHERE dwCharID = " + std::to_string(charID), [&](SQLHSTMT hStmt) {
        INT64 money = 0;
        SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_SBIGINT, &money, 0, &c);
        if (money >= rebuildCost) hasMoney = true;
    });
    
    if (!hasMoney) {
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4244;
        head->payloadSize = 1;
        ackBuf[4] = 1; // NOMONEY
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
        return;
    }
    
    // Deduct money
    DBHelper::GetInstance().ExecuteUpdate("UPDATE CHAR_DATA SET dwMoney = dwMoney - " + std::to_string(rebuildCost) + " WHERE dwCharID = " + std::to_string(charID));
    
    // Sync Money to Client (0x3B13)
    DBHelper::GetInstance().ExecuteQuery("SELECT dwMoney FROM CHAR_DATA WHERE dwCharID = " + std::to_string(charID), [&](SQLHSTMT hStmt) {
        INT64 currentMoney = 0; SQLLEN cb;
        SQLGetData(hStmt, 1, SQL_C_SBIGINT, &currentMoney, 0, &cb);
        std::vector<BYTE> moneyBuf(13);
        PACKET_HEADER* mHead = (PACKET_HEADER*)moneyBuf.data();
        mHead->id = 0x3B13;
        mHead->payloadSize = 9;
        *((INT64*)(moneyBuf.data() + 4)) = (INT64)currentMoney;
        moneyBuf[12] = 0;
        EncryptPacket(moneyBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)moneyBuf.data(), moneyBuf.size(), 0);
    });
    
    // Delete consumed resources
    for (int i=0; i<3; i++) {
        if (dwResourceID[i] != 0) {
            std::string resIdStr = std::to_string(dwResourceID[i]);
            // Decrease wAmount or Delete
            bool amountDecreased = false;
            DBHelper::GetInstance().ExecuteQuery("SELECT wAmount FROM ITEM WHERE dwItemID = " + resIdStr, [&](SQLHSTMT hStmt) {
                int amount = 0; SQLLEN c;
                SQLGetData(hStmt, 1, SQL_C_SLONG, &amount, 0, &c);
                if (amount > 1) {
                    amountDecreased = true;
                    DBHelper::GetInstance().ExecuteUpdate("UPDATE ITEM SET wAmount = wAmount - 1 WHERE dwItemID = " + resIdStr);
                    // Send change amount packet
                    std::vector<BYTE> ackBuf(4);
                    ackBuf.push_back(bResourceSackID[i]);
                    ackBuf.push_back(bResourcePos[i]);
                    pushDWord(ackBuf, dwResourceID[i]);
                    pushWord(ackBuf, amount - 1);
                    PACKET_HEADER* ah = (PACKET_HEADER*)ackBuf.data();
                    ah->id = 0x4216; // CS_IM_CHANGERES_ACK
                    ah->payloadSize = ackBuf.size() - 4;
                    EncryptPacket(ackBuf.data(), 0x42);
                    SafeSend(clientSocket, (char*)ackBuf.data(), ackBuf.size(), 0);
                }
            });
            
            if (!amountDecreased) {
                DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEM WHERE dwItemID = " + resIdStr);
                DBHelper::GetInstance().ExecuteUpdate("DELETE FROM SACKITEM WHERE dwItemID = " + resIdStr);
                // Send remove item packet
                std::vector<BYTE> ackBuf(4);
                ackBuf.push_back(bResourceSackID[i]);
                ackBuf.push_back(bResourcePos[i]);
                pushDWord(ackBuf, dwResourceID[i]);
                ackBuf.push_back(0); // reason
                PACKET_HEADER* ah = (PACKET_HEADER*)ackBuf.data();
                ah->id = 0x4208; // CS_IM_REMOVESACK_ACK
                ah->payloadSize = ackBuf.size() - 4;
                EncryptPacket(ackBuf.data(), 0x42);
                SafeSend(clientSocket, (char*)ackBuf.data(), ackBuf.size(), 0);
            }
        }
    }
    
    // RNG Success
    int successChance = 70; // 70% success
    bool isSuccess = (rand() % 100) < successChance;
    
    std::vector<BYTE> ackBuf;
    ackBuf.resize(4); // header space
    
    if (isSuccess) {
        // Upgrade attributes! Increase Damage (nData1) and RebuildLevel (nData20)
        int upgradeVal = 5;
        DBHelper::GetInstance().ExecuteUpdate("IF EXISTS (SELECT 1 FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwItemID) + ") "
                                              "UPDATE ITEMDATA SET nData1 = nData1 + " + std::to_string(upgradeVal) + ", nData20 = nData20 + 1 WHERE dwItemID = " + std::to_string(dwItemID) + " "
                                              "ELSE INSERT INTO ITEMDATA (dwItemID, nData1, nData20) VALUES (" + std::to_string(dwItemID) + ", " + std::to_string(upgradeVal) + ", 1)");
                                              
        ackBuf.push_back(0); // bResult = SUCCESS
        ackBuf.push_back(0); // bFactor = 0 (Attack Power)
        pushDWord(ackBuf, upgradeVal); // nValues
        pushString(ackBuf, itemName);
        ackBuf.push_back(currentRebuild + 1); // bItemnum (New Rebuild Level)
        
        // Broadcast SystemMessage for success if RebuildLevel is high
        if (currentRebuild + 1 >= 5) {
            // Optional system broadcast could be added here
        }
    } else {
        // Failure!
        ackBuf.push_back(5); // bResult = ERR_REBUILDITEM_FAIL
        ackBuf.push_back(255); // bFactor = 255 (No factor printed)
        pushDWord(ackBuf, 0); // nValues
        pushString(ackBuf, itemName);
        ackBuf.push_back(currentRebuild);
    }
    
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4244; // CS_IM_REBUILDITEM_ACK
    head->payloadSize = ackBuf.size() - 4;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    
    // If success, we should tell the client to refresh the item info
    if (isSuccess) {
        // We can just resend the CS_IT_CHARINFO or let the client request it.
        // Actually, the client handles updating the UI automatically, or requests item info.
        // CS_IM_ITEMINFO_REQ is requested by the client on hover.
    }
}

void RegisterRebuildItemHandlers() {
    RegisterHandler(0x4241, [](SOCKET s, BYTE* p, WORD sz) { OnRebuildItemTermReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x4243, [](SOCKET s, BYTE* p, WORD sz) { OnRebuildItemReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
}

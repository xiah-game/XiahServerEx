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
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT TOP 1 1 FROM SACKITEM WHERE dwCharID = " + std::to_string(charID) + " AND dwItemID = " + std::to_string(dwItemID),
        [&](SQLHSTMT hStmt) { found = true; }
    );
    std::vector<BYTE> ackBuf(5);
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4242; // CS_IM_REBUILDITEMTERM_ACK
    head->payloadSize = 1;
    ackBuf[4] = found ? 0 : 1;
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
    
    bool itemValid = false;
    std::string itemName = "", baseItemName = "";
    int currentRebuild = 0, currentAppend = 0, itemType = 0, reqLevel = 1;
    int baseData1 = 0, baseData3 = 0, baseData7 = 0;
    
    std::string qItem = "SELECT I.szName, ISNULL(D.nData14, 0), ISNULL(D.nData15, 0), I.bType, T.wLevel, T.szName, ISNULL(T.nBasicData1, 0), ISNULL(T.nBasicData3, 0), ISNULL(T.nBasicData7, 0) FROM SACKITEM S JOIN ITEM I ON S.dwItemID = I.dwItemID LEFT JOIN ITEMDATA D ON S.dwItemID = D.dwItemID JOIN ITEMTEMPLATE T ON I.wRefID = T.wRefID WHERE S.dwCharID = " + std::to_string(charID) + " AND S.dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteQuery(qItem, [&](SQLHSTMT hStmt) {
        char szName[128] = {0}, szBaseName[128] = {0};
        SQLLEN c[9];
        SQLGetData(hStmt, 1, SQL_C_CHAR, szName, sizeof(szName), &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &currentRebuild, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &currentAppend, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &itemType, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &reqLevel, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_CHAR, szBaseName, sizeof(szBaseName), &c[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &baseData1, 0, &c[6]);
        SQLGetData(hStmt, 8, SQL_C_SLONG, &baseData3, 0, &c[7]);
        SQLGetData(hStmt, 9, SQL_C_SLONG, &baseData7, 0, &c[8]);
        if(c[0] != SQL_NULL_DATA) itemName = szName;
        if(c[5] != SQL_NULL_DATA) baseItemName = szBaseName;
        itemValid = true;
    });
    
    if (!itemValid) {
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4244; head->payloadSize = 1; ackBuf[4] = 2; // NOTFOUNDITEM
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
        return;
    }
    
    int crystalKind = 0, crystalSuccessRate = 0;
    bool hasCrystal = false;
    if (dwResourceID[0] != 0) {
        std::string qRes = "SELECT T.bKind, ISNULL(T.nBasicData2, 0) FROM ITEM I JOIN ITEMTEMPLATE T ON I.wRefID = T.wRefID WHERE I.dwItemID = " + std::to_string(dwResourceID[0]);
        DBHelper::GetInstance().ExecuteQuery(qRes, [&](SQLHSTMT hStmt) {
            SQLLEN c[2];
            SQLGetData(hStmt, 1, SQL_C_SLONG, &crystalKind, 0, &c[0]);
            SQLGetData(hStmt, 2, SQL_C_SLONG, &crystalSuccessRate, 0, &c[1]);
            hasCrystal = true;
        });
    }

    if (!hasCrystal || (crystalKind != 1 && crystalKind != 2)) {
        // Must put Wujing or Sujing!
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4244; head->payloadSize = 1; ackBuf[4] = 5; // FAIL
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
        return;
    }

    DWORD rebuildCost = 10000;
    bool hasMoney = false;
    DBHelper::GetInstance().ExecuteQuery("SELECT dwMoney FROM CHAR_DATA WHERE dwCharID = " + std::to_string(charID), [&](SQLHSTMT hStmt) {
        INT64 money = 0; SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_SBIGINT, &money, 0, &c);
        if (money >= rebuildCost) hasMoney = true;
    });
    
    if (!hasMoney) {
        std::vector<BYTE> ackBuf(5); PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4244; head->payloadSize = 1; ackBuf[4] = 1; // NOMONEY
        EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0); return;
    }
    
    // Deduct money and send sync packet
    DBHelper::GetInstance().ExecuteUpdate("UPDATE CHAR_DATA SET dwMoney = dwMoney - " + std::to_string(rebuildCost) + " WHERE dwCharID = " + std::to_string(charID));
    DBHelper::GetInstance().ExecuteQuery("SELECT dwMoney FROM CHAR_DATA WHERE dwCharID = " + std::to_string(charID), [&](SQLHSTMT hStmt) {
        INT64 currentMoney = 0; SQLLEN cb;
        SQLGetData(hStmt, 1, SQL_C_SBIGINT, &currentMoney, 0, &cb);
        std::vector<BYTE> moneyBuf(13); PACKET_HEADER* mHead = (PACKET_HEADER*)moneyBuf.data();
        mHead->id = 0x3B13; mHead->payloadSize = 9;
        *((INT64*)(moneyBuf.data() + 4)) = (INT64)currentMoney; moneyBuf[12] = 0;
        EncryptPacket(moneyBuf.data(), 0x42); SafeSend(clientSocket, (const char*)moneyBuf.data(), moneyBuf.size(), 0);
    });
    
    // Consume materials
    bool hasAntiBreak = false; // Add AntiBreak scroll logic here if needed by checking dwResourceID[1] or [2]
    for (int i=0; i<3; i++) {
        if (dwResourceID[i] != 0) {
            std::string resIdStr = std::to_string(dwResourceID[i]);
            bool amountDecreased = false;
            DBHelper::GetInstance().ExecuteQuery("SELECT wAmount FROM ITEM WHERE dwItemID = " + resIdStr, [&](SQLHSTMT hStmt) {
                int amount = 0; SQLLEN c;
                SQLGetData(hStmt, 1, SQL_C_SLONG, &amount, 0, &c);
                if (amount > 1) {
                    amountDecreased = true;
                    DBHelper::GetInstance().ExecuteUpdate("UPDATE ITEM SET wAmount = wAmount - 1 WHERE dwItemID = " + resIdStr);
                    std::vector<BYTE> ackBuf(4); ackBuf.push_back(bResourceSackID[i]); ackBuf.push_back(bResourcePos[i]);
                    pushDWord(ackBuf, dwResourceID[i]); pushWord(ackBuf, amount - 1);
                    PACKET_HEADER* ah = (PACKET_HEADER*)ackBuf.data(); ah->id = 0x4216; ah->payloadSize = ackBuf.size() - 4;
                    EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (char*)ackBuf.data(), ackBuf.size(), 0);
                }
            });
            if (!amountDecreased) {
                DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEM WHERE dwItemID = " + resIdStr);
                DBHelper::GetInstance().ExecuteUpdate("DELETE FROM SACKITEM WHERE dwItemID = " + resIdStr);
                std::vector<BYTE> ackBuf(4); ackBuf.push_back(bResourceSackID[i]); ackBuf.push_back(bResourcePos[i]);
                pushDWord(ackBuf, dwResourceID[i]); ackBuf.push_back(0);
                PACKET_HEADER* ah = (PACKET_HEADER*)ackBuf.data(); ah->id = 0x4208; ah->payloadSize = ackBuf.size() - 4;
                EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (char*)ackBuf.data(), ackBuf.size(), 0);
            }
        }
    }

    int targetLevel = (crystalKind == 1) ? (currentRebuild + 1) : (currentAppend + 1);
    if (targetLevel > 99) return; // Limit

    int wAttr = 0, sAttr = 0, lMulti = 0, baseRate = 0, breakChance = 0;
    bool configFound = false;
    DBHelper::GetInstance().ExecuteQuery("SELECT Wujing_Attr, Sujing_Attr, LevelMultiplier, BaseRate, BreakChance FROM REBUILD_CONFIG WHERE RebuildLevel = " + std::to_string(targetLevel), [&](SQLHSTMT hStmt) {
        SQLLEN c[5];
        SQLGetData(hStmt, 1, SQL_C_SLONG, &wAttr, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &sAttr, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &lMulti, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &baseRate, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &breakChance, 0, &c[4]);
        configFound = true;
    });

    if (!configFound) {
        // Fallback or error
        return;
    }

    int finalSuccessRate = baseRate + crystalSuccessRate;
    bool isSuccess = (rand() % 100) < finalSuccessRate;
    bool didBreak = false;

    if (isSuccess) {
        if (crystalKind == 1) currentRebuild = targetLevel;
        else currentAppend = targetLevel;
    } else {
        if (crystalKind == 1) {
            if (!hasAntiBreak && (rand() % 100) < breakChance) didBreak = true;
        } else {
            if (currentAppend > 0) currentAppend--; // Sujing downgrade
        }
    }

    std::vector<BYTE> ackBuf; ackBuf.resize(4);
    
    if (didBreak) {
        std::string idStr = std::to_string(dwItemID);
        DBHelper::GetInstance().ExecuteUpdate("DELETE FROM SACKITEM WHERE dwItemID = " + idStr);
        DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEMDATA WHERE dwItemID = " + idStr);
        DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEM WHERE dwItemID = " + idStr);
        
        std::vector<BYTE> rmBuf(7); PACKET_HEADER* rmHead = (PACKET_HEADER*)rmBuf.data();
        rmHead->id = 0x4208; rmHead->payloadSize = 3;
        rmBuf[4] = bSackID; rmBuf[5] = bSackPos; rmBuf[6] = 2;
        EncryptPacket(rmBuf.data(), 0x42); SafeSend(clientSocket, (const char*)rmBuf.data(), rmBuf.size(), 0);
        
        ackBuf.push_back(5); ackBuf.push_back(255);
        pushDWord(ackBuf, 0); pushString(ackBuf, itemName); ackBuf.push_back(currentRebuild);
    } else {
        // Recalculate Total Bonus via SUM
        int totalWujingBonus = 0, totalSujingBonus = 0;
        if (currentRebuild > 0) {
            DBHelper::GetInstance().ExecuteQuery("SELECT ISNULL(SUM(Wujing_Attr * " + std::to_string(reqLevel) + " * LevelMultiplier / 100), 0) FROM REBUILD_CONFIG WHERE RebuildLevel <= " + std::to_string(currentRebuild), [&](SQLHSTMT hStmt) {
                SQLLEN c; SQLGetData(hStmt, 1, SQL_C_SLONG, &totalWujingBonus, 0, &c);
            });
        }
        if (currentAppend > 0) {
            DBHelper::GetInstance().ExecuteQuery("SELECT ISNULL(SUM(Sujing_Attr * " + std::to_string(reqLevel) + " * LevelMultiplier / 100), 0) FROM REBUILD_CONFIG WHERE RebuildLevel <= " + std::to_string(currentAppend), [&](SQLHSTMT hStmt) {
                SQLLEN c; SQLGetData(hStmt, 1, SQL_C_SLONG, &totalSujingBonus, 0, &c);
            });
        }

        int finalData1 = baseData1;
        int finalData3 = baseData3;
        int finalData7 = baseData7; // Accuracy
        
        if (itemType >= 1 && itemType <= 4) { // Weapons
            finalData1 += totalWujingBonus; // Upgrade adds Attack
            finalData7 += totalSujingBonus; // Append adds Accuracy
        } else if (itemType >= 5 && itemType <= 7) { // Armor
            finalData3 += totalWujingBonus; // Upgrade adds Defense
            // Sujing could add to Max HP (nData5 or something). Let's use nData3 for now.
            finalData3 += totalSujingBonus; 
        } else { // Others (Hats, Shoes, Acc)
            finalData7 += totalWujingBonus; // Generic 
            finalData1 += totalSujingBonus;
        }

        int nData20 = (currentRebuild > currentAppend) ? currentRebuild : currentAppend;

        DBHelper::GetInstance().ExecuteUpdate("IF EXISTS (SELECT 1 FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwItemID) + ") "
            "UPDATE ITEMDATA SET nData1 = " + std::to_string(finalData1) + ", nData3 = " + std::to_string(finalData3) + 
            ", nData7 = " + std::to_string(finalData7) + ", nData14 = " + std::to_string(currentRebuild) + 
            ", nData15 = " + std::to_string(currentAppend) + ", nData20 = " + std::to_string(nData20) + 
            " WHERE dwItemID = " + std::to_string(dwItemID) + " "
            "ELSE INSERT INTO ITEMDATA (dwItemID, nData1, nData3, nData7, nData14, nData15, nData20) VALUES (" + 
            std::to_string(dwItemID) + ", " + std::to_string(finalData1) + ", " + std::to_string(finalData3) + 
            ", " + std::to_string(finalData7) + ", " + std::to_string(currentRebuild) + ", " + std::to_string(currentAppend) + ", " + std::to_string(nData20) + ")");

        // Update Item Name
        std::string newName = "+" + std::to_string(currentRebuild) + " " + baseItemName;
        if (currentAppend > 0) newName += " (" + std::to_string(currentAppend) + "性质)";
        DBHelper::GetInstance().ExecuteUpdate("UPDATE ITEM SET szName = '" + newName + "' WHERE dwItemID = " + std::to_string(dwItemID));
        
        if (isSuccess) {
            ackBuf.push_back(0); // SUCCESS
            ackBuf.push_back((itemType >= 1 && itemType <= 4) ? 0 : 1);
            pushDWord(ackBuf, (crystalKind == 1) ? totalWujingBonus : totalSujingBonus);
            pushString(ackBuf, newName);
            ackBuf.push_back(nData20);
        } else {
            ackBuf.push_back(5); // FAIL
            ackBuf.push_back(255);
            pushDWord(ackBuf, 0); pushString(ackBuf, newName); ackBuf.push_back(nData20);
        }
    }
    
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4244;
    head->payloadSize = ackBuf.size() - 4;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
}

void RegisterRebuildItemHandlers() {
    RegisterHandler(0x4241, [](SOCKET s, BYTE* p, WORD sz) { OnRebuildItemTermReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x4243, [](SOCKET s, BYTE* p, WORD sz) { OnRebuildItemReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
}

#include <vector>
#include <string>
#include "RebuildItemHandler.h"
#include "../Network/SessionMgr.h"
#include "../DBHelper.h"
#include "../Network/SystemMessage.h"
#include "../Network/PacketRouter.h"
#include "../ServerCore.h"

static void pushDWord(std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); }
static void pushWord(std::vector<BYTE>& buf, WORD w) { buf.push_back(w&0xFF); buf.push_back(w>>8); }
static void pushByte(std::vector<BYTE>& buf, BYTE b) { buf.push_back(b); }
static void pushString(std::vector<BYTE>& buf, const std::string& str) {
    pushWord(buf, str.length());
    for(char c : str) buf.push_back(c);
}

void SendItemRefresh(SOCKET clientSocket, DWORD dwItemID, BYTE bSackID, BYTE bSackPos) {
    std::string q = "SELECT I.wVisualID, I.bType, I.bKind, I.wLevel, I.dwCost, D.nData18, D.nData19, I.wRefID, I.wAmount, ISNULL(D.nData1, -9999), ISNULL(D.nData2, -9999), ISNULL(D.nData3, -9999), ISNULL(D.nData4, -9999), ISNULL(D.nData5, -9999), ISNULL(D.nData6, -9999), ISNULL(D.nData7, -9999), ISNULL(D.nData8, -9999), ISNULL(D.nData9, -9999), ISNULL(D.nData10, -9999), ISNULL(D.nData11, -9999), ISNULL(D.nData12, -9999), ISNULL(D.nData13, -9999), ISNULL(D.nData14, -9999), ISNULL(D.nData15, -9999), ISNULL(D.nData16, -9999), ISNULL(D.nData17, -9999), ISNULL(D.nData20, 0), ISNULL(D.nData21, 0), ISNULL(D.nData25, 0), I.szName FROM ITEM I LEFT JOIN ITEMDATA D ON I.dwItemID=D.dwItemID WHERE I.dwItemID=" + std::to_string(dwItemID);
    
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        int vis=0, type=0, kind=0, lvl=0, cost=0, dat18=0, dat19=0, dat20=0, dat21=0, dat25=0, refid=0, amount=0;
        int d1=0, d2=0, d3=0, d4=0, d5=0, d6=0, d7=0, d8=0, d9=0, d10=0, d11=0, d12=0, d13=0, d14=0, d15=0, d16=0, d17=0; SQLLEN c[30]={0};
        char szName[128]={0};
        SQLGetData(hStmt, 1, SQL_C_SLONG, &vis, 0, &c[0]); SQLGetData(hStmt, 2, SQL_C_SLONG, &type, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &kind, 0, &c[2]); SQLGetData(hStmt, 4, SQL_C_SLONG, &lvl, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &cost, 0, &c[4]); SQLGetData(hStmt, 6, SQL_C_SLONG, &dat18, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &dat19, 0, &c[6]); SQLGetData(hStmt, 8, SQL_C_SLONG, &refid, 0, &c[7]);
        SQLGetData(hStmt, 9, SQL_C_SLONG, &amount, 0, &c[8]); SQLGetData(hStmt, 10, SQL_C_SLONG, &d1, 0, &c[9]);
        SQLGetData(hStmt, 11, SQL_C_SLONG, &d2, 0, &c[10]); SQLGetData(hStmt, 12, SQL_C_SLONG, &d3, 0, &c[11]);
        SQLGetData(hStmt, 13, SQL_C_SLONG, &d4, 0, &c[12]); SQLGetData(hStmt, 14, SQL_C_SLONG, &d5, 0, &c[13]);
        SQLGetData(hStmt, 15, SQL_C_SLONG, &d6, 0, &c[14]); SQLGetData(hStmt, 16, SQL_C_SLONG, &d7, 0, &c[15]);
        SQLGetData(hStmt, 17, SQL_C_SLONG, &d8, 0, &c[16]); SQLGetData(hStmt, 18, SQL_C_SLONG, &d9, 0, &c[17]);
        SQLGetData(hStmt, 19, SQL_C_SLONG, &d10, 0, &c[18]); SQLGetData(hStmt, 20, SQL_C_SLONG, &d11, 0, &c[19]);
        SQLGetData(hStmt, 21, SQL_C_SLONG, &d12, 0, &c[20]); SQLGetData(hStmt, 22, SQL_C_SLONG, &d13, 0, &c[21]);
        SQLGetData(hStmt, 23, SQL_C_SLONG, &d14, 0, &c[22]); SQLGetData(hStmt, 24, SQL_C_SLONG, &d15, 0, &c[23]);
        SQLGetData(hStmt, 25, SQL_C_SLONG, &d16, 0, &c[24]); SQLGetData(hStmt, 26, SQL_C_SLONG, &d17, 0, &c[25]);
        SQLGetData(hStmt, 27, SQL_C_SLONG, &dat20, 0, &c[26]); SQLGetData(hStmt, 28, SQL_C_SLONG, &dat21, 0, &c[27]);
        SQLGetData(hStmt, 29, SQL_C_SLONG, &dat25, 0, &c[28]); SQLGetData(hStmt, 30, SQL_C_CHAR, szName, sizeof(szName), &c[29]);

        int nd1=0, nd2=0, nd3=0, nd4=0, nd5=0;
        BYTE charType = 1;
        if (g_ItemTemplates.count(refid)) {
            if (type == 0) type = g_ItemTemplates[refid].bType;
            if (kind == 0) kind = g_ItemTemplates[refid].bKind;
            if (vis == 0) vis = g_ItemTemplates[refid].wVisualID;
            if (lvl == 0) lvl = g_ItemTemplates[refid].wLevel;
            if (cost == 0) cost = g_ItemTemplates[refid].dwCost;
            if (amount == 0) amount = g_ItemTemplates[refid].wAmount;
            charType = g_ItemTemplates[refid].bCharType;
            nd1 = g_ItemTemplates[refid].nBasicData1;
            nd2 = g_ItemTemplates[refid].nBasicData2;
            nd3 = g_ItemTemplates[refid].nBasicData3;
            nd4 = g_ItemTemplates[refid].nBasicData4;
            nd5 = g_ItemTemplates[refid].nBasicData5;
            if (d1 == -9999) d1 = g_ItemTemplates[refid].nData1;
            if (d2 == -9999) d2 = g_ItemTemplates[refid].nData2;
            if (d3 == -9999) d3 = g_ItemTemplates[refid].nData3;
            if (d4 == -9999) d4 = g_ItemTemplates[refid].nData4;
            if (d5 == -9999) d5 = g_ItemTemplates[refid].nData5;
            if (d6 == -9999) d6 = g_ItemTemplates[refid].nData6;
            if (d7 == -9999) d7 = g_ItemTemplates[refid].nData7;
            if (d8 == -9999) d8 = g_ItemTemplates[refid].nData8;
            if (d9 == -9999) d9 = g_ItemTemplates[refid].nData9;
            if (d10 == -9999) d10 = g_ItemTemplates[refid].nData10;
        }
        if (d1 == -9999) d1 = 0; if (d2 == -9999) d2 = 0; if (d3 == -9999) d3 = 0;
        if (d4 == -9999) d4 = 0; if (d5 == -9999) d5 = 0; if (d6 == -9999) d6 = 0;
        if (d7 == -9999) d7 = 0; if (d8 == -9999) d8 = 0; if (d9 == -9999) d9 = 0;
        if (d10 == -9999) d10 = 0; if (d11 == -9999) d11 = 0; if (d12 == -9999) d12 = 0;
        if (d13 == -9999) d13 = 0; if (d14 == -9999) d14 = 0; if (d15 == -9999) d15 = 0;
        if (d16 == -9999) d16 = 0; if (d17 == -9999) d17 = 0;

        std::vector<BYTE> bi;
        bi.resize(4); // header
        pushByte(bi, bSackID);
        pushByte(bi, bSackPos);
        pushDWord(bi, dwItemID); pushWord(bi, refid);
        pushByte(bi, type); pushByte(bi, kind); pushWord(bi, vis);
        std::string itemName(szName);
        if (itemName.empty() && g_ItemTemplates.count(refid)) itemName = g_ItemTemplates[refid].szName;
        pushWord(bi, itemName.length());
        for (char ch : itemName) pushByte(bi, ch);
        pushDWord(bi, cost); pushWord(bi, lvl); pushByte(bi, charType);
        pushWord(bi, amount);
        
        if (type >= 1 && type <= 9) {
            pushWord(bi, nd1); pushWord(bi, nd2); pushWord(bi, nd3); pushWord(bi, nd4); pushWord(bi, nd5);
            pushByte(bi, d1);
            pushWord(bi, d2); pushWord(bi, d3);
            pushWord(bi, d4); pushWord(bi, d5); pushWord(bi, d6); pushWord(bi, d7); pushWord(bi, d8);
            pushWord(bi, d9); pushWord(bi, d10); pushWord(bi, d11); pushWord(bi, d12); pushWord(bi, d13);
            pushByte(bi, dat18); pushByte(bi, dat19);
            pushByte(bi, d14); pushByte(bi, d15); pushByte(bi, d16); pushByte(bi, d17);
            if (type == 9) { pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); }
            else if (type == 8) { for(int i=0;i<8;i++) pushByte(bi, 0); }
            else { pushByte(bi, 0); }
            if (type >= 1 && type <= 4) { pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushWord(bi, dat20); }
        } else {
            // Include MugongManager manually if type 21 drops, but rebuild doesn't apply to skills so we just write zeros for simplicity
            switch (type) {
                case 11: case 12: case 13: case 14: case 17: pushByte(bi, 0); pushWord(bi, d2); pushWord(bi, d3); break;
                case 15: pushByte(bi, d1); pushWord(bi, d2); pushWord(bi, d3); pushByte(bi, 0); pushByte(bi, 0); break;
                case 16: pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); break;
                case 18: pushByte(bi, 0); pushDWord(bi, 0); pushWord(bi, d2); pushWord(bi, d3); pushByte(bi, 0); break;
                case 19: pushWord(bi, 0); pushWord(bi, 0); break;
                case 20: pushByte(bi, 0); pushDWord(bi, 0); break;
                case 21: pushWord(bi, lvl); pushDWord(bi, nd2); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 1); break;
                case 22: pushDWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                case 23: pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
                case 25: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                case 27: pushByte(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
                case 29: pushWord(bi, 0); pushWord(bi, 0); break;
                case 32: pushWord(bi, 0); pushWord(bi, d2); pushWord(bi, d3); pushDWord(bi, 0); break;
                case 31: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                case 34: pushDWord(bi, 0); pushByte(bi, 0); break;
            }
        }
        
        // wRebuithValue - client reads this WORD after GetItemData()
        pushWord(bi, (WORD)dat20);
        
        // Force UI redraw by removing and re-adding
        std::vector<BYTE> rmBuf(7);
        PACKET_HEADER* rmHead = (PACKET_HEADER*)rmBuf.data();
        rmHead->id = 0x4208; // CS_IM_REMOVEFROMSACK_ACK
        rmHead->payloadSize = 3;
        rmBuf[4] = bSackID; rmBuf[5] = bSackPos; rmBuf[6] = 2;
        EncryptPacket(rmBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)rmBuf.data(), rmBuf.size(), 0);

        PACKET_HEADER* head = (PACKET_HEADER*)bi.data();
        head->id = 0x420A; // CS_IM_ADDONSACK_ACK
        head->payloadSize = (WORD)(bi.size() - 4);
        EncryptPacket(bi.data(), 0x42);
        SafeSend(clientSocket, (const char*)bi.data(), bi.size(), 0);
    });
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
    int currentRebuild = 0, currentAppend = 0, currentAttempts = 0, itemType = 0, reqLevel = 1;
    int baseData1 = 0, baseData2 = 0, baseData3 = 0, baseData4 = 0, baseData5 = 0;
    
    std::string qItem = "SELECT I.szName, ISNULL(D.nData14, 0), ISNULL(D.nData15, 0), I.bType, T.nBasicData1, T.szName, ISNULL(T.nData1, 0), ISNULL(T.nData3, 0), ISNULL(T.nData5, 0), ISNULL(T.nData2, 0), ISNULL(T.nData4, 0), ISNULL(D.nData17, 0) FROM SACKITEM S JOIN ITEM I ON S.dwItemID = I.dwItemID LEFT JOIN ITEMDATA D ON S.dwItemID = D.dwItemID JOIN ITEMTEMPLATE T ON I.wRefID = T.wRefID WHERE S.dwCharID = " + std::to_string(charID) + " AND S.dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteQuery(qItem, [&](SQLHSTMT hStmt) {
        char szName[128] = {0}, szBaseName[128] = {0};
        SQLLEN c[12];
        SQLGetData(hStmt, 1, SQL_C_CHAR, szName, sizeof(szName), &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &currentRebuild, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &currentAppend, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &itemType, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &reqLevel, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_CHAR, szBaseName, sizeof(szBaseName), &c[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &baseData1, 0, &c[6]);
        SQLGetData(hStmt, 8, SQL_C_SLONG, &baseData3, 0, &c[7]);
        SQLGetData(hStmt, 9, SQL_C_SLONG, &baseData5, 0, &c[8]);
        SQLGetData(hStmt, 10, SQL_C_SLONG, &baseData2, 0, &c[9]);
        SQLGetData(hStmt, 11, SQL_C_SLONG, &baseData4, 0, &c[10]);
        SQLGetData(hStmt, 12, SQL_C_SLONG, &currentAttempts, 0, &c[11]);
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
    
    DBHelper::GetInstance().ExecuteUpdate("UPDATE CHAR_DATA SET dwMoney = dwMoney - " + std::to_string(rebuildCost) + " WHERE dwCharID = " + std::to_string(charID));
    DBHelper::GetInstance().ExecuteQuery("SELECT dwMoney FROM CHAR_DATA WHERE dwCharID = " + std::to_string(charID), [&](SQLHSTMT hStmt) {
        INT64 currentMoney = 0; SQLLEN cb;
        SQLGetData(hStmt, 1, SQL_C_SBIGINT, &currentMoney, 0, &cb);
        std::vector<BYTE> moneyBuf(13); PACKET_HEADER* mHead = (PACKET_HEADER*)moneyBuf.data();
        mHead->id = 0x3B13; mHead->payloadSize = 9;
        *((INT64*)(moneyBuf.data() + 4)) = (INT64)currentMoney; moneyBuf[12] = 0;
        EncryptPacket(moneyBuf.data(), 0x42); SafeSend(clientSocket, (const char*)moneyBuf.data(), moneyBuf.size(), 0);
    });
    
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
                DBHelper::GetInstance().ExecuteUpdate("DELETE FROM SACKITEM WHERE dwItemID = " + resIdStr);
                DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEMDATA WHERE dwItemID = " + resIdStr);
                DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEM WHERE dwItemID = " + resIdStr);
                std::vector<BYTE> ackBuf(4); ackBuf.push_back(bResourceSackID[i]); ackBuf.push_back(bResourcePos[i]);
                pushDWord(ackBuf, dwResourceID[i]); ackBuf.push_back(0);
                PACKET_HEADER* ah = (PACKET_HEADER*)ackBuf.data(); ah->id = 0x4208; ah->payloadSize = ackBuf.size() - 4;
                EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (char*)ackBuf.data(), ackBuf.size(), 0);
            }
        }
    }

    int targetLevel = (crystalKind == 1) ? (currentRebuild + 1) : (currentAppend + 1);
    if (targetLevel > 99) return;

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

    if (!configFound) return;

    int finalSuccessRate = baseRate + crystalSuccessRate;
    bool isSuccess = (rand() % 100) < finalSuccessRate;
    bool didBreak = false;
    bool hasAntiBreak = false; 
    currentAttempts++;

    if (isSuccess) {
        if (crystalKind == 1) currentRebuild = targetLevel;
        else currentAppend = targetLevel;
    } else {
        if (crystalKind == 1) {
            if (!hasAntiBreak && (rand() % 100) < breakChance) didBreak = true;
        } else {
            if (currentAppend > 0) currentAppend--; 
        }
    }

    std::string newName = baseItemName;
    if (currentRebuild > 0) newName = "+" + std::to_string(currentRebuild) + " " + newName;
    if (currentAppend > 0) newName += " (" + std::to_string(currentAppend) + "\xD0\xD4\xD6\xCA)";
    if (didBreak) newName = itemName;

    // =====================================================================
    // PHASE 1: Do ALL database operations FIRST (no network sends yet)
    // =====================================================================
    
    // 1a. Process resource consumption in DB and prepare remove packets
    struct ResourceRemoveInfo {
        std::vector<BYTE> packet;
        bool valid;
    };
    ResourceRemoveInfo resRemoves[3] = {};
    
    for (int i=0; i<3; i++) {
        resRemoves[i].valid = false;
        if (dwResourceID[i] != 0) {
            std::string resIdStr = std::to_string(dwResourceID[i]);
            bool amountDecreased = false;
            DBHelper::GetInstance().ExecuteQuery("SELECT wAmount FROM ITEM WHERE dwItemID = " + resIdStr, [&](SQLHSTMT hStmt) {
                int amount = 0; SQLLEN c;
                SQLGetData(hStmt, 1, SQL_C_SLONG, &amount, 0, &c);
                if (amount > 1) {
                    amountDecreased = true;
                    DBHelper::GetInstance().ExecuteUpdate("UPDATE ITEM SET wAmount = wAmount - 1 WHERE dwItemID = " + resIdStr);
                    // Prepare 0x4216 packet (amount update)
                    std::vector<BYTE> amtBuf(4); amtBuf.push_back(bResourceSackID[i]); amtBuf.push_back(bResourcePos[i]);
                    pushDWord(amtBuf, dwResourceID[i]); pushWord(amtBuf, amount - 1);
                    PACKET_HEADER* ah = (PACKET_HEADER*)amtBuf.data(); ah->id = 0x4216; ah->payloadSize = amtBuf.size() - 4;
                    EncryptPacket(amtBuf.data(), 0x42);
                    resRemoves[i].packet = amtBuf;
                    resRemoves[i].valid = true;
                }
            });
            if (!amountDecreased) {
                DBHelper::GetInstance().ExecuteUpdate("DELETE FROM SACKITEM WHERE dwItemID = " + resIdStr);
                DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEMDATA WHERE dwItemID = " + resIdStr);
                DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEM WHERE dwItemID = " + resIdStr);
                // Prepare 0x4208 packet (full remove)
                std::vector<BYTE> rmBuf(7); PACKET_HEADER* ah = (PACKET_HEADER*)rmBuf.data();
                ah->id = 0x4208; ah->payloadSize = 3;
                rmBuf[4] = bResourceSackID[i]; rmBuf[5] = bResourcePos[i]; rmBuf[6] = 2;
                EncryptPacket(rmBuf.data(), 0x42);
                resRemoves[i].packet = rmBuf;
                resRemoves[i].valid = true;
            }
        }
    }

    // 1b. Process item break or stat update in DB
    std::vector<BYTE> itemBreakPacket;
    bool hasItemBreakPacket = false;
    
    if (didBreak) {
        std::string idStr = std::to_string(dwItemID);
        DBHelper::GetInstance().ExecuteUpdate("DELETE FROM SACKITEM WHERE dwItemID = " + idStr);
        DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEMDATA WHERE dwItemID = " + idStr);
        DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEM WHERE dwItemID = " + idStr);
        
        itemBreakPacket.resize(7); PACKET_HEADER* rmHead = (PACKET_HEADER*)itemBreakPacket.data();
        rmHead->id = 0x4208; rmHead->payloadSize = 3;
        itemBreakPacket[4] = bSackID; itemBreakPacket[5] = bSackPos; itemBreakPacket[6] = 2;
        EncryptPacket(itemBreakPacket.data(), 0x42);
        hasItemBreakPacket = true;
    } else {
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

        int finalData4 = baseData4;
        int finalData5 = baseData5;
        int finalData9 = 0;
        
        if (itemType == 1) { finalData4 += totalWujingBonus; } 
        else if (itemType >= 2 && itemType <= 4) { finalData5 += totalWujingBonus; finalData9 += totalSujingBonus; } 
        else { finalData5 += totalWujingBonus; finalData4 += totalSujingBonus; }

        DBHelper::GetInstance().ExecuteUpdate("IF EXISTS (SELECT 1 FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwItemID) + ") "
            "UPDATE ITEMDATA SET nData4 = " + std::to_string(finalData4) + ", nData5 = " + std::to_string(finalData5) + 
            ", nData9 = " + std::to_string(finalData9) + ", nData14 = " + std::to_string(currentRebuild) + 
            ", nData15 = " + std::to_string(currentAppend) + ", nData17 = " + std::to_string(currentAttempts) +
            " WHERE dwItemID = " + std::to_string(dwItemID) + " "
            "ELSE INSERT INTO ITEMDATA (dwItemID, nData4, nData5, nData9, nData14, nData15, nData17) VALUES (" + 
            std::to_string(dwItemID) + ", " + std::to_string(finalData4) + ", " + std::to_string(finalData5) + 
            ", " + std::to_string(finalData9) + ", " + std::to_string(currentRebuild) + ", " + std::to_string(currentAppend) + ", " + std::to_string(currentAttempts) + ")");

        DBHelper::GetInstance().ExecuteUpdate("UPDATE ITEM SET szName = '" + newName + "' WHERE dwItemID = " + std::to_string(dwItemID));
    }

    // =====================================================================
    // PHASE 2: Send ALL packets in rapid succession (no DB ops between sends)
    // This ensures all packets land in the same TCP segment / recv buffer,
    // so the client processes them in a single frame with ZERO visual flicker.
    // =====================================================================
    
    // 2a. Send 0x4244 (rebuild result) - triggers HideSack which dumps items back to sack
    std::vector<BYTE> ackBuf; ackBuf.resize(4);
    if (didBreak) {
        ackBuf.push_back(5); ackBuf.push_back(255);
        pushDWord(ackBuf, 0); pushString(ackBuf, itemName); ackBuf.push_back(currentRebuild);
    } else if (isSuccess) {
        ackBuf.push_back(0); // SUCCESS
        ackBuf.push_back(255);
        pushDWord(ackBuf, 0); pushString(ackBuf, newName); ackBuf.push_back(currentRebuild);
    } else {
        ackBuf.push_back(5); // FAIL
        ackBuf.push_back(255);
        pushDWord(ackBuf, 0); pushString(ackBuf, newName); ackBuf.push_back(currentRebuild);
    }
    
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4244;
    head->payloadSize = (WORD)(ackBuf.size() - 4);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);

    // 2b. IMMEDIATELY send resource remove/update packets (same frame)
    for (int i=0; i<3; i++) {
        if (resRemoves[i].valid) {
            SafeSend(clientSocket, (const char*)resRemoves[i].packet.data(), resRemoves[i].packet.size(), 0);
        }
    }

    // 2c. Send item break or refresh (same frame)
    if (hasItemBreakPacket) {
        SafeSend(clientSocket, (const char*)itemBreakPacket.data(), itemBreakPacket.size(), 0);
    } else {
        SendItemRefresh(clientSocket, dwItemID, bSackID, bSackPos);
    }
}

void RegisterRebuildItemHandlers() {
    RegisterHandler(0x4241, [](SOCKET s, BYTE* p, WORD sz) { OnRebuildItemTermReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x4243, [](SOCKET s, BYTE* p, WORD sz) { OnRebuildItemReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
}

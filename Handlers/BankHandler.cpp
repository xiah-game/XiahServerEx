#include "BankHandler.h"
#include "../GameObjects/MugongManager.h"
#include <set>

extern std::map<WORD, sItemTemplate> g_ItemTemplates;

static void pushByte(std::vector<BYTE>& buf, BYTE b) { buf.push_back(b); }
static void pushWord(std::vector<BYTE>& buf, WORD w) { buf.push_back(w & 0xFF); buf.push_back((w >> 8) & 0xFF); }
static void pushDWord(std::vector<BYTE>& buf, DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back(d >> 24); }

static void SendBankOrMallList(SOCKET clientSocket, DWORD charID, WORD opCodeACK, bool isBank) {
    std::string account = SessionMgr::GetInstance().GetAccount(clientSocket);
    if (account.empty()) {
        std::string qAcc = "SELECT szAccount FROM CHAR_ACCOUNT WHERE dwCharID = " + std::to_string(charID);
        DBHelper::GetInstance().ExecuteQuery(qAcc, [&](SQLHSTMT hStmt) {
            char buf[64] = {0}; SQLLEN cb;
            if (SQL_SUCCEEDED(SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &cb)) && cb != SQL_NULL_DATA) {
                account = buf;
            }
        });
    }

    std::vector<BYTE> ackBuf; 
    ackBuf.resize(4); 
    
    if (isBank) {
        pushDWord(ackBuf, 1); // BOOL bflag = TRUE (4 bytes)
        pushByte(ackBuf, 1);  // BYTE bAction = ACT_ITEMLISTINBANK_NORMALOPEN (1 byte)
    }

    std::vector<std::vector<BYTE>> items;
    std::string tableName = isBank ? "BANKITEM" : "MALLITEM";
    std::string posColumn = "bSackPos";

    if (!account.empty()) {
        std::string q = "SELECT S." + posColumn + ", S.dwItemID, I.wVisualID, I.bType, I.bKind, I.wLevel, I.dwCost, D.nData18, D.nData19, I.wRefID, I.wAmount, ISNULL(D.nData1, -9999), ISNULL(D.nData2, -9999), ISNULL(D.nData3, -9999), ISNULL(D.nData4, -9999), ISNULL(D.nData5, -9999), ISNULL(D.nData6, -9999), ISNULL(D.nData7, -9999), ISNULL(D.nData8, -9999), ISNULL(D.nData9, -9999), ISNULL(D.nData10, -9999), ISNULL(D.nData11, -9999), ISNULL(D.nData12, -9999), ISNULL(D.nData13, -9999), ISNULL(D.nData14, -9999), ISNULL(D.nData15, -9999), ISNULL(D.nData16, -9999), ISNULL(D.nData17, -9999), ISNULL(D.nData20, 0), ISNULL(D.nData21, 0), ISNULL(D.nData25, 0), I.szName FROM " + tableName + " S JOIN ITEM I ON S.dwItemID=I.dwItemID LEFT JOIN ITEMDATA D ON S.dwItemID=D.dwItemID WHERE S.szAccount='" + account + "'";
        
        auto itemCallback = [&](SQLHSTMT hStmt) {
            int pos = 0, itemid = 0, vis = 0, type = 0, kind = 0, lvl = 0, cost = 0, dat18 = 0, dat19 = 0, dat20 = 0, dat21 = 0, dat25 = 0, refid = 0, amount = 0;
            int d1 = 0, d2 = 0, d3 = 0, d4 = 0, d5 = 0, d6 = 0, d7 = 0, d8 = 0, d9 = 0, d10 = 0, d11 = 0, d12 = 0, d13 = 0, d14 = 0, d15 = 0, d16 = 0, d17 = 0; SQLLEN c[32] = {0};
            char szName[128] = {0};
            SQLGetData(hStmt, 1, SQL_C_SLONG, &pos, 0, &c[0]); SQLGetData(hStmt, 2, SQL_C_SLONG, &itemid, 0, &c[1]);
            SQLGetData(hStmt, 3, SQL_C_SLONG, &vis, 0, &c[2]); SQLGetData(hStmt, 4, SQL_C_SLONG, &type, 0, &c[3]);
            SQLGetData(hStmt, 5, SQL_C_SLONG, &kind, 0, &c[4]); SQLGetData(hStmt, 6, SQL_C_SLONG, &lvl, 0, &c[5]);
            SQLGetData(hStmt, 7, SQL_C_SLONG, &cost, 0, &c[6]); SQLGetData(hStmt, 8, SQL_C_SLONG, &dat18, 0, &c[7]);
            SQLGetData(hStmt, 9, SQL_C_SLONG, &dat19, 0, &c[8]); SQLGetData(hStmt, 10, SQL_C_SLONG, &refid, 0, &c[9]);
            SQLGetData(hStmt, 11, SQL_C_SLONG, &amount, 0, &c[10]);
            SQLGetData(hStmt, 12, SQL_C_SLONG, &d1, 0, &c[11]); SQLGetData(hStmt, 13, SQL_C_SLONG, &d2, 0, &c[12]);
            SQLGetData(hStmt, 14, SQL_C_SLONG, &d3, 0, &c[13]); SQLGetData(hStmt, 15, SQL_C_SLONG, &d4, 0, &c[14]);
            SQLGetData(hStmt, 16, SQL_C_SLONG, &d5, 0, &c[15]); SQLGetData(hStmt, 17, SQL_C_SLONG, &d6, 0, &c[16]);
            SQLGetData(hStmt, 18, SQL_C_SLONG, &d7, 0, &c[17]); SQLGetData(hStmt, 19, SQL_C_SLONG, &d8, 0, &c[18]);
            SQLGetData(hStmt, 20, SQL_C_SLONG, &d9, 0, &c[19]); SQLGetData(hStmt, 21, SQL_C_SLONG, &d10, 0, &c[20]);
            SQLGetData(hStmt, 22, SQL_C_SLONG, &d11, 0, &c[21]); SQLGetData(hStmt, 23, SQL_C_SLONG, &d12, 0, &c[22]);
            SQLGetData(hStmt, 24, SQL_C_SLONG, &d13, 0, &c[23]); SQLGetData(hStmt, 25, SQL_C_SLONG, &d14, 0, &c[24]);
            SQLGetData(hStmt, 26, SQL_C_SLONG, &d15, 0, &c[25]); SQLGetData(hStmt, 27, SQL_C_SLONG, &d16, 0, &c[26]);
            SQLGetData(hStmt, 28, SQL_C_SLONG, &d17, 0, &c[27]);
            SQLGetData(hStmt, 29, SQL_C_SLONG, &dat20, 0, &c[28]); SQLGetData(hStmt, 30, SQL_C_SLONG, &dat21, 0, &c[29]);
            SQLGetData(hStmt, 31, SQL_C_SLONG, &dat25, 0, &c[30]);
            SQLGetData(hStmt, 32, SQL_C_CHAR, szName, sizeof(szName), &c[31]);
            
            int nd1 = 0, nd2 = 0, nd3 = 0, nd4 = 0, nd5 = 0;
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
            pushByte(bi, pos);
            pushDWord(bi, itemid); pushWord(bi, refid);
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
                
                if (type == 9) { 
                    pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0);
                } else if (type == 8) { 
                    pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0);
                } else {
                    pushByte(bi, 0); 
                }
                if (type >= 1 && type <= 4) { pushByte(bi, dat19); pushByte(bi, dat20); pushByte(bi, dat21); pushWord(bi, dat25); } 
            } else {
                switch (type) {
                    case 11: case 12: case 13: case 14: case 17:
                        pushByte(bi, 0); pushWord(bi, d2); pushWord(bi, d3); break;
                    case 15:
                        pushByte(bi, d1); pushWord(bi, d2); pushWord(bi, d3); pushByte(bi, 0); pushByte(bi, 0); break;
                    case 16:
                        pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); break;
                    case 18:
                        pushByte(bi, 0); pushDWord(bi, 0); pushWord(bi, d2); pushWord(bi, d3); pushByte(bi, 0); break;
                    case 19:
                        pushWord(bi, 0); pushWord(bi, 0); break;
                    case 20:
                        pushByte(bi, 0); pushDWord(bi, 0); break;
                    case 21: {
                        DWORD mid = nd2;
                        sMugongTemplate* mg = MugongManager::GetInstance()->GetTemplate(mid);
                        BYTE typeVal = mg ? mg->bType : 0;
                        BYTE kindVal = mg ? mg->bKind : 0;
                        pushWord(bi, lvl); pushDWord(bi, mid); pushByte(bi, typeVal); pushByte(bi, kindVal); pushByte(bi, 1); break;
                    }
                    case 22:
                        pushDWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                    case 23:
                        pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
                    case 25:
                        pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                    case 27:
                        pushByte(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
                    case 29:
                        pushWord(bi, 0); pushWord(bi, 0); break;
                    case 32:
                        pushWord(bi, 0); pushWord(bi, d2); pushWord(bi, d3); pushDWord(bi, 0); break;
                    case 31:
                        pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                    case 34:
                        pushDWord(bi, 0); pushByte(bi, 0); break;
                }
            }
            pushWord(bi, (WORD)dat20); // wRebuithValue
            items.push_back(bi);
        };
        DBHelper::GetInstance().ExecuteQuery(q, itemCallback);
    }
    
    pushDWord(ackBuf, items.size());
    for (auto i : items) ackBuf.insert(ackBuf.end(), i.begin(), i.end());
    
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); 
    ackHead->id = opCodeACK; 
    ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42); 
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
}

void OnItemListInBankReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    LOG("[BankHandler] OnItemListInBankReq called for CharID " + std::to_string(charID));
    // CS_EC_ITEMLISTINBANK_ACK = 0x3D01 + 157 = 0x3D9E
    SendBankOrMallList(clientSocket, charID, 0x3D9E, true); 
}

void OnDrawInBankReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    // Stub
}

static void ProcessDrawOut(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize, bool isBank) {
    if (totalSize < 15) return;
    DWORD dwTargetCharID = *(DWORD*)(payload);
    DWORD dwItemID = *(DWORD*)(payload + 4);
    BYTE bBankPos = payload[8];
    BYTE bSackID = payload[9];
    BYTE bSackPos = payload[10];
    DWORD dwAmount = *(DWORD*)(payload + 11);

    std::string account = SessionMgr::GetInstance().GetAccount(clientSocket);
    if (account.empty()) {
        std::string qAcc = "SELECT szAccount FROM CHAR_ACCOUNT WHERE dwCharID = " + std::to_string(charID);
        DBHelper::GetInstance().ExecuteQuery(qAcc, [&](SQLHSTMT hStmt) {
            char buf[64] = {0}; SQLLEN cb;
            if (SQL_SUCCEEDED(SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &cb)) && cb != SQL_NULL_DATA) {
                account = buf;
            }
        });
    }
    
    if (account.empty()) return;

    BYTE result = 0; // Success
    
    if (bSackPos == 255) {
        BYTE bCX = 1, bCY = 1;
        std::string qItem = "SELECT wRefID FROM ITEM WHERE dwItemID = " + std::to_string(dwItemID);
        DBHelper::GetInstance().ExecuteQuery(qItem, [&](SQLHSTMT hStmt) {
            int refid = 0; SQLLEN c;
            if (SQL_SUCCEEDED(SQLGetData(hStmt, 1, SQL_C_SLONG, &refid, 0, &c))) {
                if (g_ItemTemplates.count(refid)) {
                    bCX = g_ItemTemplates[refid].bCX;
                    bCY = g_ItemTemplates[refid].bCY;
                }
            }
        });
        bSackPos = FindFreeSackPos(charID, bSackID, bCX, bCY);
        if (bSackPos == 255) result = 3; // ERR_DRAWOUTBANK_FULLSACK
    }

    if (result == 0) {
        std::string tableName = isBank ? "BANKITEM" : "MALLITEM";
        std::string qDel = "DELETE FROM " + tableName + " WHERE szAccount = '" + account + "' AND dwItemID = " + std::to_string(dwItemID);
        DBHelper::GetInstance().ExecuteQuery(qDel, nullptr);
        
        std::string qIns = "INSERT INTO SACKITEM (dwCharID, bSackID, bSackPos, dwItemID) VALUES (" + 
                           std::to_string(charID) + ", " + std::to_string(bSackID) + ", " + std::to_string(bSackPos) + ", " + std::to_string(dwItemID) + ")";
        DBHelper::GetInstance().ExecuteQuery(qIns, nullptr);
    }
    
    std::vector<BYTE> ack(5);
    PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
    head->id = isBank ? 0x3DA6 : 0x3D5D;
    head->payloadSize = 1;
    ack[4] = result;
    
    EncryptPacket(ack.data(), 0x42);
    SafeSend(clientSocket, (const char*)ack.data(), ack.size(), 0);
}

void OnDrawOutBankReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    LOG("[BankHandler] OnDrawOutBankReq CharID: " + std::to_string(charID));
    ProcessDrawOut(clientSocket, charID, payload, totalSize, true);
}

void OnDrawMoveBankReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    // Stub
}

void OnItemListInMallReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    LOG("[BankHandler] OnItemListInMallReq called for CharID " + std::to_string(charID));
    // CS_EC_ITEMLISTINMALL_ACK = 0x3D01 + 90 = 0x3D5B
    SendBankOrMallList(clientSocket, charID, 0x3D5B, false); 
}

void OnDrawOutMallReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    LOG("[BankHandler] OnDrawOutMallReq CharID: " + std::to_string(charID));
    ProcessDrawOut(clientSocket, charID, payload, totalSize, false);
}

void RegisterBankMallHandlers() {
    RegisterHandler(0x3D9D, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        OnItemListInBankReq(s, charID, p, size);
    });
    
    RegisterHandler(0x3D5A, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        OnItemListInMallReq(s, charID, p, size);
    });
}

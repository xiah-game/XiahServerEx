#include "BankHandler.h"
#include "../DB/CharacterDB.h"
#include "../DB/ItemDB.h"
#include "../GameObjects/MugongManager.h"
#include <set>

extern std::map<WORD, sItemTemplate> g_ItemTemplates;

static void pushByte(std::vector<BYTE>& buf, BYTE b) { buf.push_back(b); }
static void pushWord(std::vector<BYTE>& buf, WORD w) { buf.push_back(w & 0xFF); buf.push_back((w >> 8) & 0xFF); }
static void pushDWord(std::vector<BYTE>& buf, DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back(d >> 24); }

static void SendBankOrMallList(SOCKET clientSocket, DWORD charID, WORD opCodeACK, bool isBank) {
    std::string account = SessionMgr::GetInstance().GetAccount(clientSocket);
    if (account.empty()) {
        account = CharacterDB::GetInstance().GetAccountName(charID);
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
        std::vector<ItemDB::FullItemRow> rows;
        ItemDB::GetInstance().GetFullBankItems(account, tableName, rows);
        
        for (auto& row : rows) {
            int pos = row.bSackPos, itemid = row.dwItemID, vis = row.wVisualID, type = row.bType, kind = row.bKind;
            int lvl = row.wLevel, cost = row.dwCost, dat18 = row.nData18, dat19 = row.nData19;
            int refid = row.wRefID, amount = row.wAmount;
            int dat20 = row.nData20, dat21 = row.nData21, dat25 = row.nData25;
            int d1=row.d[0], d2=row.d[1], d3=row.d[2], d4=row.d[3], d5=row.d[4], d6=row.d[5], d7=row.d[6];
            int d8=row.d[7], d9=row.d[8], d10=row.d[9], d11=row.d[10], d12=row.d[11], d13=row.d[12];
            int d14=row.d[13], d15=row.d[14], d16=row.d[15], d17=row.d[16];
            char szName[128]; memcpy(szName, row.szName, sizeof(szName));
            
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

                nd1 = row.nBasicData1 != 0 ? row.nBasicData1 : g_ItemTemplates[refid].nBasicData1;
                nd2 = row.nBasicData2 != 0 ? row.nBasicData2 : g_ItemTemplates[refid].nBasicData2;
                nd3 = row.nBasicData3 != 0 ? row.nBasicData3 : g_ItemTemplates[refid].nBasicData3;
                nd4 = row.nBasicData4 != 0 ? row.nBasicData4 : g_ItemTemplates[refid].nBasicData4;
                nd5 = row.nBasicData5 != 0 ? row.nBasicData5 : g_ItemTemplates[refid].nBasicData5;
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
                pushDWord(bi, d4); pushDWord(bi, d5); pushDWord(bi, d6); pushWord(bi, d7); pushWord(bi, d8); 
                pushDWord(bi, d9); pushDWord(bi, d10); pushDWord(bi, d11); pushDWord(bi, d12); pushWord(bi, d13); 
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
                        pushDWord(bi, nd2); pushByte(bi, (BYTE)nd3); pushWord(bi, (WORD)nd4); pushWord(bi, (WORD)nd5); break;
                    case 23:
                        pushDWord(bi, 0); pushDWord(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
                    case 25:
                        pushByte(bi, 0); pushWord(bi, (WORD)nd2); pushWord(bi, (WORD)nd3); break;
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
            items.push_back(bi);
        }
    }
    
    pushDWord(ackBuf, items.size());
    LOG("[BankHandler] SendBankOrMallList: account=" + account + " itemCount=" + std::to_string(items.size()) + " isBank=" + std::to_string(isBank));
    for (size_t idx = 0; idx < items.size(); ++idx) {
        LOG("[BankHandler]   item[" + std::to_string(idx) + "] bytes=" + std::to_string(items[idx].size()) + " pos=" + std::to_string(items[idx][0]));
        ackBuf.insert(ackBuf.end(), items[idx].begin(), items[idx].end());
    }
    
    LOG("[BankHandler] Total packet size=" + std::to_string(ackBuf.size()) + " payloadSize=" + std::to_string(ackBuf.size() - 4));
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
    LOG("[BankHandler] OnDrawInBankReq CharID: " + std::to_string(charID));
    if (totalSize < 15) return;
    
    DWORD dwTargetCharID = *(DWORD*)(payload);
    DWORD dwItemID = *(DWORD*)(payload + 4);
    BYTE bSackID = payload[8];
    BYTE bSackPos = payload[9];
    BYTE bBankPos = payload[10];
    DWORD dwAmount = *(DWORD*)(payload + 11);

    std::string account = SessionMgr::GetInstance().GetAccount(clientSocket);
    if (account.empty()) return;

    BYTE result = 0; // Success

    if (bBankPos == 255) {
        // Find free bank pos (0-35)
        std::vector<BYTE> usedPos = ItemDB::GetInstance().GetBankUsedPositions(account);
        
        for (BYTE p = 0; p < 36; ++p) {
            if (std::find(usedPos.begin(), usedPos.end(), p) == usedPos.end()) {
                bBankPos = p;
                break;
            }
        }
        if (bBankPos == 255) result = 3; // Bank full
    } else {
        bool isOccupied = ItemDB::GetInstance().IsBankPosOccupied(account, bBankPos);
        if (isOccupied) result = 2; // Position occupied
    }

    if (result == 0) {
        bool itemOwned = ItemDB::GetInstance().IsSackItemOwned(charID, dwItemID);
        if (!itemOwned) result = 1; // Item not found
    }

    if (result == 0) {
        ItemDB::GetInstance().RemoveFromSack(charID, dwItemID);
        ItemDB::GetInstance().AddToBankByAccount(account, bBankPos, dwItemID);
    }

    // CS_EC_DRAWINBANK_ACK = 0x3DA4
    std::vector<BYTE> ack(5);
    PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
    head->id = 0x3DA4;
    head->payloadSize = 1;
    ack[4] = result;
    
    EncryptPacket(ack.data(), 0x42);
    SafeSend(clientSocket, (const char*)ack.data(), ack.size(), 0);

    // If successful, clear the cursor and add the item visually to the Bank grid.
    if (result == 0) {
        // Clear cursor (CS_EC_REMOVEFROMBANK_ACK = 0x3DA2)
        std::vector<BYTE> rmBuf(9);
        PACKET_HEADER* rmHead = (PACKET_HEADER*)rmBuf.data();
        rmHead->id = 0x3DA2;
        rmHead->payloadSize = 5;
        rmBuf[4] = bBankPos; // bSackPos
        *(DWORD*)(&rmBuf[5]) = dwItemID; // dwItemID
        EncryptPacket(rmBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)rmBuf.data(), rmBuf.size(), 0);

        // Add to Bank grid (CS_EC_ADDONBANK_ACK = 0x3DA0)
        ItemDB::FullItemRow row;
        if (ItemDB::GetInstance().GetFullItemData(dwItemID, row)) {
            int vis=row.wVisualID, type=row.bType, kind=row.bKind, lvl=row.wLevel, cost=row.dwCost;
            int dat18=row.nData18, dat19=row.nData19, dat20=row.nData20, dat21=row.nData21, dat25=row.nData25;
            int refid=row.wRefID, amount=row.wAmount;
            int d1=row.d[0], d2=row.d[1], d3=row.d[2], d4=row.d[3], d5=row.d[4], d6=row.d[5], d7=row.d[6];
            int d8=row.d[7], d9=row.d[8], d10=row.d[9], d11=row.d[10], d12=row.d[11], d13=row.d[12];
            int d14=row.d[13], d15=row.d[14], d16=row.d[15], d17=row.d[16];
            char szName[128]; memcpy(szName, row.szName, sizeof(szName));

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
                nd1 = row.nBasicData1 != 0 ? row.nBasicData1 : g_ItemTemplates[refid].nBasicData1;
                nd2 = row.nBasicData2 != 0 ? row.nBasicData2 : g_ItemTemplates[refid].nBasicData2;
                nd3 = row.nBasicData3 != 0 ? row.nBasicData3 : g_ItemTemplates[refid].nBasicData3;
                nd4 = row.nBasicData4 != 0 ? row.nBasicData4 : g_ItemTemplates[refid].nBasicData4;
                nd5 = row.nBasicData5 != 0 ? row.nBasicData5 : g_ItemTemplates[refid].nBasicData5;
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

            std::vector<BYTE> iBuf;
            iBuf.resize(4); // header
            pushByte(iBuf, 2); // bAction = ACT_ADDONBANK_ITEMMOVE
            pushByte(iBuf, bBankPos);
            // GetItemData format: dwItemID, wRefID, bType, bKind, wVisualID, szName, dwPrice, wLevel, bNeedCharType, wAmount
            pushDWord(iBuf, dwItemID); pushWord(iBuf, refid);
            pushByte(iBuf, type); pushByte(iBuf, kind); pushWord(iBuf, vis);
            std::string itemName(szName);
            if (itemName.empty() && g_ItemTemplates.count(refid)) itemName = g_ItemTemplates[refid].szName;
            pushWord(iBuf, itemName.length());
            for (char ch : itemName) pushByte(iBuf, ch);
            pushDWord(iBuf, cost); pushWord(iBuf, lvl); pushByte(iBuf, charType);
            pushWord(iBuf, amount);
            if (type >= 1 && type <= 9) {
                pushWord(iBuf, nd1); pushWord(iBuf, nd2); pushWord(iBuf, nd3); pushWord(iBuf, nd4); pushWord(iBuf, nd5);
                pushByte(iBuf, d1);
                pushWord(iBuf, d2); pushWord(iBuf, d3);
                pushDWord(iBuf, d4); pushDWord(iBuf, d5); pushDWord(iBuf, d6); pushWord(iBuf, d7); pushWord(iBuf, d8);
                pushDWord(iBuf, d9); pushDWord(iBuf, d10); pushDWord(iBuf, d11); pushDWord(iBuf, d12); pushWord(iBuf, d13);
                pushByte(iBuf, dat18); pushByte(iBuf, dat19);
                pushByte(iBuf, d14); pushByte(iBuf, d15); pushByte(iBuf, d16); pushByte(iBuf, d17);
                if (type == 9) { pushDWord(iBuf, 0); pushWord(iBuf, 0); pushWord(iBuf, 0); pushWord(iBuf, 0); pushWord(iBuf, 0); }
                else if (type == 8) { for(int i=0;i<8;i++) pushByte(iBuf, 0); }
                else { pushByte(iBuf, 0); }
                if (type >= 1 && type <= 4) { pushByte(iBuf, dat19); pushByte(iBuf, dat20); pushByte(iBuf, dat21); pushWord(iBuf, dat25); }
            } else {
                switch (type) {
                    case 11: case 12: case 13: case 14: case 17: pushByte(iBuf, 0); pushWord(iBuf, d2); pushWord(iBuf, d3); break;
                    case 15: pushByte(iBuf, d1); pushWord(iBuf, d2); pushWord(iBuf, d3); pushByte(iBuf, 0); pushByte(iBuf, 0); break;
                    case 16: pushWord(iBuf, 0); pushByte(iBuf, 0); pushWord(iBuf, 0); pushByte(iBuf, 0); pushWord(iBuf, 0); break;
                    case 18: pushByte(iBuf, 0); pushDWord(iBuf, 0); pushWord(iBuf, d2); pushWord(iBuf, d3); pushByte(iBuf, 0); break;
                    case 19: pushWord(iBuf, 0); pushWord(iBuf, 0); break;
                    case 20: pushByte(iBuf, 0); pushDWord(iBuf, 0); break;
                    case 21: { DWORD mid=nd2; sMugongTemplate* mg=MugongManager::GetInstance()->GetTemplate(mid); pushWord(iBuf, lvl); pushDWord(iBuf, mid); pushByte(iBuf, mg?mg->bType:0); pushByte(iBuf, mg?mg->bKind:0); pushByte(iBuf, 1); break; }
                    case 22: pushDWord(iBuf, nd2); pushByte(iBuf, (BYTE)nd3); pushWord(iBuf, (WORD)nd4); pushWord(iBuf, (WORD)nd5); break;
                    case 23: pushDWord(iBuf, 0); pushDWord(iBuf, 0); pushDWord(iBuf, 0); pushByte(iBuf, 0); pushByte(iBuf, 0); break;
                    case 25: pushByte(iBuf, 0); pushWord(iBuf, (WORD)nd2); pushWord(iBuf, (WORD)nd3); break;
                    case 27: pushByte(iBuf, 0); pushDWord(iBuf, 0); pushByte(iBuf, 0); pushByte(iBuf, 0); pushByte(iBuf, 0); pushByte(iBuf, 0); pushDWord(iBuf, 0); break;
                    case 29: pushWord(iBuf, 0); pushWord(iBuf, 0); break;
                    case 32: pushWord(iBuf, 0); pushWord(iBuf, d2); pushWord(iBuf, d3); pushDWord(iBuf, 0); break;
                    case 31: pushByte(iBuf, 0); pushWord(iBuf, 0); pushWord(iBuf, 0); break;
                    case 34: pushDWord(iBuf, 0); pushByte(iBuf, 0); break;
                }
            }
            
            PACKET_HEADER* iHead = (PACKET_HEADER*)iBuf.data();
            iHead->id = 0x3DA0;
            iHead->payloadSize = (WORD)(iBuf.size() - 4);
            EncryptPacket(iBuf.data(), 0x42);
            SafeSend(clientSocket, (const char*)iBuf.data(), iBuf.size(), 0);
            LOG("[BankHandler] DrawIn: Sent ADDONBANK_ACK for bankPos=" + std::to_string(bBankPos) + " type=" + std::to_string(type));
        }
    }
}

static void ProcessDrawOut(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize, bool isBank) {
    if (totalSize < 15) return;
    DWORD dwTargetCharID = *(DWORD*)(payload);
    DWORD dwItemID = *(DWORD*)(payload + 4);
    BYTE bBankPos = payload[8];
    BYTE bSackID = payload[9];
    BYTE bSackPos = payload[10];
    DWORD dwAmount = *(DWORD*)(payload + 11);

    // Convert from ObjectID (400M+) to DB CharID
    DWORD dbCharID = charID;
    if (dbCharID >= 400000000) dbCharID -= 400000000;

    LOG("[BankHandler] ProcessDrawOut: charID=" + std::to_string(charID) + " dbCharID=" + std::to_string(dbCharID) + " dwItemID=" + std::to_string(dwItemID) + " bBankPos=" + std::to_string(bBankPos) + " bSackID=" + std::to_string(bSackID) + " bSackPos=" + std::to_string(bSackPos) + " isBank=" + std::to_string(isBank));

    std::string account = SessionMgr::GetInstance().GetAccount(clientSocket);
    if (account.empty()) {
        account = CharacterDB::GetInstance().GetAccountName(charID);
    }
    
    if (account.empty()) return;

    BYTE result = 0; // Success
    BYTE absolutePos = bSackPos;
    
    if (bSackPos == 255) {
        BYTE bCX = 1, bCY = 1;
        { ItemDB::ItemBasicInfo ib;
          if (ItemDB::GetInstance().GetItemBasicInfo(dwItemID, ib) && g_ItemTemplates.count(ib.wRefID)) {
              bCX = g_ItemTemplates[ib.wRefID].bCX;
              bCY = g_ItemTemplates[ib.wRefID].bCY;
          }
        }
        bSackPos = FindFreeSackPos(charID, bSackID, bCX, bCY);
        LOG("[BankHandler] ProcessDrawOut: FindFreeSackPos returned " + std::to_string(bSackPos));
        absolutePos = bSackPos;
        if (bSackPos == 255) result = 3; // ERR_DRAWOUTBANK_FULLSACK
    } else {
        if (bSackID == 1) absolutePos = 20 + bSackPos;
        else if (bSackID == 2) absolutePos = 60 + bSackPos;
        else if (bSackID == 3) absolutePos = 100 + bSackPos;
    }

    if (result == 0) {
        std::string tableName = isBank ? "BANKITEM" : "MALLITEM";
        bool itemOwned = ItemDB::GetInstance().IsItemInStorage(tableName, account, dwItemID);
        if (!itemOwned) result = 1; // Item not found
        else {
            bool occ = ItemDB::GetInstance().IsSackPosOccupiedAbs(charID, absolutePos);
            if (occ) result = 2; // Position occupied
        }
    }

    if (result == 0) {
        if (isBank)
            ItemDB::GetInstance().RemoveFromBankByAccount(account, dwItemID);
        else
            ItemDB::GetInstance().RemoveFromMallByAccount(account, dwItemID);
        ItemDB::GetInstance().AddToSack(charID, absolutePos, dwItemID);
        LOG("[BankHandler] ProcessDrawOut: SUCCESS! Moved item " + std::to_string(dwItemID) + " to sackPos=" + std::to_string(absolutePos));
    }
    
    std::vector<BYTE> ack(5);
    PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
    head->id = isBank ? 0x3DA6 : 0x3D5D;
    head->payloadSize = 1;
    ack[4] = result;
    LOG("[BankHandler] ProcessDrawOut: result=" + std::to_string(result) + " absolutePos=" + std::to_string(absolutePos));
    
    EncryptPacket(ack.data(), 0x42);
    SafeSend(clientSocket, (const char*)ack.data(), ack.size(), 0);

    if (result == 0) {
        // Send REMOVEFROMBANK_ACK (0x3DA2) to clear item from bank UI
        std::vector<BYTE> rmBuf(9);
        PACKET_HEADER* rmHead = (PACKET_HEADER*)rmBuf.data();
        rmHead->id = 0x3DA2;
        rmHead->payloadSize = 5;
        rmBuf[4] = bBankPos;
        *(DWORD*)(&rmBuf[5]) = dwItemID;
        EncryptPacket(rmBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)rmBuf.data(), rmBuf.size(), 0);
        LOG("[BankHandler] ProcessDrawOut: Sent REMOVEFROMBANK_ACK for bankPos=" + std::to_string(bBankPos));

        // Send ADDONSACK_ACK (0x420A) to add item to backpack UI
        // Reuse the same serialization format as SendBankOrMallList but for single item
        BYTE displaySackID = bSackID;
        BYTE displaySackPos = bSackPos;
        
        // Query full item data
        ItemDB::FullItemRow row;
        if (ItemDB::GetInstance().GetFullItemData(dwItemID, row)) {
            int vis=row.wVisualID, type=row.bType, kind=row.bKind, lvl=row.wLevel, cost=row.dwCost;
            int dat18=row.nData18, dat19=row.nData19, dat20=row.nData20, dat21=row.nData21, dat25=row.nData25;
            int refid=row.wRefID, amount=row.wAmount;
            int d1=row.d[0], d2=row.d[1], d3=row.d[2], d4=row.d[3], d5=row.d[4], d6=row.d[5], d7=row.d[6];
            int d8=row.d[7], d9=row.d[8], d10=row.d[9], d11=row.d[10], d12=row.d[11], d13=row.d[12];
            int d14=row.d[13], d15=row.d[14], d16=row.d[15], d17=row.d[16];
            char szName[128]; memcpy(szName, row.szName, sizeof(szName));

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
                nd1 = row.nBasicData1 != 0 ? row.nBasicData1 : g_ItemTemplates[refid].nBasicData1;
                nd2 = row.nBasicData2 != 0 ? row.nBasicData2 : g_ItemTemplates[refid].nBasicData2;
                nd3 = row.nBasicData3 != 0 ? row.nBasicData3 : g_ItemTemplates[refid].nBasicData3;
                nd4 = row.nBasicData4 != 0 ? row.nBasicData4 : g_ItemTemplates[refid].nBasicData4;
                nd5 = row.nBasicData5 != 0 ? row.nBasicData5 : g_ItemTemplates[refid].nBasicData5;
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

            // Build AddOnSack packet: bSackID + bSackPos + GetItemData + wRebuithValue
            std::vector<BYTE> bi;
            bi.resize(4); // header
            pushByte(bi, displaySackID);
            pushByte(bi, displaySackPos);
            // GetItemData fields (same order as SendBankOrMallList)
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
                pushDWord(bi, d4); pushDWord(bi, d5); pushDWord(bi, d6); pushWord(bi, d7); pushWord(bi, d8);
                pushDWord(bi, d9); pushDWord(bi, d10); pushDWord(bi, d11); pushDWord(bi, d12); pushWord(bi, d13);
                pushByte(bi, dat18); pushByte(bi, dat19);
                pushByte(bi, d14); pushByte(bi, d15); pushByte(bi, d16); pushByte(bi, d17);
                if (type == 9) { pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); }
                else if (type == 8) { for(int i=0;i<8;i++) pushByte(bi, 0); }
                else { pushByte(bi, 0); }
                if (type >= 1 && type <= 4) { pushByte(bi, dat19); pushByte(bi, dat20); pushByte(bi, dat21); pushWord(bi, dat25); }
            } else {
                switch (type) {
                    case 11: case 12: case 13: case 14: case 17: pushByte(bi, 0); pushWord(bi, d2); pushWord(bi, d3); break;
                    case 15: pushByte(bi, d1); pushWord(bi, d2); pushWord(bi, d3); pushByte(bi, 0); pushByte(bi, 0); break;
                    case 16: pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); break;
                    case 18: pushByte(bi, 0); pushDWord(bi, 0); pushWord(bi, d2); pushWord(bi, d3); pushByte(bi, 0); break;
                    case 19: pushWord(bi, 0); pushWord(bi, 0); break;
                    case 20: pushByte(bi, 0); pushDWord(bi, 0); break;
                    case 21: { DWORD mid=nd2; sMugongTemplate* mg=MugongManager::GetInstance()->GetTemplate(mid); pushWord(bi, lvl); pushDWord(bi, mid); pushByte(bi, mg?mg->bType:0); pushByte(bi, mg?mg->bKind:0); pushByte(bi, 1); break; }
                    case 22: pushDWord(bi, nd2); pushByte(bi, (BYTE)nd3); pushWord(bi, (WORD)nd4); pushWord(bi, (WORD)nd5); break;
                    case 23: pushDWord(bi, 0); pushDWord(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
                    case 25: pushByte(bi, 0); pushWord(bi, (WORD)nd2); pushWord(bi, (WORD)nd3); break;
                    case 27: pushByte(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
                    case 29: pushWord(bi, 0); pushWord(bi, 0); break;
                    case 32: pushWord(bi, 0); pushWord(bi, d2); pushWord(bi, d3); pushDWord(bi, 0); break;
                    case 31: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                    case 34: pushDWord(bi, 0); pushByte(bi, 0); break;
                }
            }
            // ADDONSACK reads wRebuithValue AFTER GetItemData
            pushWord(bi, (WORD)dat25); // wRebuithValue - 觉醒值(nData25)
            
            PACKET_HEADER* addHead = (PACKET_HEADER*)bi.data();
            addHead->id = 0x420A;
            addHead->payloadSize = (WORD)(bi.size() - 4);
            EncryptPacket(bi.data(), 0x42);
            SafeSend(clientSocket, (const char*)bi.data(), bi.size(), 0);
            LOG("[BankHandler] ProcessDrawOut: Sent ADDONSACK_ACK for sackID=" + std::to_string(displaySackID) + " sackPos=" + std::to_string(displaySackPos));
        }
    }
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

    RegisterHandler(0x3DA5, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        OnDrawOutBankReq(s, charID, p, size);
    });

    RegisterHandler(0x3DA3, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        OnDrawInBankReq(s, charID, p, size);
    });

    RegisterHandler(0x3D5C, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        OnDrawOutMallReq(s, charID, p, size);
    });
}

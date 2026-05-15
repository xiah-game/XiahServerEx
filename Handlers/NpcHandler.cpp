#include "NpcHandler.h"
#include "../GameObjects/MugongManager.h"
#include <unordered_set>
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"

extern std::map<DWORD, CMapInstance*> g_MapInstances;

void OnNpcInfoReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    DWORD reqId = *(DWORD*)(payload);
    DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (g_MapInstances.count(pMapID)) {
        CMapInstance* mapInst = g_MapInstances[pMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetMonster(reqId);
        if (pObj && pObj->bObjectType == 3) {
            std::vector<BYTE> ackBuf; ackBuf.reserve(64);
            ackBuf.push_back(0); // bResult
            DWORD oid = pObj->dwObjectID; ackBuf.push_back(oid&0xFF); ackBuf.push_back((oid>>8)&0xFF); ackBuf.push_back((oid>>16)&0xFF); ackBuf.push_back(oid>>24);
            ackBuf.push_back(pObj->bPropType); // bNpcType
            DWORD mid = pObj->dwMapID; ackBuf.push_back(mid&0xFF); ackBuf.push_back((mid>>8)&0xFF); ackBuf.push_back((mid>>16)&0xFF); ackBuf.push_back(mid>>24);
            ackBuf.push_back(pObj->wPosX & 0xFF); ackBuf.push_back(pObj->wPosX >> 8);
            WORD wl = pObj->wLevel; ackBuf.push_back(wl&0xFF); ackBuf.push_back(wl>>8);
            ackBuf.push_back(pObj->wPosY & 0xFF); ackBuf.push_back(pObj->wPosY >> 8);
            ackBuf.push_back(pObj->bHeight);
            ackBuf.push_back(0); ackBuf.push_back(0); // wDirection
            WORD nlen = pObj->szName.length(); ackBuf.push_back(nlen & 0xFF); ackBuf.push_back(nlen >> 8);
            for (char c : pObj->szName) ackBuf.push_back(c); // szName
            ackBuf.push_back(1); // bState
            ackBuf.push_back(pObj->wPosX & 0xFF); ackBuf.push_back(pObj->wPosX >> 8); // DesPosX
            ackBuf.push_back(pObj->wPosY & 0xFF); ackBuf.push_back(pObj->wPosY >> 8); // DesPosY
            ackBuf.push_back(pObj->bHeight); // DesHeight
            DWORD hm = pObj->dwHpMax; ackBuf.push_back(hm&0xFF); ackBuf.push_back((hm>>8)&0xFF); ackBuf.push_back((hm>>16)&0xFF); ackBuf.push_back(hm>>24);
            DWORD hc = pObj->dwHpCur; ackBuf.push_back(hc&0xFF); ackBuf.push_back((hc>>8)&0xFF); ackBuf.push_back((hc>>16)&0xFF); ackBuf.push_back(hc>>24);
            ackBuf.push_back(pObj->bWalkSpeedByte); // bWalkSpeed
            ackBuf.push_back(pObj->bGroupOrder); // bOrderID
            ackBuf.push_back(255); // bSubType
            
            std::vector<BYTE> fullAck; fullAck.resize(4);
            fullAck.insert(fullAck.end(), ackBuf.begin(), ackBuf.end());
            PACKET_HEADER* ackHead = (PACKET_HEADER*)fullAck.data(); ackHead->id = 0x352c; ackHead->payloadSize = (WORD)ackBuf.size();
            EncryptPacket(fullAck.data(), 0x42); 
            SafeSend(clientSocket, (const char*)fullAck.data(), fullAck.size(), 0);
        }
    }
}

void OnNpcInfoListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    DWORD reqMapID = *(DWORD*)(payload);
    std::unordered_set<DWORD> reqIDs;
    if (totalSize >= 6) {
        WORD wCount = *(WORD*)(payload + 4);
        int actualCount = (totalSize - 6) / 4;
        int processCount = (wCount < actualCount) ? wCount : actualCount;
        for (int i = 0; i < processCount; i++) reqIDs.insert(*(DWORD*)(payload + 6 + i*4));
    }
    std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); // bResult
    ackBuf.push_back(reqMapID & 0xFF); ackBuf.push_back((reqMapID>>8)&0xFF); ackBuf.push_back((reqMapID>>16)&0xFF); ackBuf.push_back(reqMapID>>24); // dwMapID
    if (g_MapInstances.count(reqMapID)) {
        CMapInstance* mapInst = g_MapInstances[reqMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        WORD count = 0;
        for (auto& pair : mapInst->GetMonsters()) {
            auto& o = pair.second;
            if (o.bObjectType == 3 && reqIDs.count(o.dwObjectID)) count++;
        }
        ackBuf.push_back(count & 0xFF); ackBuf.push_back(count >> 8);
        for (auto& pair : mapInst->GetMonsters()) {
            auto& o = pair.second;
            if (o.bObjectType == 3 && reqIDs.count(o.dwObjectID)) {
                auto pushDWord = [&](DWORD d) { ackBuf.push_back(d & 0xFF); ackBuf.push_back((d>>8)&0xFF); ackBuf.push_back((d>>16)&0xFF); ackBuf.push_back((d>>24)&0xFF); };
                auto pushWord = [&](WORD w) { ackBuf.push_back(w & 0xFF); ackBuf.push_back((w>>8)&0xFF); };
                auto pushByte = [&](BYTE b) { ackBuf.push_back(b); };
                pushDWord(o.dwObjectID); pushByte(o.bPropType); pushWord(o.wPosX); pushWord(o.wLevel); pushWord(o.wPosY); pushByte(o.bHeight);
                pushWord(0); // wDirection
                WORD nlen = o.szName.length(); pushWord(nlen);
                for (char c : o.szName) pushByte(c); // szName
                pushByte(1); // bState
                pushWord(o.wPosX); pushWord(o.wPosY); pushByte(o.bHeight);
                pushDWord(o.dwHpMax); pushDWord(o.dwHpCur); pushByte(o.bWalkSpeedByte); pushByte(o.bGroupOrder); 
                pushByte(255); // bSubType
            }
        }
    } else {
        ackBuf.push_back(0); ackBuf.push_back(0);
    }
    PACKET_HEADER* ah = (PACKET_HEADER*)ackBuf.data(); ah->id = 0x352E; ah->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[NpcHandler] Spawning Monsters for Map " + std::to_string(reqMapID));
}

void OnFunctionalNpcInfoListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    DWORD reqMapID = *(DWORD*)(payload);
    std::unordered_set<DWORD> reqIDs;
    if (totalSize >= 6) {
        WORD wCount = *(WORD*)(payload + 4);
        int actualCount = (totalSize - 6) / 4;
        int processCount = (wCount < actualCount) ? wCount : actualCount;
        for (int i = 0; i < processCount; i++) reqIDs.insert(*(DWORD*)(payload + 6 + i*4));
    }
    std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); // bResult
    ackBuf.push_back(reqMapID & 0xFF); ackBuf.push_back((reqMapID>>8)&0xFF); ackBuf.push_back((reqMapID>>16)&0xFF); ackBuf.push_back(reqMapID>>24); // dwMapID
    if (g_MapInstances.count(reqMapID)) {
        CMapInstance* mapInst = g_MapInstances[reqMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        WORD count = 0;
        for (auto& pair : mapInst->GetMonsters()) {
            auto& o = pair.second;
            if (o.bObjectType == 5 && reqIDs.count(o.dwObjectID)) count++;
        }
        ackBuf.push_back(count & 0xFF); ackBuf.push_back(count >> 8);
        for (auto& pair : mapInst->GetMonsters()) {
            auto& o = pair.second;
            if (o.bObjectType == 5 && reqIDs.count(o.dwObjectID)) {
                auto pushDWord = [&](DWORD d) { ackBuf.push_back(d & 0xFF); ackBuf.push_back((d>>8)&0xFF); ackBuf.push_back((d>>16)&0xFF); ackBuf.push_back((d>>24)&0xFF); };
                auto pushWord = [&](WORD w) { ackBuf.push_back(w & 0xFF); ackBuf.push_back((w>>8)&0xFF); };
                auto pushByte = [&](BYTE b) { ackBuf.push_back(b); };
                pushDWord(o.dwObjectID); 
                pushByte(o.bPropType); pushByte(0); pushByte(0); 
                pushWord(o.wPosX); pushWord(o.wPosY); pushByte(o.bHeight); 
                pushWord(0); // wDirection
                WORD nlen = o.szName.length(); pushWord(nlen);
                for (char c : o.szName) pushByte(c); // szName
                pushByte(0); pushDWord(0); // owner info
                pushByte(1); pushByte(1); // sizes
                pushWord(0); // wNumItem
            }
        }
    } else {
        ackBuf.push_back(0); ackBuf.push_back(0);
    }
    PACKET_HEADER* ah = (PACKET_HEADER*)ackBuf.data(); ah->id = 0x3532; ah->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[NpcHandler] Spawning Friendly NPCs for Map " + std::to_string(reqMapID));
}

void OnFunctionalNpcItemListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 9) return;
    DWORD reqMapID = *(DWORD*)(payload);
    DWORD reqObjectID = *(DWORD*)(payload + 4);
    BYTE bSackCnt = *(BYTE*)(payload + 8);

    std::vector<BYTE> ackBuf; ackBuf.reserve(1024);
    ackBuf.push_back(0); // bResult
    ackBuf.push_back(reqObjectID & 0xFF); ackBuf.push_back((reqObjectID>>8)&0xFF); ackBuf.push_back((reqObjectID>>16)&0xFF); ackBuf.push_back((reqObjectID>>24)&0xFF); // dwObjectID
    ackBuf.push_back(bSackCnt); // bSackID
    
    std::vector<sFunctionalNpcItem> items;
    if (g_MapInstances.count(reqMapID)) {
        CMapInstance* mapInst = g_MapInstances[reqMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetMonster(reqObjectID);
        if (pObj) {
            for (auto& it : pObj->npcItems) {
                if (it.bSackCnt == bSackCnt) {
                    if (g_ItemTemplates.count(it.dwItemID)) {
                        items.push_back(it);
                    }
                }
            }
        }
    }
    
    WORD numItems = items.size();
    ackBuf.push_back(numItems & 0xFF); ackBuf.push_back(numItems >> 8);
    
    auto pushDWord = [&](DWORD d) { ackBuf.push_back(d & 0xFF); ackBuf.push_back((d>>8)&0xFF); ackBuf.push_back((d>>16)&0xFF); ackBuf.push_back((d>>24)&0xFF); };
    auto pushWord = [&](WORD w) { ackBuf.push_back(w & 0xFF); ackBuf.push_back((w>>8)&0xFF); };
    auto pushByte = [&](BYTE b) { ackBuf.push_back(b); };
    
    for (auto& item : items) {
        if (g_ItemTemplates.count(item.dwItemID)) {
            sItemTemplate& tpl = g_ItemTemplates[item.dwItemID];
            pushWord(tpl.wRefID);
            pushByte(tpl.bType);
            pushByte(tpl.bKind);
            pushWord(tpl.wVisualID);
            WORD nlen = tpl.szName.length(); pushWord(nlen);
            for (char c : tpl.szName) pushByte(c);
            pushDWord(item.dwPrice > 0 ? item.dwPrice : tpl.dwCost);
            pushWord(tpl.wLevel);
            pushByte(tpl.bCharType);
            pushWord(item.wAmount);
            pushByte(item.bPos);
            pushByte(item.bRarity);
            
            switch (tpl.bType) {
                case 1: // WEAPON
                case 2: // CLOTH
                case 3: // HAT
                case 4: // SHOE
                case 5: // CLOAK
                case 6: // RING
                case 7: // NECKLACE
                case 13: // SOCKET
                case 20: // BONGIN
                    pushWord(0); // wNeedLevel
                    pushWord(0); // wNeedDex
                    pushWord(0); // wNeedStr
                    pushWord(0); // wNeedSus
                    pushWord(0); // wNeedVit
                    pushWord(0); // wDecrDurRate
                    pushWord(tpl.nBasicData1); // wCurDur
                    pushWord(tpl.nBasicData1); // wMaxDur
                    pushWord(tpl.nData4); // wAtkPwr
                    pushWord(tpl.nData5); // wDefPwr
                    pushWord(0); // wAtkRating
                    pushWord(0); // wStkSpeed
                    pushWord(0); // wAtkRange
                    pushWord(0); // wIncrHp
                    pushWord(0); // wIncrIp
                    pushWord(0); // wRestoreHp
                    pushWord(0); // wRestoreIp
                    pushWord(0); // wIncrCritical
                    pushWord(0); // wHukjungModityCount
                    pushWord(0); // wSojungModityCount
                    pushWord(0); // wHuljungModityCount
                    pushWord(0); // wModifyCount
                    
                    if (tpl.bType == 20) {
                        pushWord(0); // wSoakHPRatio
                        pushWord(0); // wSoakAtkRatio
                        pushWord(0); // wSoakDefRatio
                        pushWord(0); // wSoakHitRatio
                    }
                    break;
                case 15: // ITEMTYPE_NPCBAG
                    pushByte(0); // bDecrDurRate
                    pushWord(tpl.nBasicData1); // wCurDur
                    pushWord(tpl.nBasicData1); // wMaxDur
                    break;
                case 16: // ITEMTYPE_NPCITEM
                    pushWord(tpl.nData1); // wTamingLevel
                    pushWord(tpl.nData2); // wNpcItemType
                    pushWord(tpl.nData3); // wTamingRate
                    pushWord(tpl.nData4); // wWildRate
                    pushWord(tpl.nData5); // wIncrHp
                    break;
                case 23: // ITEMTYPE_POTION
                    pushWord(tpl.nData1); // wIncrHp
                    pushWord(tpl.nData2); // wIncrIp
                    break;
                case 27: // ITEMTYPE_LOTTO
                    pushByte(0); // bPrizeRank
                    pushDWord(0); // dwRound
                    pushByte(0); // bLottoNum[0]
                    pushByte(0); // bLottoNum[1]
                    pushByte(0); // bLottoNum[2]
                    break;
                case 32: // ITEMTYPE_GISDURABLITY
                    pushWord(tpl.nData1); // wFunctionItem
                    pushWord(tpl.nBasicData1); // wCurDur
                    pushWord(tpl.nBasicData1); // wMaxDur
                    pushDWord(tpl.nData3); // dwValue
                    break;
            }
        }
    }
    
    std::vector<BYTE> fullAck; fullAck.resize(4);
    fullAck.insert(fullAck.end(), ackBuf.begin(), ackBuf.end());
    PACKET_HEADER* ah = (PACKET_HEADER*)fullAck.data(); ah->id = 0x3534; ah->payloadSize = (WORD)ackBuf.size();
    EncryptPacket(fullAck.data(), 0x42); 
    SafeSend(clientSocket, (const char*)fullAck.data(), fullAck.size(), 0);
    std::cout << "[NpcHandler] Sent NPC Item List for NPC " << reqObjectID << " Items: " << numItems << std::endl;
}

void OnBuyItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 16) return;
    DWORD dwShopID = *(DWORD*)(payload);
    DWORD dwItemID = *(DWORD*)(payload + 4); 
    DWORD dwAmount = *(DWORD*)(payload + 8);
    BYTE bShopSackCnt = *(BYTE*)(payload + 12);
    BYTE bShopSackPos = *(BYTE*)(payload + 13);
    BYTE bCharSackCnt = *(BYTE*)(payload + 14);
    BYTE bCharSackPos = *(BYTE*)(payload + 15);

    if (g_ItemTemplates.find(dwItemID) == g_ItemTemplates.end()) return;
    sItemTemplate& tpl = g_ItemTemplates[dwItemID];

    DWORD price = tpl.dwCost;
    DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (g_MapInstances.count(pMapID)) {
        CMapInstance* mapInst = g_MapInstances[pMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetMonster(dwShopID);
        if (pObj) {
            for (auto& it : pObj->npcItems) {
                if (it.dwItemID == dwItemID) {
                    if (it.dwPrice > 0) price = it.dwPrice;
                    break;
                }
            }
        }
    }

    DWORD totalCost = price * dwAmount;

    INT64 currentMoney = -1;
    std::string q = "SELECT dwMoney FROM CHAR_DATA WHERE dwCharID = " + std::to_string(charID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN cbMoney = 0;
        SQLGetData(hStmt, 1, SQL_C_SBIGINT, &currentMoney, 0, &cbMoney);
    });

    if (currentMoney == -1) return;

    if (currentMoney >= totalCost || totalCost == 0) {
        std::string qUpd = "UPDATE CHAR_DATA SET dwMoney = dwMoney - " + std::to_string(totalCost) + " WHERE dwCharID = " + std::to_string(charID);
        DBHelper::GetInstance().ExecuteUpdate(qUpd);

        std::vector<BYTE> mBuf(13);
        PACKET_HEADER* mHead = (PACKET_HEADER*)mBuf.data();
        mHead->id = 0x3B13;
        mHead->payloadSize = 9;
        *((INT64*)(mBuf.data() + 4)) = (INT64)(currentMoney - totalCost);
        mBuf[12] = 0;
        EncryptPacket(mBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)mBuf.data(), mBuf.size(), 0);

        DWORD newDbItemID = 0;
        std::string insItem = "SET NOCOUNT ON; INSERT INTO ITEM (wRefID, bType, bKind, wVisualID, szName, dwCost, wLevel, bCharType, wAmount) VALUES (" + std::to_string(dwItemID) + ", " + std::to_string(tpl.bType) + ", " + std::to_string(tpl.bKind) + ", " + std::to_string(tpl.wVisualID) + ", '" + tpl.szName + "', " + std::to_string(tpl.dwCost) + ", " + std::to_string(tpl.wLevel) + ", " + std::to_string(tpl.bCharType) + ", " + std::to_string(dwAmount) + "); SELECT @@IDENTITY;";
        DBHelper::GetInstance().ExecuteQuery(insItem, [&](SQLHSTMT hStmt2) {
            SQLLEN c;
            SQLGetData(hStmt2, 1, SQL_C_ULONG, &newDbItemID, 0, &c);
        });
        if (newDbItemID == 0) newDbItemID = rand() * rand();

        if (bCharSackPos == 255) {
            BYTE result = FindFreeSackPos(charID, bCharSackCnt, tpl.bCX, tpl.bCY);
            if (result == 255) {
                // Inventory full!
                return;
            }
            int startPos = (bCharSackCnt == 1) ? 20 : (bCharSackCnt == 2) ? 60 : 100;
            bCharSackPos = result - startPos;
        }

        BYTE absolutePos = bCharSackPos;
        if (bCharSackCnt == 1) absolutePos = 20 + bCharSackPos;
        else if (bCharSackCnt == 2) absolutePos = 20 + 36 + bCharSackPos;
        
        std::string insSack = "INSERT INTO SACKITEM (dwCharID, bSackPos, dwItemID) VALUES (" + std::to_string(charID) + ", " + std::to_string(absolutePos) + ", " + std::to_string(newDbItemID) + ")";
        DBHelper::GetInstance().ExecuteUpdate(insSack);

        std::vector<BYTE> ackBuf;
        ackBuf.resize(4);
        auto pushByte = [&](BYTE v) { ackBuf.push_back(v); };
        auto pushWord = [&](WORD v) { ackBuf.push_back(v & 0xFF); ackBuf.push_back(v >> 8); };
        auto pushDWord = [&](DWORD v) { ackBuf.push_back(v & 0xFF); ackBuf.push_back((v >> 8) & 0xFF); ackBuf.push_back((v >> 16) & 0xFF); ackBuf.push_back(v >> 24); };
        auto pushString = [&](const std::string& str) {
            std::string trimmed = str;
            size_t endpos = trimmed.find_last_not_of(" \n\r\t");
            if (endpos != std::string::npos) trimmed = trimmed.substr(0, endpos + 1);
            else trimmed = "";
            WORD len = trimmed.size();
            pushWord(len);
            for (char c : trimmed) pushByte(c);
        };

        pushByte(bCharSackCnt);
        pushByte(bCharSackPos);
        pushDWord(newDbItemID);
        pushWord(tpl.wRefID);
        pushByte(tpl.bType);
        pushByte(tpl.bKind);
        pushWord(tpl.wVisualID);
        pushString(tpl.szName);
        pushDWord(tpl.dwCost);
        pushWord(tpl.wLevel);
        pushByte(tpl.bCharType);
        pushWord(dwAmount);

        if (tpl.bType < 10 || tpl.bType == 13 || tpl.bType == 20) {
            for (int i = 0; i < 85; i++) pushByte(0);
        } else if (tpl.bType == 15) { // ITEMTYPE_NPCBAG
            pushByte(0); // m_bDecrDurRate
            pushWord(tpl.nBasicData1); // m_wCurDur
            pushWord(tpl.nBasicData1); // m_wMaxDur
            pushByte(0); // m_bNpcRace
            pushByte(0); // m_bNpcBagSize
        } else if (tpl.bType == 16) { // ITEMTYPE_NPCITEM
            pushWord(tpl.nData1); // m_wTamingLevel
            pushByte(tpl.nData2); // m_bNpcItemType
            pushWord(tpl.nData3); // m_wTamingRate
            pushByte(tpl.nData4); // m_bWildRate
            pushWord(tpl.nData5); // m_wIncrHp
        } else if (tpl.bType == 23) { // ITEMTYPE_POTION
            pushDWord(0); // m_dwKeepUpTime
            pushWord(tpl.nData1); // m_wIncrHp
            pushWord(tpl.nData2); // m_wIncrIp
            pushByte(0); // m_bMinLevel
            pushByte(0); // m_bMaxLevel
        } else if (tpl.bType == 27) { // ITEMTYPE_LOTTO
            pushByte(0); // m_bPrizeRank
            pushDWord(0); // m_dwRound
            pushByte(0); pushByte(0); pushByte(0); pushByte(0); // m_bLottoNum[4]
            pushDWord(0); // m_dwPrizeMoney
        } else if (tpl.bType == 32) { // ITEMTYPE_GISDURABLITY
            pushWord(tpl.nData1); // m_wFunctionItem
            pushWord(tpl.nBasicData1); // m_wCurDur
            pushWord(tpl.nBasicData1); // m_wMaxDur
            pushDWord(tpl.nData3); // m_dwValue
        } else if (tpl.bType == 33) { // ITEMTYPE_REBIRTH
            pushByte(0); // m_bStepID
            pushWord(tpl.nData1); // m_wRebirthFuncID
            pushWord(0); // m_wRebirthNeedLevel
        } else if (tpl.bType == 21) { // ITEMTYPE_BOOK
            DWORD mid = tpl.nBasicData2;
            sMugongTemplate* mg = MugongManager::GetInstance()->GetTemplate(mid);
            BYTE type = mg ? mg->bType : 0;
            BYTE kind = mg ? mg->bKind : 0;
            pushWord(tpl.wLevel); // m_wNeedLevel
            pushDWord(mid); // m_dwMugongID
            pushByte(type); // m_bMugongType
            pushByte(kind); // m_bMugongKind
            pushByte(1); // m_bNeedMugongLevel
        }

        pushWord(0); // HT_1116 m_wRebuithValue
        
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x420A;
        head->payloadSize = ackBuf.size() - 4;
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);

        std::vector<BYTE> bAckBuf(5);
        PACKET_HEADER* bHead = (PACKET_HEADER*)bAckBuf.data();
        bHead->id = 0x3D31;
        bHead->payloadSize = 1;
        bAckBuf[4] = 0;
        EncryptPacket(bAckBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)bAckBuf.data(), bAckBuf.size(), 0);
    } else {
        std::vector<BYTE> errBuf(5);
        PACKET_HEADER* eHead = (PACKET_HEADER*)errBuf.data();
        eHead->id = 0x3D31;
        eHead->payloadSize = 1;
        errBuf[4] = 1;
        EncryptPacket(errBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)errBuf.data(), errBuf.size(), 0);
    }
}

void OnSellItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 10) return;
    DWORD dwShopID = *(DWORD*)(payload);
    DWORD dwItemID = *(DWORD*)(payload + 4);
    BYTE bSackID = *(BYTE*)(payload + 8);
    BYTE bSackPos = *(BYTE*)(payload + 9);

    DWORD amountToSell = 1;
    if (totalSize >= 14) {
        amountToSell = *(DWORD*)(payload + 10);
    }

    WORD wRefID = 0;
    DWORD dbAmount = 0;
    DWORD dbCost = 0;
    std::string qItem = "SELECT wRefID, wAmount, dwCost FROM ITEM WHERE dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteQuery(qItem, [&](SQLHSTMT hStmt) {
        SQLLEN c1, c2, c3;
        SQLGetData(hStmt, 1, SQL_C_USHORT, &wRefID, 0, &c1);
        SQLGetData(hStmt, 2, SQL_C_ULONG, &dbAmount, 0, &c2);
        SQLGetData(hStmt, 3, SQL_C_ULONG, &dbCost, 0, &c3);
    });

    if (wRefID == 0) return;
    if (amountToSell > dbAmount) amountToSell = dbAmount;
    
    DWORD sellPrice = 0;
    if (g_ItemTemplates.count(wRefID)) {
        sellPrice = g_ItemTemplates[wRefID].dwCost * amountToSell;
    }

    std::string qUpd = "UPDATE CHAR_DATA SET dwMoney = dwMoney + " + std::to_string(sellPrice) + " WHERE dwCharID = " + std::to_string(charID);
    DBHelper::GetInstance().ExecuteUpdate(qUpd);

    INT64 currentMoney = 0;
    std::string qMoney = "SELECT dwMoney FROM CHAR_DATA WHERE dwCharID = " + std::to_string(charID);
    DBHelper::GetInstance().ExecuteQuery(qMoney, [&](SQLHSTMT hStmt2) {
        SQLLEN cbMoney = 0;
        SQLGetData(hStmt2, 1, SQL_C_SBIGINT, &currentMoney, 0, &cbMoney);
    });

    std::vector<BYTE> mBuf(13);
    PACKET_HEADER* mHead = (PACKET_HEADER*)mBuf.data();
    mHead->id = 0x3B13;
    mHead->payloadSize = 9;
    *((INT64*)(mBuf.data() + 4)) = (INT64)currentMoney;
    mBuf[12] = 0;
    EncryptPacket(mBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)mBuf.data(), mBuf.size(), 0);

    if (dbAmount <= amountToSell) {
        std::string delSack = "DELETE FROM SACKITEM WHERE dwItemID = " + std::to_string(dwItemID);
        std::string delItem = "DELETE FROM ITEM WHERE dwItemID = " + std::to_string(dwItemID);
        DBHelper::GetInstance().ExecuteUpdate(delSack);
        DBHelper::GetInstance().ExecuteUpdate(delItem);

        std::vector<BYTE> rBuf(7);
        PACKET_HEADER* rHead = (PACKET_HEADER*)rBuf.data();
        rHead->id = 0x4208;
        rHead->payloadSize = 3;
        rBuf[4] = bSackID;
        rBuf[5] = bSackPos;
        rBuf[6] = 0;
        EncryptPacket(rBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)rBuf.data(), rBuf.size(), 0);
    } else {
        std::string updItem = "UPDATE ITEM SET wAmount = wAmount - " + std::to_string(amountToSell) + " WHERE dwItemID = " + std::to_string(dwItemID);
        DBHelper::GetInstance().ExecuteUpdate(updItem);
        // Do not send 0x4208, client handles amount deduction internally or needs an amount update packet
    }

    std::vector<BYTE> sBuf(5);
    PACKET_HEADER* sHead = (PACKET_HEADER*)sBuf.data();
    sHead->id = 0x3D59;
    sHead->payloadSize = 1;
    sBuf[4] = 0;
    EncryptPacket(sBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)sBuf.data(), sBuf.size(), 0);
}

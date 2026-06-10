#include "NpcHandler.h"
#include "../DB/CharacterDB.h"
#include "../DB/ItemDB.h"
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
        MonsterData* pObj = mapInst->GetMonster(reqId);
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
  try {
    if (totalSize < 9) return;
    DWORD reqMapID = *(DWORD*)(payload);
    DWORD reqObjectID = *(DWORD*)(payload + 4);
    BYTE bSackCnt = *(BYTE*)(payload + 8);
    LOG("[NpcHandler] ItemListReq: objID=" + std::to_string(reqObjectID) + " mapID=" + std::to_string(reqMapID) + " sackCnt=" + std::to_string(bSackCnt));

    std::vector<BYTE> ackBuf; ackBuf.reserve(1024);
    ackBuf.push_back(0); // bResult
    ackBuf.push_back(reqObjectID & 0xFF); ackBuf.push_back((reqObjectID>>8)&0xFF); ackBuf.push_back((reqObjectID>>16)&0xFF); ackBuf.push_back((reqObjectID>>24)&0xFF); // dwObjectID
    ackBuf.push_back(bSackCnt); // bSackID
    
    std::vector<sFunctionalNpcItem> items;
    if (g_MapInstances.count(reqMapID)) {
        CMapInstance* mapInst = g_MapInstances[reqMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        MonsterData* pObj = mapInst->GetMonster(reqObjectID);
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
                case 8: // SOCKET
                case 9: // BONGIN (shield)
                    pushWord((WORD)tpl.nBasicData1); // wNeedLevel
                    pushWord((WORD)tpl.nBasicData2); // wNeedDex
                    pushWord((WORD)tpl.nBasicData3); // wNeedStr
                    pushWord((WORD)tpl.nBasicData4); // wNeedSus
                    pushWord((WORD)tpl.nBasicData5); // wNeedVit
                    pushWord((WORD)tpl.nData1); // wDecrDurRate
                    pushWord((WORD)tpl.nData2); // wCurDur (from template nData2)
                    pushWord((WORD)tpl.nData3); // wMaxDur (from template nData3)
                    pushDWord(tpl.nData4); // wAtkPwr (DWORD)
                    pushDWord(tpl.nData5); // wDefPwr (DWORD)
                    pushDWord(tpl.nData6); // wAtkRating (DWORD)
                    pushWord((WORD)tpl.nData7); // wStkSpeed
                    pushWord((WORD)tpl.nData8); // wAtkRange
                    pushDWord(tpl.nData9); // wIncrHp (DWORD)
                    pushDWord(tpl.nData10); // wIncrIp (DWORD)
                    pushDWord(0); // dwRestoreHp (DWORD: 支持超过65535)
                    pushDWord(0); // dwRestoreIp (DWORD: 支持超过65535)
                    pushWord((WORD)tpl.nData13); // wIncrCritical
                    pushWord(0); // wHukjungModityCount (rebuild rarity)
                    pushWord(0); // wSojungModityCount (stx type)
                    pushWord(0); // wHuljungModityCount
                    pushWord(0); // wModifyCount
                    
                    if (tpl.bType == 9) { // BONGIN has extra soak fields
                        pushWord(0); // wSoakHPRatio
                        pushWord(0); // wSoakAtkRatio
                        pushWord(0); // wSoakDefRatio
                        pushWord(0); // wSoakHitRatio
                    }
                    break;
                case 11: // NPCRING
                case 12: // NPCNECKLACE
                case 13: // NPCWEAPON
                case 14: // NPCRIDING
                case 17: // SADDLE
                    pushByte(0); // bTemp
                    pushWord((WORD)tpl.nData2); // wCurDur
                    pushWord((WORD)tpl.nData3); // wMaxDur
                    break;
                case 15: // ITEMTYPE_NPCBAG
                    pushByte(0); // bDecrDurRate
                    pushWord((WORD)tpl.nBasicData1); // wCurDur
                    pushWord((WORD)tpl.nBasicData1); // wMaxDur
                    break;
                case 16: // ITEMTYPE_NPCITEM
                    pushWord((WORD)tpl.nData1); // wTamingLevel
                    pushWord((WORD)tpl.nData2); // wNpcItemType
                    pushWord((WORD)tpl.nData3); // wTamingRate
                    pushWord((WORD)tpl.nData4); // wWildRate
                    pushDWord((DWORD)tpl.nData5); // wIncrHp
                    break;
                case 18: // SUNANG (event item)
                    pushByte(0); // bModifyCnt
                    break;
                case 20: // SURESOURCE — client reads bFuncID(1) + dwValue(4)
                    pushByte(0); // bFuncID
                    pushDWord(0); // dwValue
                    break;
                case 23: // ITEMTYPE_POTION
                    pushDWord((DWORD)tpl.nBasicData2); // wIncrHp (HP recovery)
                    pushDWord((DWORD)tpl.nBasicData3); // wIncrIp (IP recovery)
                    break;
                case 27: // ITEMTYPE_LOTTO
                    pushByte(0); // bPrizeRank
                    pushDWord(0); // dwRound
                    pushByte(0); // bLottoNum[0]
                    pushByte(0); // bLottoNum[1]
                    pushByte(0); // bLottoNum[2]
                    pushByte(0); // bLottoNum[3]
                    pushDWord(0); // dwPrizeMoney
                    break;
                case 32: // ITEMTYPE_GISDURABLITY
                    pushWord((WORD)tpl.nData1); // wFunctionItem
                    pushWord((WORD)tpl.nBasicData1); // wCurDur
                    pushWord((WORD)tpl.nBasicData1); // wMaxDur
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
  } catch (std::exception& e) {
    LOG("[NpcHandler] EXCEPTION in ItemListReq: " + std::string(e.what()));
  } catch (...) {
    LOG("[NpcHandler] EXCEPTION in ItemListReq: unknown");
  }
}

void OnBuyItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
  try {
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
    WORD npcItemAmount = tpl.wAmount;  // Default to template amount
    DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
MonsterData* pObj = NULL;
    bool foundObj = false;

    if (dwShopID == 100038 || dwShopID == 38) {
        // Global search for NPC 38 across all maps
        for (auto& pair : g_MapInstances) {
            CMapInstance* mapInst = pair.second;
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            pObj = mapInst->GetMonster(100038);
            if (pObj) {
                foundObj = true;
                bool sellsItem = false;
                for (auto& it : pObj->npcItems) {
                    if (it.dwItemID == dwItemID && it.bPos == bShopSackPos) {
                        if (it.dwPrice > 0) price = it.dwPrice;
                        if (it.wAmount > 0) npcItemAmount = it.wAmount;
                        sellsItem = true;
                        break;
                    }
                }
                if (!sellsItem) {
                    for (auto& it : pObj->npcItems) {
                        if (it.dwItemID == dwItemID) {
                            if (it.dwPrice > 0) price = it.dwPrice;
                            if (it.wAmount > 0) npcItemAmount = it.wAmount;
                            sellsItem = true;
                            break;
                        }
                    }
                }
                if (!sellsItem) {
                    // Item not sold by NPC 38, reject purchase
                    return;
                }
                break;
            }
        }
        if (!foundObj) {
            // NPC 38 not found at all, reject purchase
            return;
        }
    } else {
        if (g_MapInstances.count(pMapID)) {
            CMapInstance* mapInst = g_MapInstances[pMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            pObj = mapInst->GetMonster(dwShopID);
            if (pObj) {
                for (auto& it : pObj->npcItems) {
                    if (it.dwItemID == dwItemID && it.bPos == bShopSackPos) {
                        if (it.dwPrice > 0) price = it.dwPrice;
                        if (it.wAmount > 0) npcItemAmount = it.wAmount;
                        break;
                    }
                }
                if (npcItemAmount == tpl.wAmount) {
                    for (auto& it : pObj->npcItems) {
                        if (it.dwItemID == dwItemID && it.bSackCnt == bShopSackCnt) {
                            if (it.dwPrice > 0) price = it.dwPrice;
                            if (it.wAmount > 0) npcItemAmount = it.wAmount;
                            break;
                        }
                    }
                }
            }
        }
    }
    
    // Use NPC item's wAmount as the actual stack size, multiplied by client's buy count
    DWORD actualAmount = (DWORD)npcItemAmount * dwAmount;
    DWORD totalCost = price * actualAmount;
    LOG("[NpcHandler] BuyItem: refID=" + std::to_string(dwItemID) + " npcAmount=" + std::to_string(npcItemAmount) + " clientAmount=" + std::to_string(dwAmount) + " actualAmount=" + std::to_string(actualAmount) + " price=" + std::to_string(price) + " totalCost=" + std::to_string(totalCost));

    INT64 currentMoney = (INT64)CharacterDB::GetInstance().GetMoney(charID);

    if (currentMoney == -1) return;

    if (currentMoney >= totalCost || totalCost == 0) {
        CharacterDB::GetInstance().SubtractMoney(charID, totalCost);

        std::vector<BYTE> mBuf(13);
        PACKET_HEADER* mHead = (PACKET_HEADER*)mBuf.data();
        mHead->id = 0x3B13;
        mHead->payloadSize = 9;
        *((INT64*)(mBuf.data() + 4)) = (INT64)(currentMoney - totalCost);
        mBuf[12] = 0;
        EncryptPacket(mBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)mBuf.data(), mBuf.size(), 0);

        DWORD newDbItemID = ItemDB::GetInstance().InsertItem(dwItemID, tpl.bType, tpl.bKind, tpl.wVisualID, tpl.szName, tpl.dwCost, tpl.wLevel, tpl.bCharType, actualAmount);
        if (newDbItemID == 0) newDbItemID = rand() * rand();

        // Insert ITEMDATA for equipment types so stats persist in DB
        if (newDbItemID > 0 && tpl.bType >= 1 && tpl.bType <= 9) {
            int nData[25] = {0};
            nData[0] = tpl.nData1;  // nData1 = DecrDurRate
            nData[1] = tpl.nData2;  // nData2 = CurDur
            nData[2] = tpl.nData3;  // nData3 = MaxDur
            nData[3] = tpl.nData4;  // nData4 = AtkPwr
            nData[4] = tpl.nData5;  // nData5 = DefPwr
            nData[5] = tpl.nData6;  // nData6 = AtkRating
            nData[6] = tpl.nData7;  // nData7 = StkSpeed
            nData[7] = tpl.nData8;  // nData8 = AtkRange
            nData[8] = tpl.nData9;  // nData9 = IncrHp
            nData[9] = tpl.nData10; // nData10 = IncrIp
            nData[12] = tpl.nData13; // nData13 = IncrCritical
            ItemDB::GetInstance().InsertItemData(newDbItemID, nData);
        }

        // 无论客户端传什么位置，都由服务端自动分配空位，避免唯一约束冲突导致物品覆盖
        {
            BYTE result = 255;
            for (BYTE tryPage = 1; tryPage <= 3 && result == 255; tryPage++) {
                result = FindFreeSackPos(charID, tryPage, tpl.bCX, tpl.bCY);
                if (result != 255) {
                    bCharSackCnt = tryPage;
                    break;
                }
            }
            if (result == 255) {
                // 所有背包页都满了，回滚已创建的物品和扣款
                ItemDB::GetInstance().DeleteItem(newDbItemID);
                CharacterDB::GetInstance().AddMoney(charID, totalCost);
                LOG("[NpcHandler] BuyItem FAILED: inventory full, rolled back itemID=" + std::to_string(newDbItemID));
                return;
            }
            int startPos = (bCharSackCnt == 1) ? 20 : (bCharSackCnt == 2) ? 60 : 100;
            bCharSackPos = result - startPos;
        }

        BYTE absolutePos = bCharSackPos;
        if (bCharSackCnt == 1) absolutePos = 20 + bCharSackPos;
        else if (bCharSackCnt == 2) absolutePos = 60 + bCharSackPos;
        else if (bCharSackCnt == 3) absolutePos = 100 + bCharSackPos;
        
        LOG("[NpcHandler] BuyItem SLOT: page=" + std::to_string(bCharSackCnt) + " relPos=" + std::to_string(bCharSackPos) + " absPos=" + std::to_string(absolutePos) + " itemID=" + std::to_string(newDbItemID));
        ItemDB::GetInstance().AddToSack(charID, absolutePos, newDbItemID);

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
        pushWord((WORD)actualAmount);

        if (tpl.bType >= 1 && tpl.bType <= 9) { // Equipment types (WEAPON thru BONGIN)
            pushWord((WORD)tpl.nBasicData1); // wNeedLevel
            pushWord((WORD)tpl.nBasicData2); // wNeedDex
            pushWord((WORD)tpl.nBasicData3); // wNeedStr
            pushWord((WORD)tpl.nBasicData4); // wNeedSus
            pushWord((WORD)tpl.nBasicData5); // wNeedVit
            pushByte((BYTE)tpl.nData1); // bDecrDurRate
            pushWord((WORD)tpl.nData2); // wCurDur
            pushWord((WORD)tpl.nData3); // wMaxDur
            pushDWord(tpl.nData4); // wAtkPwr (DWORD)
            pushDWord(tpl.nData5); // wDefPwr (DWORD)
            pushDWord(tpl.nData6); // wAtkRating (DWORD)
            pushWord((WORD)tpl.nData7); // wStkSpeed
            pushWord((WORD)tpl.nData8); // wAtkRange
            pushDWord(tpl.nData9); // wIncrHp (DWORD)
            pushDWord(tpl.nData10); // wIncrIp (DWORD)
            pushDWord(0); // dwRestoreHp (DWORD: 支持超过65535)
            pushDWord(0); // dwRestoreIp (DWORD: 支持超过65535)
            pushWord((WORD)tpl.nData13); // wIncrCritical
            pushByte(0); // bRarity (HukjungModity)
            pushByte(0); // bStxType (SojungModity)
            pushByte(0); // bLimitCnt (HuljungModity)
            pushByte(0); // bModifyCnt
            pushByte(0); // bRepairCnt
            pushByte(0); // bRepairDiscount
            if (tpl.bType == 9) { // BONGIN
                pushDWord(0); // dwNpcID
                pushWord(0); // wSoakHPRatio
                pushWord(0); // wSoakAtkRatio
                pushWord(0); // wSoakDefRatio
                pushWord(0); // wSoakHitRatio
            } else if (tpl.bType == 8) { // SOCKET
                pushByte(0); // bDanIncExp
                pushByte(0); // bMopDecAtk
                pushByte(0); // bMopDecDef
                pushByte(0); // bMopDecAtkRatio
                pushByte(0); // bMopDecHP
                pushByte(0); // bShopDecTax
                pushByte(0); // bGambleShopDec
                pushByte(0); // bEffectType
            } else { // types 1-7
                pushByte(0); // bPuzzleType
            }
            // Socket data for types 1-4 (WEAPON, CLOTH, HAT, SHOE)
            if (tpl.bType >= 1 && tpl.bType <= 4) {
                pushByte(0); // bSocketItem[0]
                pushByte(0); // bSocketItem[1]
                pushByte(0); // bSocketItem[2]
                pushWord(0); // wRBSocketItem
            }
        } else if (tpl.bType == 11 || tpl.bType == 12 || tpl.bType == 13 || tpl.bType == 14 || tpl.bType == 17) {
            // NPCRING, NPCNECKLACE, NPCWEAPON, NPCRIDING, SADDLE
            pushByte(0); // bTemp
            pushWord((WORD)tpl.nData2); // wCurDur
            pushWord((WORD)tpl.nData3); // wMaxDur
        } else if (tpl.bType == 15) { // ITEMTYPE_NPCBAG
            pushByte(0); // m_bDecrDurRate
            pushWord((WORD)tpl.nBasicData1); // m_wCurDur
            pushWord((WORD)tpl.nBasicData1); // m_wMaxDur
            pushByte(0); // m_bNpcRace
            pushByte(0); // m_bNpcBagSize
        } else if (tpl.bType == 16) { // ITEMTYPE_NPCITEM
            pushWord((WORD)tpl.nData1); // m_wTamingLevel
            pushByte((BYTE)tpl.nData2); // m_bNpcItemType
            pushWord((WORD)tpl.nData3); // m_wTamingRate
            pushByte((BYTE)tpl.nData4); // m_bWildRate
            pushWord((WORD)tpl.nData5); // m_wIncrHp
        } else if (tpl.bType == 18) { // ITEMTYPE_SUNANG
            pushByte(0); // m_bFuncID
            pushDWord(0); // m_dwValue
            pushWord(0); // m_wCurDur
            pushWord(0); // m_wMaxDur
            pushByte(0); // m_bModifyCnt
        } else if (tpl.bType == 19) { // ITEMTYPE_EVENT
            pushWord(0); // m_wKeyRefID
            pushWord(0); // m_wKeyAmount
        } else if (tpl.bType == 20) { // ITEMTYPE_SURESOURCE
            pushByte(0); // m_bFuncID
            pushDWord(0); // m_dwValue
        } else if (tpl.bType == 23) { // ITEMTYPE_POTION
            pushDWord(0); // m_dwKeepUpTime
            pushWord((WORD)tpl.nBasicData2); // m_wIncrHp (HP recovery)
            pushWord((WORD)tpl.nBasicData3); // m_wIncrIp (IP recovery)
            pushByte(0); // m_bMinLevel
            pushByte(0); // m_bMaxLevel
        } else if (tpl.bType == 25) { // ITEMTYPE_REBUILDRES（改造材料）
            pushByte(0); // m_bIsDividedRes（必须为0，bKind=1会被客户端拒绝放入改造槽）
            pushWord((WORD)tpl.nBasicData2); // m_wSuccessRatio
            pushWord((WORD)tpl.nBasicData3); // m_wFactorValue
        } else if (tpl.bType == 27) { // ITEMTYPE_LOTTO
            pushByte(0); // m_bPrizeRank
            pushDWord(0); // m_dwRound
            pushByte(0); pushByte(0); pushByte(0); pushByte(0); // m_bLottoNum[4]
            pushDWord(0); // m_dwPrizeMoney
        } else if (tpl.bType == 29) { // ITEMTYPE_MANUAL
            pushWord(0); // m_wOriginRefID
            pushWord(0); // m_wUnionRefID
        } else if (tpl.bType == 31) { // ITEMTYPE_REBIRTH
            pushByte(0); // m_bStepID
            pushWord((WORD)tpl.nData1); // m_wRebirthFuncID
            pushWord(0); // m_wRebirthNeedLevel
        } else if (tpl.bType == 32) { // ITEMTYPE_GISDURABLITY
            pushWord((WORD)tpl.nData1); // m_wFunctionItem
            pushWord((WORD)tpl.nBasicData1); // m_wCurDur
            pushWord((WORD)tpl.nBasicData1); // m_wMaxDur
            pushDWord(tpl.nData3); // m_dwValue
        } else if (tpl.bType == 33) { // ITEMTYPE_GISTIMELIMIT (was REBIRTH)
            // No additional data (PREMIUMQUEST case is 34)
        } else if (tpl.bType == 34) { // ITEMTYPE_PREMIUMQUEST
            pushDWord(0); // m_dwPremiumQuestID
            pushByte(0); // m_bLimitCnt
        } else if (tpl.bType == 22) { // ITEMTYPE_PORTAL
            pushDWord((DWORD)tpl.nBasicData2); // m_dwPotalMapID (target map)
            pushByte(0); // m_bPortalType
            pushWord(0); // m_wPosX
            pushWord(0); // m_wPosY
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

  } catch (std::exception& e) {
    LOG("[NpcHandler] EXCEPTION in BuyItemReq: " + std::string(e.what()));
  } catch (...) {
    LOG("[NpcHandler] EXCEPTION in BuyItemReq: unknown");
  }
}

void OnSellItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 10) return;
    DWORD dwShopID = *(DWORD*)(payload);
    DWORD dwItemID = *(DWORD*)(payload + 4);
    BYTE bSackID = *(BYTE*)(payload + 8);
    BYTE bSackPos = *(BYTE*)(payload + 9);

    WORD wRefID = 0; WORD dbAmount = 0; DWORD dbCost = 0;
    ItemDB::ItemBasicInfo ibi;
    if (ItemDB::GetInstance().GetItemBasicInfo(dwItemID, ibi)) {
        wRefID = ibi.wRefID; dbAmount = ibi.wAmount; dbCost = ibi.dwCost;
    }
    if (wRefID == 0) return;

    DWORD amountToSell = dbAmount;
    if (totalSize >= 14) {
        amountToSell = *(DWORD*)(payload + 10);
    }
    if (amountToSell > dbAmount) amountToSell = dbAmount;
    if (amountToSell == 0) amountToSell = dbAmount;

    DWORD sellPrice = 0;
    if (g_ItemTemplates.count(wRefID)) {
        sellPrice = g_ItemTemplates[wRefID].dwCost * amountToSell;
    }

    CharacterDB::GetInstance().AddMoney(charID, sellPrice);
    INT64 currentMoney = (INT64)CharacterDB::GetInstance().GetMoney(charID);

    std::vector<BYTE> mBuf(13);
    PACKET_HEADER* mHead = (PACKET_HEADER*)mBuf.data();
    mHead->id = 0x3B13; mHead->payloadSize = 9;
    *((INT64*)(mBuf.data() + 4)) = (INT64)currentMoney;
    mBuf[12] = 0;
    EncryptPacket(mBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)mBuf.data(), mBuf.size(), 0);

    if (dbAmount <= amountToSell) {
        BYTE absolutePos = bSackPos;
        if (bSackID == 1) absolutePos = 20 + bSackPos;
        else if (bSackID == 2) absolutePos = 60 + bSackPos;
        else if (bSackID == 3) absolutePos = 100 + bSackPos;
        ItemDB::GetInstance().RemoveFromSack(charID, dwItemID);
        ItemDB::GetInstance().DeleteItemData(dwItemID);
        ItemDB::GetInstance().DeleteItem(dwItemID);

        std::vector<BYTE> rBuf(7);
        PACKET_HEADER* rHead = (PACKET_HEADER*)rBuf.data();
        rHead->id = 0x4208; rHead->payloadSize = 3;
        rBuf[4] = bSackID; rBuf[5] = bSackPos; rBuf[6] = 0;
        EncryptPacket(rBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)rBuf.data(), rBuf.size(), 0);
    } else {
        ItemDB::GetInstance().DecrementItemAmountBy(dwItemID, (WORD)amountToSell);
    }

    std::vector<BYTE> sBuf(5);
    PACKET_HEADER* sHead = (PACKET_HEADER*)sBuf.data();
    sHead->id = 0x3D59; sHead->payloadSize = 1; sBuf[4] = 0;
    EncryptPacket(sBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)sBuf.data(), sBuf.size(), 0);
}

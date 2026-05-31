#include <vector>
#include <string>
#include "RebuildItemHandler.h"
#include "../DB/ItemDB.h"
#include "../DB/CharacterDB.h"
#include "../Network/SessionMgr.h"
#include "../DBHelper.h"
#include "../Network/SystemMessage.h"
#include "../Network/PacketRouter.h"
#include "../ServerCore.h"

// 每件装备允许的最大改造总次数（成功+失败均计入）
static const int MAX_REBUILD_ATTEMPTS = 201;

// 装备强化白名单：仅允许特定 bType/bKind 组合进行强化
// bType=1(武器,排除bKind=8矿工锤)、2(衣服)、3(帽子)、4(鞋子)、6(戒指)、7(项链)
// bType=5(披风无数据)、8(宝石)、9(盾牌/法宝) 均不可强化
static bool IsRebuildableItem(BYTE bType, BYTE bKind) {
    switch (bType) {
        case 1: return bKind != 8; // 武器（排除矿工锤 bKind=8）
        case 2: // 衣服
        case 3: // 帽子
        case 4: // 鞋子
        case 6: // 戒指
        case 7: // 项链
            return true;
        default:
            return false;
    }
}

static void pushDWord(std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); }
static void pushWord(std::vector<BYTE>& buf, WORD w) { buf.push_back(w&0xFF); buf.push_back(w>>8); }
static void pushByte(std::vector<BYTE>& buf, BYTE b) { buf.push_back(b); }
static void pushString(std::vector<BYTE>& buf, const std::string& str) {
    pushWord(buf, str.length());
    for(char c : str) buf.push_back(c);
}

void SendItemRefresh(SOCKET clientSocket, DWORD dwItemID, BYTE bSackID, BYTE bSackPos) {
    ItemDB::FullItemRow row;
    if (!ItemDB::GetInstance().GetFullItemData(dwItemID, row)) return;
    {
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
            pushDWord(bi, d4); pushDWord(bi, d5); pushDWord(bi, d6); pushWord(bi, d7); pushWord(bi, d8);
            pushDWord(bi, d9); pushDWord(bi, d10); pushWord(bi, d11); pushWord(bi, d12); pushWord(bi, d13);
            pushByte(bi, dat18); pushByte(bi, dat19);
            pushByte(bi, d14); pushByte(bi, d15); pushByte(bi, d16); pushByte(bi, d17);
            if (type == 9) { pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); }
            else if (type == 8) { for(int i=0;i<8;i++) pushByte(bi, 0); }
            else { pushByte(bi, 0); }
            if (type >= 1 && type <= 4) { pushByte(bi, dat19); pushByte(bi, dat20); pushByte(bi, dat21); pushWord(bi, dat25); }
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
                case 22: pushDWord(bi, nd2); pushByte(bi, (BYTE)nd3); pushWord(bi, (WORD)nd4); pushWord(bi, (WORD)nd5); break;
                case 23: pushDWord(bi, 0); pushDWord(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
                case 25: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                case 27: pushByte(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
                case 29: pushWord(bi, 0); pushWord(bi, 0); break;
                case 32: pushWord(bi, 0); pushWord(bi, d2); pushWord(bi, d3); pushDWord(bi, 0); break;
                case 31: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                case 34: pushDWord(bi, 0); pushByte(bi, 0); break;
            }
        }
        
        // wRebuithValue - 觉醒值，数据源为 nData25（DB 高版本对齐）
        pushWord(bi, (WORD)dat25);
        
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
    }
}

void OnRebuildItemTermReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 6) return;
    DWORD dwItemID = *(DWORD*)(payload + 0);
    bool found = ItemDB::GetInstance().IsSackItemOwned(charID, dwItemID);

    BYTE result = 1; // 默认：物品未找到
    if (found) {
        // 查模板获取 bType/bKind，校验是否可强化
        WORD refID = ItemDB::GetInstance().GetItemRefID(dwItemID);
        if (refID > 0 && g_ItemTemplates.count(refID)) {
            BYTE bType = g_ItemTemplates[refID].bType;
            BYTE bKind = g_ItemTemplates[refID].bKind;
            if (IsRebuildableItem(bType, bKind)) {
                result = 0; // 校验通过
            } else {
                result = 3; // ERR_REBUILDITEMTREM_NOTREBUILDITEM：不可改造的物品
                LOG("[Rebuild] 拒绝强化：bType=" + std::to_string(bType) + " bKind=" + std::to_string(bKind) + " refID=" + std::to_string(refID));
            }
        } else {
            result = 2; // ERR_REBUILDITEMTREM_NOTBASICITEM：无模板数据
        }
    }

    std::vector<BYTE> ackBuf(5);
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4242; // CS_IM_REBUILDITEMTERM_ACK
    head->payloadSize = 1;
    ackBuf[4] = result;
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
    
    // Verify ownership and load item data
    if (!ItemDB::GetInstance().IsSackItemOwned(charID, dwItemID)) {
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4244; head->payloadSize = 1; ackBuf[4] = 2;
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
        return;
    }
    ItemDB::FullItemRow fir;
    if (ItemDB::GetInstance().GetFullItemData(dwItemID, fir)) {
        itemName = fir.szName;
        currentRebuild = fir.d[13]; // nData14
        currentAppend = fir.d[14];  // nData15
        currentAttempts = fir.d[16]; // nData17
        itemType = fir.bType;
        int refid = fir.wRefID;
        if (g_ItemTemplates.count(refid)) {
            reqLevel = g_ItemTemplates[refid].nBasicData1;
            baseItemName = g_ItemTemplates[refid].szName;
            baseData1 = g_ItemTemplates[refid].nData1;
            baseData2 = g_ItemTemplates[refid].nData2;
            baseData3 = g_ItemTemplates[refid].nData3;
            baseData4 = g_ItemTemplates[refid].nData4;
            baseData5 = g_ItemTemplates[refid].nData5;
        }
        // 白名单校验：拒绝不可强化的物品类型
        BYTE itemKind = fir.bKind;
        if (itemKind == 0 && g_ItemTemplates.count(fir.wRefID)) {
            itemKind = g_ItemTemplates[fir.wRefID].bKind;
        }
        if (!IsRebuildableItem((BYTE)itemType, itemKind)) {
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = 0x4244; head->payloadSize = 1; ackBuf[4] = 7; // ERR_REBUILDITEM_CANNOTTREBUILD
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
            LOG("[Rebuild] 拒绝强化执行：bType=" + std::to_string(itemType) + " bKind=" + std::to_string(itemKind));
            return;
        }
        itemValid = true;
    }
    
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
        WORD crystalRef = ItemDB::GetInstance().GetItemRefID(dwResourceID[0]);
        if (crystalRef > 0 && g_ItemTemplates.count(crystalRef)) {
            crystalKind = g_ItemTemplates[crystalRef].bKind;
            crystalSuccessRate = g_ItemTemplates[crystalRef].nBasicData2;
            hasCrystal = true;
        }
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
    {
        CharacterDB::CharPower cp;
        CharacterDB::GetInstance().GetCharData(charID, cp);
        if (cp.dwMoney >= rebuildCost) hasMoney = true;
    }
    
    if (!hasMoney) {
        std::vector<BYTE> ackBuf(5); PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4244; head->payloadSize = 1; ackBuf[4] = 1; // NOMONEY
        EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0); return;
    }
    
    CharacterDB::GetInstance().SubtractMoney(charID, rebuildCost);
    {
        INT64 currentMoney = CharacterDB::GetInstance().GetMoney(charID);
        std::vector<BYTE> moneyBuf(13); PACKET_HEADER* mHead = (PACKET_HEADER*)moneyBuf.data();
        mHead->id = 0x3B13; mHead->payloadSize = 9;
        *((INT64*)(moneyBuf.data() + 4)) = (INT64)currentMoney; moneyBuf[12] = 0;
        EncryptPacket(moneyBuf.data(), 0x42); SafeSend(clientSocket, (const char*)moneyBuf.data(), moneyBuf.size(), 0);
    }
    
    // [已删除] 此处曾有遗留的材料消耗代码（Phase 1/Phase 2 重构前残留），
    // 与下方 Phase 1 的材料处理逻辑完全重复，导致双重扣材料 BUG。

    // 校验改造次数上限：每件装备最多允许 MAX_REBUILD_ATTEMPTS 次改造尝试
    if (currentAttempts >= MAX_REBUILD_ATTEMPTS) {
        std::vector<BYTE> limitBuf; limitBuf.resize(4);
        limitBuf.push_back(6); // ERR_REBUILDITEM_FULL：客户端已有"无法再进行改造"的提示
        limitBuf.push_back(255); pushDWord(limitBuf, 0);
        pushString(limitBuf, itemName); limitBuf.push_back(currentRebuild);
        PACKET_HEADER* lHead = (PACKET_HEADER*)limitBuf.data();
        lHead->id = 0x4244; lHead->payloadSize = (WORD)(limitBuf.size() - 4);
        EncryptPacket(limitBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)limitBuf.data(), limitBuf.size(), 0);
        LOG("[Rebuild] 改造次数已达上限: itemID=" + std::to_string(dwItemID) + " attempts=" + std::to_string(currentAttempts));
        return;
    }

    int targetLevel = (crystalKind == 1) ? (currentRebuild + 1) : (currentAppend + 1);
    if (targetLevel > 99) return;

    int wAttr = 0, sAttr = 0, lMulti = 0, baseRate = 0, breakChance = 0;
    bool configFound = false;
    if (g_RebuildConfig.count(targetLevel)) {
        auto& cfg = g_RebuildConfig[targetLevel];
        wAttr = cfg.wAttr; sAttr = cfg.sAttr; lMulti = cfg.lMulti;
        baseRate = cfg.baseRate; breakChance = cfg.breakChance;
        configFound = true;
    }

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
            int amount = (int)ItemDB::GetInstance().GetItemAmount(dwResourceID[i]);
            bool amountDecreased = false;
            if (amount > 1) {
                amountDecreased = true;
                ItemDB::GetInstance().DecrementItemAmount(dwResourceID[i]);
                // Prepare 0x4216 packet (amount update)
                std::vector<BYTE> amtBuf(4); amtBuf.push_back(bResourceSackID[i]); amtBuf.push_back(bResourcePos[i]);
                pushDWord(amtBuf, dwResourceID[i]); pushWord(amtBuf, amount - 1);
                PACKET_HEADER* ah = (PACKET_HEADER*)amtBuf.data(); ah->id = 0x4216; ah->payloadSize = amtBuf.size() - 4;
                EncryptPacket(amtBuf.data(), 0x42);
                resRemoves[i].packet = amtBuf;
                resRemoves[i].valid = true;
            }
            if (!amountDecreased) {
                ItemDB::GetInstance().DeleteItemCascade(dwResourceID[i]);
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
        ItemDB::GetInstance().RemoveFromSack(charID, dwItemID);
        ItemDB::GetInstance().DeleteItemData(dwItemID);
        ItemDB::GetInstance().DeleteItem(dwItemID);
        
        itemBreakPacket.resize(7); PACKET_HEADER* rmHead = (PACKET_HEADER*)itemBreakPacket.data();
        rmHead->id = 0x4208; rmHead->payloadSize = 3;
        itemBreakPacket[4] = bSackID; itemBreakPacket[5] = bSackPos; itemBreakPacket[6] = 2;
        EncryptPacket(itemBreakPacket.data(), 0x42);
        hasItemBreakPacket = true;
    } else {
        int totalWujingBonus = 0, totalSujingBonus = 0;
        if (currentRebuild > 0) {
            totalWujingBonus = ItemDB::GetInstance().GetRebuildBonusSum("Wujing_Attr", reqLevel, currentRebuild);
        }
        if (currentAppend > 0) {
            totalSujingBonus = ItemDB::GetInstance().GetRebuildBonusSum("Sujing_Attr", reqLevel, currentAppend);
        }

        int finalData4 = baseData4;
        int finalData5 = baseData5;
        int finalData9 = 0;
        
        if (itemType == 1) { finalData4 += totalWujingBonus; } 
        else if (itemType >= 2 && itemType <= 4) { finalData5 += totalWujingBonus; finalData9 += totalSujingBonus; } 
        else { finalData5 += totalWujingBonus; finalData4 += totalSujingBonus; }

        ItemDB::GetInstance().UpsertRebuildData(dwItemID, finalData4, finalData5, finalData9, currentRebuild, currentAppend, currentAttempts);

        ItemDB::GetInstance().UpdateItemName(dwItemID, newName);
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

    // 2d. 通过聊天系统消息向玩家显示累计改造次数
    {
        std::string attemptMsg;
        if (isSuccess) {
            attemptMsg = "\xB8\xC4\xD4\xEC\xB3\xC9\xB9\xA6\xA3\xA1\xC0\xDB\xBC\xC6\xB8\xC4\xD4\xEC\xB4\xCE\xCA\xFD\xA3\xBA";
        } else if (didBreak) {
            attemptMsg = "\xB8\xC4\xD4\xEC\xCA\xA7\xB0\xDC\xA3\xAC\xD7\xB0\xB1\xB8\xD2\xD1\xCB\xF0\xBB\xD9\xA3\xA1\xC0\xDB\xBC\xC6\xB8\xC4\xD4\xEC\xB4\xCE\xCA\xFD\xA3\xBA";
        } else {
            attemptMsg = "\xB8\xC4\xD4\xEC\xCA\xA7\xB0\xDC\xA1\xA3\xC0\xDB\xBC\xC6\xB8\xC4\xD4\xEC\xB4\xCE\xCA\xFD\xA3\xBA";
        }
        attemptMsg += std::to_string(currentAttempts) + "/" + std::to_string(MAX_REBUILD_ATTEMPTS);
        // 构造 CS_CH_CHAT_ACK (0x3E02) 系统提示包
        std::vector<BYTE> chatBuf;
        chatBuf.resize(4, 0);
        pushDWord(chatBuf, 0);       // dwSenderObjectID = 0（系统消息）
        pushByte(chatBuf, 8);        // type = CT_TIMEMESSAGE
        pushString(chatBuf, attemptMsg);
        WORD chatID = 0x3E02;
        WORD chatPay = (WORD)(chatBuf.size() - 4);
        memcpy(&chatBuf[0], &chatID, 2);
        memcpy(&chatBuf[2], &chatPay, 2);
        EncryptPacket(chatBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)chatBuf.data(), (int)chatBuf.size(), 0);
    }
}

void RegisterRebuildItemHandlers() {
    RegisterHandler(0x4241, [](SOCKET s, BYTE* p, WORD sz) { OnRebuildItemTermReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x4243, [](SOCKET s, BYTE* p, WORD sz) { OnRebuildItemReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
}

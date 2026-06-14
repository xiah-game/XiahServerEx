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
#include "../GameObjects/PlayerManager.h"

// 每件装备允许的最大改造总次数（成功+失败均计入）
static const int MAX_REBUILD_ATTEMPTS = 201;

// 装备强化白名单：仅允许特�?bType/bKind 组合进行强化
// bType=1(武器,排除bKind=8矿工�?�?(衣服)�?(帽子)�?(鞋子)�?(戒指)�?(项链)
// bType=5(披风无数�?�?(宝石)�?(盾牌/法宝) 均不可强�?
static bool IsRebuildableItem(BYTE bType, BYTE bKind) {
    switch (bType) {
        case 1: return bKind != 8; // 武器（排除矿工锤 bKind=8�?
        case 2: // 衣服
        case 3: // 帽子
        case 4: // 鞋子
        case 5: // 披风
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
            pushDWord(bi, d9); pushDWord(bi, d10); pushDWord(bi, d11); pushDWord(bi, d12); pushWord(bi, d13);
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
                case 16: pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
                case 18: pushByte(bi, 0); pushDWord(bi, 0); pushWord(bi, d2); pushWord(bi, d3); pushByte(bi, 0); break;
                case 19: pushWord(bi, 0); pushWord(bi, 0); break;
                case 20: pushByte(bi, 0); pushDWord(bi, 0); break;
                case 21: pushWord(bi, lvl); pushDWord(bi, nd2); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 1); break;
                case 22: pushDWord(bi, nd2); pushByte(bi, (BYTE)nd3); pushWord(bi, (WORD)nd4); pushWord(bi, (WORD)nd5); break;
                case 23: pushDWord(bi, 0); pushDWord(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
                case 25: pushByte(bi, 0); pushWord(bi, (WORD)nd2); pushWord(bi, (WORD)nd3); break; // 第一字节=m_bIsDividedRes，必须为0才能放入改造槽�?=客户端拒绝）
                case 27: pushByte(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
                case 29: pushWord(bi, 0); pushWord(bi, 0); break;
                case 32: pushWord(bi, (WORD)nd2); pushWord(bi, (WORD)nd3); pushWord(bi, (WORD)nd3); pushDWord(bi, 0); break;
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
        // 查模板获?bType/bKind，校验是否可强化
        WORD refID = ItemDB::GetInstance().GetItemRefID(dwItemID);
        if (refID > 0 && g_ItemTemplates.count(refID)) {
            BYTE bType = g_ItemTemplates[refID].bType;
            BYTE bKind = g_ItemTemplates[refID].bKind;
            if (IsRebuildableItem(bType, bKind)) {
                // 校验强化/追加等级上限：两者都已满99时，不允许继续改造
                ItemDB::FullItemRow termRow;
                if (ItemDB::GetInstance().GetFullItemData(dwItemID, termRow)) {
                    int curRebuild = termRow.d[13]; // nData14 = 强化等级
                    int curAppend  = termRow.d[14]; // nData15 = 追加等级
                    if (curRebuild >= 99 && curAppend >= 99) {
                        result = 3; // 强化和追加均已满级，拒绝改造
                        LOG("[Rebuild] 拒绝放入改造槽：强化(" + std::to_string(curRebuild) + ")和追加(" + std::to_string(curAppend) + ")均已达上限 itemID=" + std::to_string(dwItemID));
                    } else {
                        result = 0; // 校验通过
                    }
                } else {
                    result = 0; // 无 ITEMDATA 记录说明从未改造过，允许放入
                }
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
    LOG("[Rebuild] OnRebuildItemReq: charID=" + std::to_string(charID) + " payloadSize=" + std::to_string(totalSize));
    
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
    int baseData1 = 0, baseData2 = 0, baseData3 = 0, baseData4 = 0, baseData5 = 0, baseData9 = 0;
    
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
            baseData9 = g_ItemTemplates[refid].nData9;
        }
        // 白名单校验：拒绝不可强化的物品类�?
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
    
    // ====================================================================
    // 改造系�?v2：材料槽校验 + 三分支逻辑
    // ====================================================================
    
    // 血晶固定基础成功�?
    static const int BLOOD_CRYSTAL_BASE_RATE = 50;
    
    // 遍历 3 个材料槽，识别主晶石类型、累加成功率、检测防爆卷
    int crystalKind = 0;        // 主晶�?bKind (1=乌晶, 2=素晶, 3=血�?
    int crystalAttrType = 0;    // 素晶子类�?nBasicData1�?=随机, 2-8=指定属性）
    int totalSuccessBonus = 0;  // 所有晶�?nBasicData2 之和
    bool hasCrystal = false;
    bool hasAntiBreak = false;  // 防爆卷标�?
    
    for (int i = 0; i < 3; i++) {
        if (dwResourceID[i] == 0) continue;
        WORD resRef = ItemDB::GetInstance().GetItemRefID(dwResourceID[i]);
        if (resRef == 0 || !g_ItemTemplates.count(resRef)) continue;
        
        auto& resTpl = g_ItemTemplates[resRef];
        
        if (resTpl.bType == 25) {
            // bKind=99 -> Anti-Break Talisman
            if (resTpl.bKind == 99) {
                hasAntiBreak = true;
                LOG("[Rebuild] Anti-break talisman detected: slot=" + std::to_string(i) + " refID=" + std::to_string(resRef));
                continue;
            }
            // bType=25: crystal stone logic
            if (!hasCrystal) {
                crystalKind = resTpl.bKind;
                crystalAttrType = resTpl.nBasicData1;
                totalSuccessBonus += resTpl.nBasicData2;
                hasCrystal = true;
            } else {
                if (resTpl.bKind != crystalKind) {
                    std::vector<BYTE> ackBuf(5);
                    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
                    head->id = 0x4244; head->payloadSize = 1; ackBuf[4] = 5;
                    EncryptPacket(ackBuf.data(), 0x42);
                    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
                    LOG("[Rebuild] Rejected mixed crystal types: slot=" + std::to_string(i));
                    return;
                }
                totalSuccessBonus += resTpl.nBasicData2;
            }
        } else {
            // Unknown non-crystal material in slot, ignore safely
            LOG("[Rebuild] Unknown material ignored: slot=" + std::to_string(i) + " bType=" + std::to_string(resTpl.bType) + " refID=" + std::to_string(resRef));
        }
    }

    if (!hasCrystal || (crystalKind != 1 && crystalKind != 2 && crystalKind != 3)) {
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4244; head->payloadSize = 1; ackBuf[4] = 5; // FAIL
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
        return;
    }

    // 扣金�?
    DWORD rebuildCost = 10000;
    {
        CharacterDB::CharPower cp;
        CharacterDB::GetInstance().GetCharData(charID, cp);
        if (cp.dwMoney < rebuildCost) {
            std::vector<BYTE> ackBuf(5); PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = 0x4244; head->payloadSize = 1; ackBuf[4] = 1; // NOMONEY
            EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0); return;
        }
    }
    
    CharacterDB::GetInstance().SubtractMoney(charID, rebuildCost);
    {
        INT64 currentMoney = CharacterDB::GetInstance().GetMoney(charID);
        std::vector<BYTE> moneyBuf(13); PACKET_HEADER* mHead = (PACKET_HEADER*)moneyBuf.data();
        mHead->id = 0x3B13; mHead->payloadSize = 9;
        *((INT64*)(moneyBuf.data() + 4)) = (INT64)currentMoney; moneyBuf[12] = 0;
        EncryptPacket(moneyBuf.data(), 0x42); SafeSend(clientSocket, (const char*)moneyBuf.data(), moneyBuf.size(), 0);
    }

    // 校验改造次数上�?
    if (currentAttempts >= MAX_REBUILD_ATTEMPTS) {
        std::vector<BYTE> limitBuf; limitBuf.resize(4);
        limitBuf.push_back(6); // ERR_REBUILDITEM_FULL
        limitBuf.push_back(255); pushDWord(limitBuf, 0);
        pushString(limitBuf, itemName); limitBuf.push_back(currentRebuild);
        PACKET_HEADER* lHead = (PACKET_HEADER*)limitBuf.data();
        lHead->id = 0x4244; lHead->payloadSize = (WORD)(limitBuf.size() - 4);
        EncryptPacket(limitBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)limitBuf.data(), limitBuf.size(), 0);
        return;
    }
    
    currentAttempts++;
    
    // ====================================================================
    // 根据装备 bType 确定乌晶主属性字段编�?
    // ====================================================================
    // 返回 nData 字段编号�?,5,6,9,10,13），-1 表示不支�?
    auto GetPrimaryAttrField = [](int bType) -> int {
        switch (bType) {
            case 1: return 4;           // bType=1 ����(��) �� nData4(������)
            case 2: return 5;           // bType=2 �·�     �� nData5(������)
            case 3: return 6;           // bType=3 ñ��     �� nData6(������)
            case 4: return 13;          // bType=4 Ь��     �� nData13(������) [�ٷֱ�]
            case 5: return 5;           // bType=5 披风     -> nData5(防御力)
            case 6: return 9;           // bType=6 ��ָ     �� nData9(���HP)
            case 7: return 10;          // bType=7 ����     �� nData10(���IP)
            default: return -1;         // bType=5(����)�Ȳ�֧���ھ�������ǿ��
        }
    };
    
    // 素晶副属性池：nBasicData1 ?nData字段编号
    // 排除当前装备的主属性后，从有追加值的属性中随机?
    auto GetSujingAttrField = [](int nBasicData1) -> int {
        switch (nBasicData1) {
            case 2: return 4;   // 攻击力
            case 3: return 5;   // 防御力
            case 4: return 6;   // 命中率
            case 5: return 13;  // 暴击率 [百分比]
            case 6: return 9;   // 最大HP
            case 7: return 10;  // 最大MP
            case 8: return 11;  // 生命恢复
            case 9: return 12;  // 内力恢复
            default: return -1;
        }
    };
    
    // 判断字段是否为百分比类型（暴击率 nData13?
    auto IsPercentField = [](int fieldNum) -> bool {
        return fieldNum == 13;
    };
    
    // 获取模板?nData 字段的基础?
    auto GetTemplateNData = [&](int fieldNum) -> int {
        if (!g_ItemTemplates.count(fir.wRefID)) return 0;
        auto& tpl = g_ItemTemplates[fir.wRefID];
        switch (fieldNum) {
            case 4: return tpl.nData4;
            case 5: return tpl.nData5;
            case 6: return tpl.nData6;
            case 7: return tpl.nData7;
            case 8: return tpl.nData8;
            case 9: return tpl.nData9;
            case 10: return tpl.nData10;
            case 11: return tpl.nData11;
            case 12: return tpl.nData12;
            case 13: return tpl.nData13;
            default: return 0;
        }
    };
    
    // 获取当前 ITEMDATA 中的 nData 字段?
    auto GetCurrentNData = [&](int fieldNum) -> int {
        // fir.d[] 映射: d[0]=nData1 .. d[12]=nData13, d[13]=nData14 ..
        if (fieldNum >= 1 && fieldNum <= 17) return fir.d[fieldNum - 1];
        return 0;
    };
    
    // ?REBUILD_CONFIG 全量重算属性加成（SUM 1..level 的每级?× 等级系数?
    auto CalcTotalBonus = [&](const std::string& attrCol, int level) -> int {
        if (level <= 0) return 0;
        return ItemDB::GetInstance().GetRebuildBonusSum(attrCol, reqLevel, level);
    };
    
    // nData 字段编号 ?GBK 属性名称（用于系统消息?
    auto GetAttrName = [](int fieldNum) -> std::string {
        switch (fieldNum) {
            case 4:  return "\xB9\xA5\xBB\xF7";       // 攻击
            case 5:  return "\xB7\xC0\xD3\xF9";       // 防御
            case 6:  return "\xC3\xFC\xD6\xD0";       // 命中
            case 9:  return "HP";
            case 10: return "MP";
            case 11: return "\xBB\xD8\xB8\xB4HP";     // 回复HP
            case 12: return "\xBB\xD8\xB8\xB4MP";     // 回复MP
            case 13: return "\xB1\xA9\xBB\xF7";       // 暴击
            default: return "\xCA\xF4\xD0\xD4";       // 属?
        }
    };
    
    // ====================================================================
    // 三分支判?
    // ====================================================================
    bool isSuccess = false;
    bool didBreak = false;
    std::map<int, int> dbFields; // 要写?ITEMDATA 的字?
    bool updateItemTable = false; // 是否需要更?ITEM ?
    int newNBasicData1 = 0;       // 血晶降级用
    int attrFieldChanged = 0;     // 本次变化的属性字段编?
    int attrDelta = 0;            // 属性变化量（正=增加, ?减少?
    
    if (crystalKind == 1) {
        // ==================== 乌晶强化 ====================
        // 强化等级已达上限99，拒绝继续乌晶改造
        if (currentRebuild >= 99) {
            std::vector<BYTE> capBuf; capBuf.resize(4);
            capBuf.push_back(6); // ERR_REBUILDITEM_FULL
            capBuf.push_back(255); pushDWord(capBuf, 0);
            pushString(capBuf, itemName); capBuf.push_back((BYTE)currentRebuild);
            PACKET_HEADER* cHead = (PACKET_HEADER*)capBuf.data();
            cHead->id = 0x4244; cHead->payloadSize = (WORD)(capBuf.size() - 4);
            EncryptPacket(capBuf.data(), 0x42);
            SafeSend(clientSocket, (const char*)capBuf.data(), capBuf.size(), 0);
            LOG("[Rebuild] 乌晶强化拒绝：强化等级已达上限99 itemID=" + std::to_string(dwItemID));
            return;
        }
        int targetLevel = currentRebuild + 1;
        
        int wAttr = 0, lMulti = 0, baseRate = 0, breakChance = 0;
        if (g_RebuildConfig.count(targetLevel)) {
            auto& cfg = g_RebuildConfig[targetLevel];
            wAttr = cfg.wAttr; lMulti = cfg.lMulti;
            baseRate = cfg.baseRate; breakChance = cfg.breakChance;
        }
        
        int finalSuccessRate = baseRate + totalSuccessBonus;
        isSuccess = (rand() % 100) < finalSuccessRate;
        
        int primaryField = GetPrimaryAttrField(itemType);
        if (primaryField < 0) return; // 不支持的装备类型
        
        if (isSuccess) {
            currentRebuild++;
        } else {
            if (currentRebuild > 0) currentRebuild--;
            // 破碎判定（防爆卷可免疫）
            if (!hasAntiBreak && (rand() % 100) < breakChance) {
                didBreak = true;
            }
        }
        
        if (!didBreak) {
            // 全量重算主属?
            int oldVal = GetCurrentNData(primaryField);
            int baseVal = GetTemplateNData(primaryField);
            int bonus;
            if (IsPercentField(primaryField)) {
                bonus = currentRebuild;
            } else {
                bonus = CalcTotalBonus("Wujing_Attr", currentRebuild);
            }
            int finalVal = baseVal + bonus;
            
            attrFieldChanged = primaryField;
            attrDelta = finalVal - oldVal;
            
            dbFields[primaryField] = finalVal;
            dbFields[14] = currentRebuild;
            dbFields[17] = currentAttempts;
        }
        
    } else if (crystalKind == 2) {
        // ==================== 素晶追加 ====================
        // 追加等级已达上限99，拒绝继续素晶改造
        if (currentAppend >= 99) {
            std::vector<BYTE> capBuf; capBuf.resize(4);
            capBuf.push_back(6); // ERR_REBUILDITEM_FULL
            capBuf.push_back(255); pushDWord(capBuf, 0);
            pushString(capBuf, itemName); capBuf.push_back((BYTE)currentRebuild);
            PACKET_HEADER* cHead = (PACKET_HEADER*)capBuf.data();
            cHead->id = 0x4244; cHead->payloadSize = (WORD)(capBuf.size() - 4);
            EncryptPacket(capBuf.data(), 0x42);
            SafeSend(clientSocket, (const char*)capBuf.data(), capBuf.size(), 0);
            LOG("[Rebuild] 素晶追加拒绝：追加等级已达上限99 itemID=" + std::to_string(dwItemID));
            return;
        }
        int targetLevel = currentAppend + 1;
        
        int sAttr = 0, lMulti = 0, baseRate = 0;
        if (g_RebuildConfig.count(targetLevel)) {
            auto& cfg = g_RebuildConfig[targetLevel];
            sAttr = cfg.sAttr; lMulti = cfg.lMulti;
            baseRate = cfg.baseRate;
        }
        
        int finalSuccessRate = baseRate + totalSuccessBonus;
        isSuccess = (rand() % 100) < finalSuccessRate;
        
        int primaryField = GetPrimaryAttrField(itemType); // 装备主属性（素晶失败时排除此属性）
        
        if (isSuccess) {
            currentAppend++;
            
            // 选择追加的属?
            int chosenField = -1;
            if (crystalAttrType >= 2 && crystalAttrType <= 9) {
                // 指定类型素晶
                chosenField = GetSujingAttrField(crystalAttrType);
            } else {
                // 随机从 8 种副属性中选
                int candidates[] = {4, 5, 6, 9, 10, 11, 12, 13};
                chosenField = candidates[rand() % 8];
            }
            
            if (chosenField > 0) {
                int curVal = GetCurrentNData(chosenField);
                int addVal;
                if (IsPercentField(chosenField)) {
                    addVal = 1;
                } else {
                    addVal = sAttr * reqLevel * lMulti / 100;
                    if (addVal < 1) addVal = 1;
                }
                dbFields[chosenField] = curVal + addVal;
                attrFieldChanged = chosenField;
                attrDelta = addVal;
            }
            
            dbFields[15] = currentAppend;
            dbFields[17] = currentAttempts;
            
        } else {
            // 失败：随机清零一项有追加值的非主属?
            // 收集所有有追加值的副属?
            int allFields[] = {4, 5, 6, 9, 10, 11, 12, 13};
            std::vector<int> enhanced;
            for (int f : allFields) {
                if (f == primaryField) continue; // 排除主属?
                int curVal = GetCurrentNData(f);
                int baseVal = GetTemplateNData(f);
                if (curVal > baseVal) {
                    enhanced.push_back(f);
                }
            }
            
            if (!enhanced.empty()) {
                int clearField = enhanced[rand() % enhanced.size()];
                int curVal = GetCurrentNData(clearField);
                int baseVal = GetTemplateNData(clearField);
                dbFields[clearField] = baseVal;
                attrFieldChanged = clearField;
                attrDelta = baseVal - curVal; // 负?
            }
            
            // nData15 不减！只更新尝试次数
            dbFields[17] = currentAttempts;
        }
        
    } else if (crystalKind == 3) {
        // ==================== 血晶降?====================
        int finalSuccessRate = BLOOD_CRYSTAL_BASE_RATE + totalSuccessBonus;
        isSuccess = (rand() % 100) < finalSuccessRate;
        
        if (isSuccess) {
            // 读取当前 ITEM.nBasicData1
            int curReqLevel = ItemDB::GetInstance().GetItemNBasicData(dwItemID, 1);
            if (curReqLevel <= 0) curReqLevel = reqLevel; // fallback
            int newLevel = curReqLevel - 1;
            if (newLevel < 1) newLevel = 1;
            newNBasicData1 = newLevel;
            updateItemTable = true;
        }
        
        // 血晶只更新尝试次数
        dbFields[17] = currentAttempts;
    }
    
    // ====================================================================
    // 物品名称更新
    // ====================================================================
    std::string newName = baseItemName;
    if (!didBreak) {
        if (currentRebuild > 0) newName = "+" + std::to_string(currentRebuild) + " " + newName;
        if (currentAppend > 0) newName += " (" + std::to_string(currentAppend) + "\xD0\xD4\xD6\xCA)";
    }

    // =====================================================================
    // PHASE 1: 所?DB 操作（不发包?
    // =====================================================================
    
    // 1a. 材料消?
    struct ResourceRemoveInfo {
        std::vector<BYTE> packet;
        bool valid;
    };
    ResourceRemoveInfo resRemoves[3] = {};
    
    for (int i=0; i<3; i++) {
        resRemoves[i].valid = false;
        if (dwResourceID[i] != 0) {
            int amount = (int)ItemDB::GetInstance().GetItemAmount(dwResourceID[i]);
            if (amount > 1) {
                ItemDB::GetInstance().DecrementItemAmount(dwResourceID[i]);
                std::vector<BYTE> amtBuf(4); amtBuf.push_back(bResourceSackID[i]); amtBuf.push_back(bResourcePos[i]);
                pushDWord(amtBuf, dwResourceID[i]); pushWord(amtBuf, amount - 1);
                PACKET_HEADER* ah = (PACKET_HEADER*)amtBuf.data(); ah->id = 0x4216; ah->payloadSize = amtBuf.size() - 4;
                EncryptPacket(amtBuf.data(), 0x42);
                resRemoves[i].packet = amtBuf;
                resRemoves[i].valid = true;
            } else {
                ItemDB::GetInstance().DeleteItemCascade(dwResourceID[i]);
                std::vector<BYTE> rmBuf(7); PACKET_HEADER* ah = (PACKET_HEADER*)rmBuf.data();
                ah->id = 0x4208; ah->payloadSize = 3;
                rmBuf[4] = bResourceSackID[i]; rmBuf[5] = bResourcePos[i]; rmBuf[6] = 2;
                EncryptPacket(rmBuf.data(), 0x42);
                resRemoves[i].packet = rmBuf;
                resRemoves[i].valid = true;
            }
        }
    }
    
    // 1b. 装备破碎 ?属性写?
    std::vector<BYTE> itemBreakPacket;
    bool hasItemBreakPacket = false;
    
    if (didBreak) {
        ItemDB::GetInstance().RemoveFromSack(charID, dwItemID);
        ItemDB::GetInstance().DeleteItemData(dwItemID);
        ItemDB::GetInstance().DeleteItem(dwItemID);
        
        itemBreakPacket.resize(7); PACKET_HEADER* rmHead = (PACKET_HEADER*)itemBreakPacket.data();
        rmHead->id = 0x4208; rmHead->payloadSize = 3;
        itemBreakPacket[4] = bSackID; itemBreakPacket[5] = bSackPos; itemBreakPacket[6] = 2;
        EncryptPacket(itemBreakPacket.data(), 0x42);
        hasItemBreakPacket = true;
    } else {
        // 写入属性到 ITEMDATA
        if (!dbFields.empty()) {
            ItemDB::GetInstance().UpsertRebuildData(dwItemID, dbFields);
        }
        // 血晶降级：更新 ITEM.nBasicData1
        if (updateItemTable) {
            ItemDB::GetInstance().UpdateItemNBasicData1(dwItemID, newNBasicData1);
        }
        // 更新物品名称
        ItemDB::GetInstance().UpdateItemName(dwItemID, newName);
    }

    // =====================================================================
    // PHASE 2: 批量发包（同帧到达，?UI 闪烁?
    // =====================================================================
    
    // 2-pre. 系统消息必须?0x4244 之前发送，因为客户端收?0x4244 ?
    //        立即触发 HideSack 重建 UI，后续聊天包会被丢弃或不显示
    {
        // 构造属性变化描?
        std::string attrChangeStr;
        if (attrFieldChanged > 0 && attrDelta != 0) {
            std::string attrName = GetAttrName(attrFieldChanged);
            if (attrDelta > 0) {
                attrChangeStr = " +" + std::to_string(attrDelta) + attrName;
            } else {
                attrChangeStr = " " + std::to_string(attrDelta) + attrName;
            }
        } else if (crystalKind == 3 && isSuccess) {
            attrChangeStr = " \xB5\xC8\xBC\xB6\xD0\xE8\xC7\xF3-1"; // 等级需?1
        }
        
        std::string attemptMsg;
        if (isSuccess) {
            attemptMsg = "\xB8\xC4\xD4\xEC\xB3\xC9\xB9\xA6\xA3\xA1"; // 改造成功！
        } else if (didBreak) {
            attemptMsg = "\xB8\xC4\xD4\xEC\xCA\xA7\xB0\xDC\xA3\xAC\xD7\xB0\xB1\xB8\xD2\xD1\xCB\xF0\xBB\xD9\xA3\xA1"; // 改造失败，装备已损毁！
        } else {
            attemptMsg = "\xB8\xC4\xD4\xEC\xCA\xA7\xB0\xDC\xA1\xA3"; // 改造失败?
        }
        attemptMsg += attrChangeStr;
        LOG("[Rebuild] Result: success=" + std::to_string(isSuccess) + " break=" + std::to_string(didBreak) + " kind=" + std::to_string(crystalKind) + " attrField=" + std::to_string(attrFieldChanged) + " delta=" + std::to_string(attrDelta));
        std::vector<BYTE> chatBuf;
        chatBuf.resize(4, 0);
        pushDWord(chatBuf, 0);
        pushByte(chatBuf, 8); // CT_TIMEMESSAGE
        pushString(chatBuf, attemptMsg);
        WORD chatID = 0x3E02;
        WORD chatPay = (WORD)(chatBuf.size() - 4);
        memcpy(&chatBuf[0], &chatID, 2);
        memcpy(&chatBuf[2], &chatPay, 2);
        EncryptPacket(chatBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)chatBuf.data(), (int)chatBuf.size(), 0);
    }

    // nData 字段编号 ?客户?bFactor 索引映射
    auto GetClientFactor = [](int nDataField) -> BYTE {
        switch (nDataField) {
            case 4:  return 0;   // 攻击?
            case 5:  return 1;   // 防御?
            case 6:  return 2;   // 命中?
            case 13: return 3;   // 暴击?
            case 9:  return 4;   // 最大生命力
            case 10: return 5;   // 最大内力
            case 11: return 6;   // 自动生命力恢复
            case 12: return 7;   // 自动内力恢复
            default: return 255; // 不显?
        }
    };
    
    BYTE clientFactor = (attrFieldChanged > 0 && attrDelta != 0) ? GetClientFactor(attrFieldChanged) : 255;
    // 血晶降级特殊处理：bFactor=8 对应"要求等级"
    if (crystalKind == 3 && isSuccess) { clientFactor = 8; attrDelta = -1; }
    int clientNValues = attrDelta;

    // 2a. 0x4244 改造结�?
    std::vector<BYTE> ackBuf; ackBuf.resize(4);
    if (didBreak) {
        ackBuf.push_back(5); ackBuf.push_back(clientFactor);
        pushDWord(ackBuf, clientNValues); pushString(ackBuf, itemName); ackBuf.push_back(currentRebuild);
    } else if (isSuccess) {
        ackBuf.push_back(0); // SUCCESS
        ackBuf.push_back(clientFactor);
        pushDWord(ackBuf, clientNValues); pushString(ackBuf, newName); ackBuf.push_back(currentRebuild);
    } else {
        ackBuf.push_back(5); // FAIL
        ackBuf.push_back(clientFactor);
        pushDWord(ackBuf, clientNValues); pushString(ackBuf, newName); ackBuf.push_back(currentRebuild);
    }
    
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4244;
    head->payloadSize = (WORD)(ackBuf.size() - 4);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);

    // 2b. 材料移除�?
    for (int i=0; i<3; i++) {
        if (resRemoves[i].valid) {
            SafeSend(clientSocket, (const char*)resRemoves[i].packet.data(), resRemoves[i].packet.size(), 0);
        }
    }

    // 2c. 装备破碎 �?刷新
    if (hasItemBreakPacket) {
        SafeSend(clientSocket, (const char*)itemBreakPacket.data(), itemBreakPacket.size(), 0);
    } else {
        SendItemRefresh(clientSocket, dwItemID, bSackID, bSackPos);
        // 改造成功后，如果装备已穿戴（bSackPos < 20），需重算角色属性面�?
        if (isSuccess && bSackPos < 20) {
            PlayerManager::GetInstance().RecalculateStats(charID, true);
        }
    }

}

void RegisterRebuildItemHandlers() {
    RegisterHandler(0x4241, [](SOCKET s, BYTE* p, WORD sz) { OnRebuildItemTermReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x4243, [](SOCKET s, BYTE* p, WORD sz) { OnRebuildItemReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
}

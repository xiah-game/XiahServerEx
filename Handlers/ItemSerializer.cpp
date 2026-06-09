#include "ItemSerializer.h"
#include "../GameObjects/MugongManager.h"

// 内部 static helpers，不暴露给其他编译单元
static void pushByte(std::vector<BYTE>& buf, BYTE b) { buf.push_back(b); }
static void pushWord(std::vector<BYTE>& buf, WORD w) { buf.push_back(w & 0xFF); buf.push_back((w >> 8) & 0xFF); }
static void pushDWord(std::vector<BYTE>& buf, DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back(d >> 24); }

extern std::map<WORD, sItemTemplate> g_ItemTemplates;

void SerializeItemData(const ItemDB::FullItemRow& row, std::vector<BYTE>& bi) {
    int vis = row.wVisualID, type = row.bType, kind = row.bKind, lvl = row.wLevel, cost = row.dwCost;
    int dat18 = row.nData18, dat19 = row.nData19, dat20 = row.nData20, dat21 = row.nData21, dat25 = row.nData25;
    int refid = row.wRefID, amount = row.wAmount;
    int d[17];
    for (int i = 0; i < 17; i++) d[i] = row.d[i];

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
        if (d[0] == -9999) d[0] = g_ItemTemplates[refid].nData1;
        if (d[1] == -9999) d[1] = g_ItemTemplates[refid].nData2;
        if (d[2] == -9999) d[2] = g_ItemTemplates[refid].nData3;
        if (d[3] == -9999) d[3] = g_ItemTemplates[refid].nData4;
        if (d[4] == -9999) d[4] = g_ItemTemplates[refid].nData5;
        if (d[5] == -9999) d[5] = g_ItemTemplates[refid].nData6;
        if (d[6] == -9999) d[6] = g_ItemTemplates[refid].nData7;
        if (d[7] == -9999) d[7] = g_ItemTemplates[refid].nData8;
        if (d[8] == -9999) d[8] = g_ItemTemplates[refid].nData9;
        if (d[9] == -9999) d[9] = g_ItemTemplates[refid].nData10;
    }
    for (int i = 0; i < 17; i++) { if (d[i] == -9999) d[i] = 0; }

    std::string itemName(row.szName);
    if (itemName.empty() && g_ItemTemplates.count(refid)) itemName = g_ItemTemplates[refid].szName;

    pushDWord(bi, row.dwItemID); pushWord(bi, refid);
    pushByte(bi, type); pushByte(bi, kind); pushWord(bi, vis);
    // 物品名称：WORD长度 + 字节数据（不含 null 终止符，与客户端 sString 解析对齐）
    pushWord(bi, (WORD)itemName.length());
    for (char ch : itemName) pushByte(bi, ch);
    pushDWord(bi, cost); pushWord(bi, lvl); pushByte(bi, charType);
    pushWord(bi, amount);

    if (type >= 1 && type <= 9) {
        // 装备类：nBasicData1-5（需求属性） + nData1-17（装备属性）+ 类型专属尾部
        pushWord(bi, nd1); pushWord(bi, nd2); pushWord(bi, nd3); pushWord(bi, nd4); pushWord(bi, nd5);
        pushByte(bi, d[0]);
        pushWord(bi, d[1]); pushWord(bi, d[2]);
        pushDWord(bi, d[3]); pushDWord(bi, d[4]); pushDWord(bi, d[5]); pushWord(bi, d[6]); pushWord(bi, d[7]);
        pushDWord(bi, d[8]); pushDWord(bi, d[9]); pushDWord(bi, d[10]); pushDWord(bi, d[11]); pushWord(bi, d[12]);
        pushByte(bi, dat18); pushByte(bi, dat19);
        pushByte(bi, d[13]); pushByte(bi, d[14]); pushByte(bi, d[15]); pushByte(bi, d[16]);
        if (type == 9) {
            // 坐骑（Bongin）：额外 12 字节
            pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0);
        } else if (type == 8) {
            // 宝石插槽（Socket）：8 字节
            for (int i = 0; i < 8; i++) pushByte(bi, 0);
        } else {
            // 其他装备：1 字节 bPuzzleType
            pushByte(bi, 0);
        }
        if (type >= 1 && type <= 4) {
            // 武器/衣服/帽子/鞋子：强化等级 + 插槽数据
            pushByte(bi, dat19); pushByte(bi, dat20); pushByte(bi, dat21); pushWord(bi, dat25);
        }
    } else {
        // 非装备类：按 bType 分别序列化
        switch (type) {
            // NPC 饰品 / 坐骑 / 鞍
            case 11: case 12: case 13: case 14: case 17:
                pushByte(bi, 0); pushWord(bi, d[1]); pushWord(bi, d[2]); break;
            // NPC 包裹
            case 15:
                pushByte(bi, d[0]); pushWord(bi, d[1]); pushWord(bi, d[2]); pushByte(bi, 0); pushByte(bi, 0); break;
            // NPC 物品
            case 16:
                pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); break;
            // 修炼石 / 变身道具
            case 18:
                pushByte(bi, 0); pushDWord(bi, 0); pushWord(bi, d[1]); pushWord(bi, d[2]); pushByte(bi, 0); break;
            // 活动物品
            case 19:
                pushWord(bi, 0); pushWord(bi, 0); break;
            // 纯资源
            case 20:
                pushByte(bi, 0); pushDWord(bi, 0); break;
            // 技能书：查 MugongManager 获取武功类型信息
            case 21: {
                DWORD mid = nd2;
                sMugongTemplate* mg = MugongManager::GetInstance()->GetTemplate(mid);
                BYTE typeVal = mg ? mg->bType : 0;
                BYTE kindVal = mg ? mg->bKind : 0;
                pushWord(bi, lvl); pushDWord(bi, mid); pushByte(bi, typeVal); pushByte(bi, kindVal); pushByte(bi, 1);
                break;
            }
            // 传送卷轴
            case 22:
                pushDWord(bi, nd2); pushByte(bi, (BYTE)nd3); pushWord(bi, (WORD)nd4); pushWord(bi, (WORD)nd5); break;
            // 药品
            case 23:
                pushDWord(bi, 0); pushDWord(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
            // 改造材料（乌晶/素晶/血晶）：m_bIsDividedRes(必须为0) + 成功率(nBasicData2) + 系数值(nBasicData3)
            // 注意：客户端将此字节=1视为"不可改造"，乌晶bKind恰好=1会被拒绝，故固定发0
            case 25:
                pushByte(bi, 0); pushWord(bi, (WORD)nd2); pushWord(bi, (WORD)nd3); break;
            // 彩票
            case 27:
                pushByte(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
            // 合成手册
            case 29:
                pushWord(bi, 0); pushWord(bi, 0); break;
            // bType=32 (GISDURABLITY): m_wFunctionItem(nd2) + m_wCurDur(nd3) + m_wMaxDur(nd3) + m_dwValue(0)
            case 32:
                pushWord(bi, (WORD)nd2); pushWord(bi, (WORD)nd3); pushWord(bi, (WORD)nd3); pushDWord(bi, 0); break;
            // 转生道具
            case 31:
                pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
            // 特权任务
            case 34:
                pushDWord(bi, 0); pushByte(bi, 0); break;
        }
    }
}

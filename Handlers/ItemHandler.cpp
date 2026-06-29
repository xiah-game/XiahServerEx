#include "ItemHandler.h"
#include "../DB/CharacterDB.h"
#include "../DB/GameDataDB.h"
#include "../DB/ItemDB.h"
#include "../GameObjects/MugongManager.h"
#include "ItemSerializer.h"
#include "../GameObjects/TitleManager.h"
#include "../GameObjects/DropManager.h"
#include <set>
#include <algorithm>
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"

extern std::map<DWORD, CMapInstance*> g_MapInstances;
#include "ShopHandler.h"
#include "../Network/SystemMessage.h"
#include "../DBHelper.h"

// 外部声明，用于打开随身仓库
void OnItemListInBankReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);
// 外部声明，用于 Remove+Add 模式刷新客户端物品数据（定义在 RebuildItemHandler.cpp）
void SendItemRefresh(SOCKET clientSocket, DWORD dwItemID, BYTE bSackID, BYTE bSackPos);

#include <unordered_map>
#include <mutex>

class PetLevelExpManager {
public:
    static PetLevelExpManager& GetInstance() {
        static PetLevelExpManager instance;
        return instance;
    }

    bool LoadFromDB() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_levelMap.clear();

        std::string query = "SELECT wLevel, i64NeedExp FROM PET_LEVEL_EXP";
        bool success = DBHelper::GetInstance().ExecuteQuery(query, [&](SQLHSTMT hStmt) {
            int level = 0;
            long long needExp = 0;
            SQLGetData(hStmt, 1, SQL_INTEGER, &level, 0, NULL);
            SQLGetData(hStmt, 2, SQL_C_SBIGINT, &needExp, 0, NULL);
            m_levelMap[level] = needExp;
        });

        LOG("[PetLevelExpManager] Loaded " + std::to_string(m_levelMap.size()) + " levels data from DB.");
        return success;
    }

    long long GetNeedExp(int level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_levelMap.find(level);
        return (it != m_levelMap.end()) ? it->second : 0;
    }

    long long GetLevelStartExp(int level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        long long sum = 0;
        for (int i = 1; i < level; ++i) {
            auto it = m_levelMap.find(i);
            if (it != m_levelMap.end()) {
                sum += it->second;
            }
        }
        return sum;
    }

private:
    std::unordered_map<int, long long> m_levelMap;
    std::mutex m_mutex;
    PetLevelExpManager() = default;
};

bool LoadPetLevelExp() {
    return PetLevelExpManager::GetInstance().LoadFromDB();
}

void SendCharPremiumList(SOCKET clientSocket, DWORD charID) {
    if (charID == 0) return;
    
    struct BuffNode {
        WORD wRefID;
        BYTE bEndDay;
        BYTE bEndHour;
        BYTE bEndMin;
    };
    std::vector<BuffNode> activeBuffs;
    
    // 利用 DATEDIFF + 分钟模除，直接在 SQL 引擎端完成高精度时区及时间差换算，防止跨天误差！
    std::string query = 
        "SELECT wRefID, "
        "       DATEDIFF(minute, GETDATE(), dateEnd) / 1440 AS endDay, "
        "       (DATEDIFF(minute, GETDATE(), dateEnd) % 1440) / 60 AS endHour, "
        "       DATEDIFF(minute, GETDATE(), dateEnd) % 60 AS endMin "
        "FROM CHAR_PREMIUM "
        "WHERE dwCharID = " + std::to_string(charID) + " AND dateEnd > GETDATE()";
        
    DBHelper::GetInstance().ExecuteQuery(query, [&](SQLHSTMT hStmt) {
        WORD wRefID = 0;
        int d = 0, h = 0, m = 0;
        SQLLEN l1 = 0, l2 = 0, l3 = 0, l4 = 0;
        while (SQLFetch(hStmt) == SQL_SUCCESS) {
            SQLGetData(hStmt, 1, SQL_C_USHORT, &wRefID, 0, &l1);
            SQLGetData(hStmt, 2, SQL_C_LONG, &d, 0, &l2);
            SQLGetData(hStmt, 3, SQL_C_LONG, &h, 0, &l3);
            SQLGetData(hStmt, 4, SQL_C_LONG, &m, 0, &l4);
            
            BuffNode node;
            node.wRefID = wRefID;
            node.bEndDay = (BYTE)d;
            node.bEndHour = (BYTE)h;
            node.bEndMin = (BYTE)m;
            activeBuffs.push_back(node);
        }
    });
    
    // 组装并发送 0x4321 (CS_NV_CHARPREMIUM_ACK) 封包
    std::vector<BYTE> buf;
    buf.resize(4, 0); // 预留包头
    buf.push_back((BYTE)activeBuffs.size()); // bCount
    
    for (auto& b : activeBuffs) {
        buf.push_back(b.wRefID & 0xFF); buf.push_back(b.wRefID >> 8); // wRefID
        buf.push_back(b.bEndDay);   // bEndDay
        buf.push_back(b.bEndHour);  // bEndHour
        buf.push_back(b.bEndMin);   // bEndMin
    }
    
    WORD packetID = 0x4321; // CS_NV_CHARPREMIUM_ACK
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);
    
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
    LOG("[PremiumBuff] Sent " + std::to_string(activeBuffs.size()) + " active buffs to client for charID=" + std::to_string(charID));
}


static void SendSystemWarningChat(SOCKET clientSocket, const std::string& msg) {
    std::vector<BYTE> buf;
    buf.resize(4, 0);
    DWORD senderObjID = 0;
    buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
    buf.push_back(8); // CT_TIMEMESSAGE
    WORD len = (WORD)msg.size();
    buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
    buf.insert(buf.end(), msg.begin(), msg.end());
    WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
}

void OnSackItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (charID > 0) {
        UpdatePlayerStatsAndSend(clientSocket, charID);
    }
}

void OnEquipItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); 
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); 
    ackHead->id = payload[0] == 0 ? 0x441A : 0x441C; 
    ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42); 
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
}

void OnItemListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    BYTE sackId = payload[0];
    std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(sackId);
    
    std::vector<std::vector<BYTE>> items;
    if (charID > 0) {
        std::string posCond = "";
        if (sackId == 0) posCond = "S.bSackPos < 20";
        else if (sackId == 1) posCond = "S.bSackPos >= 20 AND S.bSackPos < 60";
        else if (sackId == 2) posCond = "S.bSackPos >= 60 AND S.bSackPos < 100";
        else posCond = "S.bSackPos >= 100 AND S.bSackPos < 140";
        
        std::vector<ItemDB::FullItemRow> rows;
        ItemDB::GetInstance().GetFullSackItems(charID, posCond, rows);
        
        for (auto& row : rows) {
            int pos = row.bSackPos;
            int type = row.bType, refid = row.wRefID;
            if (type == 0 && g_ItemTemplates.count(refid)) type = g_ItemTemplates[refid].bType;
            
            std::string itemName(row.szName);
            if (itemName.empty() && g_ItemTemplates.count(refid)) itemName = g_ItemTemplates[refid].szName;
            
            std::vector<BYTE> bi;
            bi.push_back(sackId == 0 ? pos : (pos - 20 - (sackId - 1) * 40));
            SerializeItemData(row, bi);
            WORD rebVal = (WORD)row.nData25; bi.push_back(rebVal & 0xFF); bi.push_back(rebVal >> 8); // wRebuithValue
            LOG("[ItemHandler] Item pos=" + std::to_string(pos) + " type=" + std::to_string(type) + " refid=" + std::to_string(refid) + " name=" + itemName + " size=" + std::to_string(bi.size()));
            items.push_back(bi);
        }
    }
    
    ackBuf.push_back(items.size());
    for (auto i : items) ackBuf.insert(ackBuf.end(), i.begin(), i.end());
    LOG("[ItemHandler] ITEM SACK " + std::to_string(sackId) + " LOADED: " + std::to_string(items.size()) + " items");
    
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x4418; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    
    // 当客户端请求加载主装备栏物品 (Sack 0) 时，同步特权 Buff 列表（用于上线和跨图重连）
    if (sackId == 0) {
        SendCharPremiumList(clientSocket, charID);
    }
}

void OnItemMoveReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize >= 8) {
        BYTE bSrcSackID = payload[0];
        BYTE bSrcSackPos = payload[1];
        DWORD dwSrcObjID = *(DWORD*)(payload + 2);
        BYTE bDesSackID = payload[6];
        BYTE bDesSackPos = payload[7];
        DWORD dwDesObjID = 0;
        if (totalSize >= 12) {
            dwDesObjID = *(DWORD*)(payload + 8);
        }

        // 非 VIP 玩家拖拽移入第三页背包的安全静默拦截
        if (bDesSackID == 3) {
            if (CharacterDB::GetInstance().GetVipLevel(charID) == 0) {
                std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(1); // 1 = 失败/复位
                ackBuf.push_back(bSrcSackID); ackBuf.push_back(bSrcSackPos);
                ackBuf.push_back(bDesSackID); ackBuf.push_back(bDesSackPos);
                PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); 
                ackHead->id = 0x420E; 
                ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(ackBuf.data(), 0x42); 
                SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
                return; // 直接静默结束，不再往下进行任何移动和 DB 写入
            }
        }
        
        LOG("[ItemHandler] ITEM MOVE: CharID=" + std::to_string(charID) + " " + std::to_string(bSrcSackID) + ":" + std::to_string(bSrcSackPos) + " -> " + std::to_string(bDesSackID) + ":" + std::to_string(bDesSackPos));
        
        if (charID > 0) {
            // 移动失败时发送 0x420E (bResult=3) 让客户端回滚物品位置并提示"能力不足，无法装备"
            auto sendMoveFailAck = [&]() {
                std::vector<BYTE> failBuf; failBuf.resize(4); failBuf.push_back(3); // 3 = 能力不足(IDS_SHORT_ABLE)
                failBuf.push_back(bSrcSackID); failBuf.push_back(bSrcSackPos);
                failBuf.push_back(bDesSackID); failBuf.push_back(bDesSackPos);
                PACKET_HEADER* fh = (PACKET_HEADER*)failBuf.data();
                fh->id = 0x420E;
                fh->payloadSize = failBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(failBuf.data(), 0x42);
                SafeSend(clientSocket, (const char*)failBuf.data(), failBuf.size(), 0);
            };
            // Check if source and destination are same position

            // Boundary validation: if a specific position is given for a backpack sack,
            // check that the item's bCX×bCY dimensions actually fit within the 6×6 grid.
            // If not, switch to auto-find mode (bDesSackPos=255).
            if (bDesSackID > 0 && bDesSackPos != 255) {
                BYTE bcx = 1, bcy = 1;
                WORD wRefID = 0;
                { ItemDB::ItemBasicInfo ib; if (ItemDB::GetInstance().GetItemBasicInfo(dwSrcObjID, ib)) wRefID = ib.wRefID; }
                if (g_ItemTemplates.count(wRefID)) {
                    bcx = g_ItemTemplates[wRefID].bCX;
                    bcy = g_ItemTemplates[wRefID].bCY;
                }
                if (bcx < 1) bcx = 1; if (bcy < 1) bcy = 1;

                int col = bDesSackPos % 6;
                int row = bDesSackPos / 6;
                if (col + bcx > 6 || row + bcy > 6) {
                    LOG("[ItemHandler] Item (" + std::to_string(bcx) + "x" + std::to_string(bcy) + ") doesn't fit at grid (" + std::to_string(row) + "," + std::to_string(col) + "), auto-finding...");
                    bDesSackPos = 255; // Trigger auto-find below
                    dwDesObjID = 0;    // Auto-find targets an empty slot, no swap needed
                }

                // Check 2: For multi-cell items, verify no overlap with other items at destination.
                // This catches the case where a 2×2 item is swapped with a 1×1 item but the
                // surrounding cells are occupied by other items.
                if (bDesSackPos != 255 && (bcx > 1 || bcy > 1)) {
                    int startPos = (bDesSackID == 1) ? 20 : (bDesSackID == 2) ? 60 : 100;
                    int endPos = startPos + 35;
                    bool grid[6][6] = {false};

                    // Build occupancy grid, excluding the two items being swapped
                    std::vector<ItemDB::SackOccupancy> occRows;
                    ItemDB::GetInstance().GetSackOccupancy(charID, startPos, endPos, occRows);
                    for (auto& r : occRows) {
                        // Skip the two items involved in the swap
                        if (r.dwItemID == dwSrcObjID || r.dwItemID == dwDesObjID) continue;
                        int ox = (r.bSackPos - startPos) % 6;
                        int oy = (r.bSackPos - startPos) / 6;
                        int ocx = 1, ocy = 1;
                        if (g_ItemTemplates.count(r.wRefID)) {
                            ocx = g_ItemTemplates[r.wRefID].bCX;
                            ocy = g_ItemTemplates[r.wRefID].bCY;
                        }
                        if (ocx < 1) ocx = 1; if (ocy < 1) ocy = 1;
                        for (int dy = 0; dy < ocy; dy++)
                            for (int dx = 0; dx < ocx; dx++)
                                if (oy + dy < 6 && ox + dx < 6)
                                    grid[oy + dy][ox + dx] = true;
                    }

                    // Check if source item's footprint fits at the destination position
                    bool fits = true;
                    for (int dy = 0; dy < bcy && fits; dy++)
                        for (int dx = 0; dx < bcx && fits; dx++)
                            if (grid[row + dy][col + dx]) fits = false;

                    if (!fits) {
                        LOG("[ItemHandler] Swap rejected: " + std::to_string(bcx) + "x" + std::to_string(bcy) + " item overlaps other items at (" + std::to_string(row) + "," + std::to_string(col) + ")");
                        sendMoveFailAck();
                        return;
                    }
                }
            }

            if (bDesSackPos == 255 && bDesSackID > 0) {
                BYTE bcx = 1, bcy = 1;
                WORD wRefID = 0;
                { ItemDB::ItemBasicInfo ib; if (ItemDB::GetInstance().GetItemBasicInfo(dwSrcObjID, ib)) wRefID = ib.wRefID; }
                if (g_ItemTemplates.count(wRefID)) {
                    bcx = g_ItemTemplates[wRefID].bCX;
                    bcy = g_ItemTemplates[wRefID].bCY;
                }
                if (bcx < 1) bcx = 1; if (bcy < 1) bcy = 1;

                bool found = false;
                int actualDesSackID = bDesSackID;
                
                // Try up to 3 backpack pages if auto-placing
                for (int attempt = 1; attempt <= 3; attempt++) {
                    int currentSackID = (attempt == 1) ? bDesSackID : ((bDesSackID == 1) ? (attempt == 2 ? 2 : 3) : (bDesSackID == 2 ? (attempt == 2 ? 1 : 3) : (attempt == 2 ? 1 : 2)));
                    if (currentSackID > 3) continue;

                    BYTE result = FindFreeSackPos(charID, currentSackID, bcx, bcy);
                    if (result != 255) {
                        int startPos = (currentSackID == 1) ? 20 : (currentSackID == 2) ? 60 : 100;
                        bDesSackPos = result - startPos;
                        actualDesSackID = currentSackID;
                        found = true;
                        break;
                    }
                }
                
                if (!found) {
                    LOG("[ItemHandler] All Sacks are full for dwItemID=" + std::to_string(dwSrcObjID));
                    return;
                }
                bDesSackID = actualDesSackID;
            }
            
            int realSrc = bSrcSackID == 0 ? bSrcSackPos : (bSrcSackID == 1 ? 20 + bSrcSackPos : (bSrcSackID == 2 ? 60 + bSrcSackPos : 100 + bSrcSackPos));
            int realDes = bDesSackID == 0 ? bDesSackPos : (bDesSackID == 1 ? 20 + bDesSackPos : (bDesSackID == 2 ? 60 + bDesSackPos : 100 + bDesSackPos));
            
            DWORD dbCharID = charID;

            if (bDesSackID == 0) { // Moving to Equip Sack
                WORD wRefID = 0;
                { ItemDB::ItemBasicInfo ib; if (ItemDB::GetInstance().GetItemBasicInfo(dwSrcObjID, ib)) wRefID = ib.wRefID; }

                if (g_ItemTemplates.count(wRefID)) {
                    sItemTemplate* pTpl = &g_ItemTemplates[wRefID];
                    BYTE t = pTpl->bType;
                    bool validPos = false;
                    if (t == 1 && bDesSackPos == 0) validPos = true;
                    else if (t == 3 && bDesSackPos == 1) validPos = true;
                    else if (t == 2 && bDesSackPos == 2) validPos = true;
                    else if (t == 4 && bDesSackPos == 3) validPos = true;
                    else if (t == 6 && bDesSackPos == 5) validPos = true;
                    else if (t == 7 && bDesSackPos == 6) validPos = true;
                    else if (t == 5 && bDesSackPos == 7) validPos = true;
                    else if (t == 9 && bDesSackPos == 8) validPos = true;
                    else if (t == 8 && (bDesSackPos == 4 || bDesSackPos == 9)) validPos = true;
                    else if (t == 36 && bDesSackPos == 10) validPos = true;

                    if (!validPos) {
                        LOG("[ItemHandler] Invalid Equip Slot " + std::to_string(bDesSackPos) + " for Item Type " + std::to_string(t));
                        SendSystemWarningChat(clientSocket, "无法将该物品装备到此位置！");
                        sendMoveFailAck();
                        return;
                    }

                    int pLevel = 0, pCharType = 0, pStr = 0, pDex = 0, pVit = 0, pSus = 0;
                    CharacterDB::CharPower cpEq;
                    if (CharacterDB::GetInstance().GetCharData(dbCharID, cpEq)) {
                        pLevel = cpEq.wLevel; pCharType = cpEq.bCharType;
                        pStr = cpEq.wStr; pDex = cpEq.wDex; pVit = cpEq.wVit; pSus = cpEq.wSus;
                    }

                    // 从 ITEM 表读取物品实例的实际需求值（改造后可能与模板不同）
                    int reqLv = pTpl->nBasicData1, reqDex = pTpl->nBasicData2;
                    int reqStr = pTpl->nBasicData3, reqSus = pTpl->nBasicData4, reqVit = pTpl->nBasicData5;
                    BYTE reqCharType = pTpl->bCharType;
                    ItemDB::FullItemRow itemRow;
                    if (ItemDB::GetInstance().GetFullItemData(dwSrcObjID, itemRow)) {
                        // ITEM 表有实例数据时，优先使用实例的需求值
                        reqLv = itemRow.nBasicData1;
                        reqDex = itemRow.nBasicData2;
                        reqStr = itemRow.nBasicData3;
                        reqSus = itemRow.nBasicData4;
                        reqVit = itemRow.nBasicData5;
                    }

                    if (pLevel < reqLv || 
                        (reqCharType != 0 && pCharType != reqCharType) ||
                        pDex < reqDex || pStr < reqStr ||
                        pSus < reqSus || pVit < reqVit) {
                        LOG("[ItemHandler] Requirements not met for dwItemID=" + std::to_string(dwSrcObjID) +
                            " Lv:" + std::to_string(pLevel) + "/" + std::to_string(reqLv) +
                            " Str:" + std::to_string(pStr) + "/" + std::to_string(reqStr) +
                            " Dex:" + std::to_string(pDex) + "/" + std::to_string(reqDex) +
                            " Vit:" + std::to_string(pVit) + "/" + std::to_string(reqVit) +
                            " Sus:" + std::to_string(pSus) + "/" + std::to_string(reqSus) +
                            " CharType:" + std::to_string(pCharType) + "/" + std::to_string(reqCharType));

                        // 细分失败原因，给玩家明确的中文提示
                        if (reqCharType != 0 && pCharType != reqCharType) {
                            SendSystemWarningChat(clientSocket, "职业不符，无法穿戴此装备！");
                        } else if (pLevel < reqLv) {
                            SendSystemWarningChat(clientSocket, "等级不足，需要 " + std::to_string(reqLv) + " 级才能穿戴！");
                        } else {
                            SendSystemWarningChat(clientSocket, "属性不足，无法穿戴此装备！");
                        }
                        sendMoveFailAck();
                        return;
                    }
                }
            }

            // Use dwItemID-based updates to avoid UNIQUE constraint conflicts on (dwCharID, bSackPos)
            LOG("[ItemHandler] DB Swap: realSrc=" + std::to_string(realSrc) + " realDes=" + std::to_string(realDes) + " dwSrcObjID=" + std::to_string(dwSrcObjID) + " dwDesObjID=" + std::to_string(dwDesObjID));
            
            if (dwDesObjID != 0) {
                // Swap: move dest item to temp pos first (using dwItemID), then source to dest, then temp to source
                ItemDB::GetInstance().UpdateSackPos(dwDesObjID, 254);
                ItemDB::GetInstance().UpdateSackPos(dwSrcObjID, (BYTE)realDes);
                ItemDB::GetInstance().UpdateSackPos(dwDesObjID, (BYTE)realSrc);
            } else {
                // Simple move: no item at destination, just update position of source item.
                ItemDB::GetInstance().UpdateSackPos(dwSrcObjID, (BYTE)realDes);
            }
            
            UpdatePlayerStatsAndSend(clientSocket, charID);
        
            std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); 
            ackBuf.push_back(bSrcSackID); ackBuf.push_back(bSrcSackPos);
            ackBuf.push_back(bDesSackID); ackBuf.push_back(bDesSackPos);
            
            PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x420E; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
            EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
        }
    }
}

void OnItemDropReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    // CS_IM_THROW_REQ: bSackID(1) bSackPos(1) dwItemID(4) wPosX(2) wPosY(2) bHeight(1) dwAmount(4)
    if (totalSize < 15) return;
    
    BYTE bSackID = payload[0];
    BYTE bSackPos = payload[1];
    DWORD dwItemID = *(DWORD*)(payload + 2);
    WORD wPosX = *(WORD*)(payload + 6);
    WORD wPosY = *(WORD*)(payload + 8);
    BYTE bHeight = payload[10];
    DWORD dwAmount = *(DWORD*)(payload + 11);
    
    LOG("[ItemHandler] OnItemDropReq: charID=" + std::to_string(charID) + " dwItemID=" + std::to_string(dwItemID) + " pos=(" + std::to_string(wPosX) + "," + std::to_string(wPosY) + ") amount=" + std::to_string(dwAmount));
    
    // Verify item exists and belongs to this character
    WORD wRefID = 0;
    DWORD dbAmount = 0;
    { ItemDB::ItemBasicInfo ib;
      if (ItemDB::GetInstance().GetItemBasicInfo(dwItemID, ib)) { wRefID = ib.wRefID; dbAmount = ib.wAmount; }
    }
    
    if (wRefID == 0) {
        LOG("[ItemHandler] OnItemDropReq: Item not found or not owned! dwItemID=" + std::to_string(dwItemID));
        return;
    }
    
    // Retrieve full item attributes (sockets, rebuild level, stats) BEFORE deleting from DB
    ItemDB::FullItemRow row;
    bool hasFullData = ItemDB::GetInstance().GetFullItemData(dwItemID, row);
    
    // Remove the item from the player's backpack mapping, keeping the ITEM and ITEMDATA rows intact in the database
    ItemDB::GetInstance().RemoveFromSack(charID, dwItemID);
    
    // Send CS_IM_REMOVESACK_ACK (0x4208) to remove from client inventory
    {
        std::vector<BYTE> rmBuf(7);
        PACKET_HEADER* head = (PACKET_HEADER*)rmBuf.data();
        head->id = 0x4208;
        head->payloadSize = 3;
        rmBuf[4] = bSackID;
        rmBuf[5] = bSackPos;
        rmBuf[6] = 0; // reason
        EncryptPacket(rmBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)rmBuf.data(), rmBuf.size(), 0);
    }
    
    // Drop item onto the map using DropManager
    // ownerID = 0: anyone can pick up immediately (player-discarded items have no exclusive period)
    DWORD mapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    
    // Build a temporary MonsterData for DropItemToMap position
    MonsterData fakeObj;
    fakeObj.dwMapID = mapID;
    fakeObj.wPosX = wPosX;
    fakeObj.wPosY = wPosY;
    
    if (hasFullData) {
        DropManager::GetInstance()->DropCustomItemToMap(0, fakeObj, row, false);
    } else {
        DropManager::GetInstance()->DropItemToMap(0, fakeObj, wRefID, false);
    }
    
    LOG("[ItemHandler] Item thrown on ground! RefID=" + std::to_string(wRefID));
}

void OnUseItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 6) {
        LOG("[ItemHandler] OnUseItemReq: payload too small: " + std::to_string(totalSize));
        return;
    }
    BYTE bSackID = payload[0];
    BYTE bSackPos = payload[1];
    DWORD dwItemID = *(DWORD*)(payload + 2);

    LOG("[ItemHandler] OnUseItemReq called! charID: " + std::to_string(charID) + " dwItemID: " + std::to_string(dwItemID));

    // 称号系统：UI 实例 ID 50 (向聊天框发送当前激活称号信息)
    if (dwItemID == 50) {
        TitleManager::HandleQueryTitle(clientSocket, charID);
        return;
    }

    // 称号系统：UI 实例 ID 51 (顺序切换当前显示的称号)
    if (dwItemID == 51) {
        TitleManager::HandleSwitchTitle(clientSocket, charID);
        return;
    }

    DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (g_MapInstances.count(pMapID)) {
        std::lock_guard<std::mutex> lock(g_MapInstances[pMapID]->GetMutex());
        PlayerData* pObj = g_MapInstances[pMapID]->GetPlayer(charID + 400000000);
        if (pObj && pObj->bShopStatus == 1) {
            SendSystemWarningChat(clientSocket, "[Shop] Setup is active. Item usage is blocked!");
            return;
        }
    }

    auto pushDWord = [&](std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); };
    auto pushWord = [&](std::vector<BYTE>& buf, WORD w) { buf.push_back(w&0xFF); buf.push_back(w>>8); };

    WORD wRefID = 0;
    WORD wAmount = 0;
    bool foundItem = false;

    // 存储过程开箱子临时产出锁定变量
    WORD openRefID = 0;
    BYTE boxItemCount = 0;
    BYTE boxResult = 0;

    { ItemDB::ItemBasicInfo ib;
      if (ItemDB::GetInstance().GetItemBasicInfo(dwItemID, ib)) {
          wRefID = ib.wRefID; wAmount = (WORD)ib.wAmount; foundItem = true;
      }
    }

    if (!foundItem) {
        LOG("[ItemHandler] OnUseItemReq: Item not found in DB! dwItemID: " + std::to_string(dwItemID));
        return;
    }
    if (g_ItemTemplates.find(wRefID) == g_ItemTemplates.end()) {
        LOG("[ItemHandler] OnUseItemReq: Template not found for wRefID: " + std::to_string(wRefID));
        return;
    }

    sItemTemplate& tpl = g_ItemTemplates[wRefID];
    LOG("[ItemHandler] Using item wRefID: " + std::to_string(wRefID) + " bType: " + std::to_string(tpl.bType));

    // 号角物品（bType=32, nBasicData2 ∈ {2,3,4,8}）不应由 UseItem 处理：
    // 客户端本地弹出消息输入框后走 ChatReq 路径，由 ChatHandler 扣耐久。
    // 如果服务端仍收到 UseItem 请求，说明物品序列化异常，静默拒绝即可。
    if (tpl.bType == 32) {
        int funcID = tpl.nBasicData2;
        if (funcID == 2 || funcID == 3 || funcID == 4 || funcID == 8) {
            LOG("[ItemHandler] 号角物品不由 UseItem 处理，忽略: funcID=" + std::to_string(funcID));
            return;
        }
    }

    // Consumable types: 12 (Potion), 21 (Skill Book), 16 (Teleport), etc.
    if (tpl.bType == 21 || tpl.bType == 12 || tpl.bType == 16 || tpl.bType == 17 || tpl.bType == 19 || tpl.bType == 22 || tpl.bType == 23 || tpl.bType == 33 || tpl.bType == 32) {
        // Continue with normal consumable logic
    } else {
        LOG("[ItemHandler] OnUseItemReq: Unusable item type: " + std::to_string(tpl.bType));
        return;
    }

    // 0. Pre-checks (Do NOT deduct if fails!)
    if (tpl.bType == 19 && tpl.bKind == 17) {
        std::string itemName = tpl.szName;
        WORD premiumRefID = 0;
        
        // 前置校验：查询该卡片在 ITEM_PREMIUM 中是否有效配置
        std::string qPremium = "SELECT wRefID FROM ITEM_PREMIUM WHERE szName = '" + itemName + "'";
        DBHelper::GetInstance().ExecuteQuery(qPremium, [&](SQLHSTMT hStmt) {
            SQLGetData(hStmt, 1, SQL_C_USHORT, &premiumRefID, 0, NULL);
        });
        
        if (premiumRefID == 0) {
            LOG("[ItemHandler] Premium card mismatch! itemName=" + itemName);
            SendSystemWarningChat(clientSocket, "增益卡激活失败：无法识别该增益卡或其已失效！");
            return;
        }
    }

    if (tpl.bType == 19 && tpl.bKind == 4) {
        // 纯 SQL 数据库随机开箱计算逻辑，取代原有 spBoxItemOpen 存储过程
        std::string query = 
            "DECLARE @MaxRand INT; "
            "SELECT @MaxRand = MAX(wMaxRand) FROM BOX_ITEM_CONFIG WHERE dwBoxRefID = " + std::to_string(tpl.wRefID) + "; "
            "IF @MaxRand IS NOT NULL AND @MaxRand > 0 "
            "BEGIN "
            "    DECLARE @RandVal INT = ABS(CHECKSUM(NewId())) % @MaxRand; "
            "    SELECT TOP 1 dwOpenRefID AS wOpenRefID, wAmount AS bItemCount, 0 AS bResult "
            "    FROM BOX_ITEM_CONFIG "
            "    WHERE dwBoxRefID = " + std::to_string(tpl.wRefID) + " AND @RandVal >= wMinRand AND @RandVal < wMaxRand "
            "    ORDER BY id; "
            "END "
            "ELSE "
            "BEGIN "
            "    SELECT 0 AS wOpenRefID, 0 AS bItemCount, 1 AS bResult; "
            "END";
        
        DBHelper::GetInstance().ExecuteQuery(query, [&](SQLHSTMT hStmt) {
            SQLGetData(hStmt, 1, SQL_C_USHORT, &openRefID, 0, NULL);
            SQLGetData(hStmt, 2, SQL_C_UTINYINT, &boxItemCount, 0, NULL);
            SQLGetData(hStmt, 3, SQL_C_UTINYINT, &boxResult, 0, NULL);
        });

        if (boxResult != 0 || openRefID == 0 || g_ItemTemplates.find(openRefID) == g_ItemTemplates.end()) {
            LOG("[ItemHandler] Box pre-open failed or invalid openRefID! wBoxItemRefID: " + std::to_string(tpl.wRefID) + " openRefID: " + std::to_string(openRefID));
            SendSystemWarningChat(clientSocket, "宝箱开启失败：无法识别该宝箱或其无产出！");
            return;
        }

        sItemTemplate& tplOpen = g_ItemTemplates[openRefID];
        BYTE freePos = FindFreeSackPos(charID, 1, tplOpen.bCX, tplOpen.bCY);
        if (freePos == 255) {
            freePos = FindFreeSackPos(charID, 2, tplOpen.bCX, tplOpen.bCY);
        }
        if (freePos == 255) {
            freePos = FindFreeSackPos(charID, 3, tplOpen.bCX, tplOpen.bCY);
        }

        if (freePos == 255) {
            LOG("[ItemHandler] Box open blocked: Inventory full for charID: " + std::to_string(charID));
            SendSystemWarningChat(clientSocket, "背包空间不足，请先清理背包！");
            return;
        }
    }

    BYTE targetLevel = 1;
    if (tpl.bType == 21) { // Book (Skill)
        int playerLevel = 0, playerType = 0;
        CharacterDB::CharPower cp;
        if (CharacterDB::GetInstance().GetCharData(charID, cp)) {
            playerLevel = cp.wLevel;
            playerType = cp.bCharType;
        }

        // Check career/class restriction (bCharType in item template)
        if (tpl.bCharType != 0 && playerType != tpl.bCharType) {
            LOG("[ItemHandler] Career mismatch for skill book! Player: " + std::to_string(playerType) + " Book Career Req: " + std::to_string(tpl.bCharType));
            SendSystemWarningChat(clientSocket, "职业不符，无法使用此技能书！");
            return;
        }

        // Check level requirement (wLevel in item template)
        if (playerLevel < tpl.wLevel) {
            LOG("[ItemHandler] Level too low for skill book! Player Level: " + std::to_string(playerLevel) + " Book Level Req: " + std::to_string(tpl.wLevel));
            SendSystemWarningChat(clientSocket, "等级不足，无法使用此技能书！");
            return;
        }

        DWORD dwMugongID = tpl.nBasicData2;
        DWORD reqPrevLevel = tpl.nBasicData5;
        
        int currentLevel = MugongManager::GetInstance()->GetPlayerMugongLevel(charID, dwMugongID);
        
        if (reqPrevLevel == 0) {
            if (currentLevel > 0) {
                LOG("[ItemHandler] Already learned this beginner skill! Aborting.");
                return;
            }
            targetLevel = 1;
        } else {
            if (currentLevel < reqPrevLevel) {
                LOG("[ItemHandler] Skill level too low to read advanced book! Current: " + std::to_string(currentLevel) + " Req: " + std::to_string(reqPrevLevel));
                return;
            }
            if (currentLevel > reqPrevLevel) { // wait, if they are already higher, don't downgrade
                LOG("[ItemHandler] Already advanced past this book! Aborting.");
                return;
            }
            targetLevel = reqPrevLevel + 1; // Advance the skill!
        }

        // 五行必杀技学习门槛校验 (150 ~ 154)
        if (dwMugongID >= 150 && dwMugongID <= 154) {
            WORD reqPoints = 0;
            if (targetLevel == 1) reqPoints = 200;
            else if (targetLevel == 2) reqPoints = 400;
            else if (targetLevel == 3) reqPoints = 600;

            WORD currentFiveElmVal = 0;
            // 从 DB 中直接加载最新的玩家五行分配值进行校验
            CharacterDB::ExpData expData;
            if (CharacterDB::GetInstance().GetExpData(charID, expData)) {
                if (dwMugongID == 150) currentFiveElmVal = expData.wFireExp;
                else if (dwMugongID == 151) currentFiveElmVal = expData.wWaterExp;
                else if (dwMugongID == 152) currentFiveElmVal = expData.wWoodExp;
                else if (dwMugongID == 153) currentFiveElmVal = expData.wMetalExp;
                else if (dwMugongID == 154) currentFiveElmVal = expData.wEarthExp;
            }

            if (currentFiveElmVal < reqPoints) {
                LOG("[ItemHandler] 五行加点数值不足以学习该技能书! Current: " + std::to_string(currentFiveElmVal) + " Required: " + std::to_string(reqPoints));
                
                // 向客户端发送读取武功书结果失败包 0x421E (CS_IM_READRESULT_ACK)
                // 协议格式：bResult(1) + szMugongBookName (sString: WORD len + data)
                std::vector<BYTE> ackBuf; ackBuf.reserve(64);
                ackBuf.resize(4); // 占位 header
                ackBuf.push_back(10); // bResult = 10 (IDS_FIVEELEMENT_EXPLACK)
                
                std::string bookName = tpl.szName;
                WORD nameLen = (WORD)bookName.length();
                ackBuf.push_back(nameLen & 0xFF);
                ackBuf.push_back(nameLen >> 8);
                for (char c : bookName) ackBuf.push_back(c);
                
                PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
                head->id = 0x421E; // CS_IM_READRESULT_ACK = OFFSET_CS_IM + 29 = 0x4201 + 29 = 0x421E
                head->payloadSize = ackBuf.size() - 4;
                EncryptPacket(ackBuf.data(), 0x42);
                SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
                return;
            }
        }

        if (!MugongManager::GetInstance()->CanLearnMugong(charID, dwMugongID, targetLevel)) {
            LOG("[ItemHandler] Failed CanLearnMugong pre-check, item deduction aborted.");
            return;
        }
    }

    // 1. Amount Deduction
    if (wAmount > 1) {
        ItemDB::GetInstance().DecrementItemAmount(dwItemID);

        std::vector<BYTE> ackBuf(4);
        ackBuf.push_back(bSackID);
        ackBuf.push_back(bSackPos);
        pushDWord(ackBuf, dwItemID);
        pushWord(ackBuf, wAmount - 1);
        PACKET_HEADER* ah = (PACKET_HEADER*)ackBuf.data();
        ah->id = 0x4216; // CS_IM_CHANGERES_ACK
        ah->payloadSize = ackBuf.size() - 4;
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (char*)ackBuf.data(), ackBuf.size(), 0);
    } else {
        // Delete order: SACKITEM -> ITEMDATA -> ITEM (FK constraint)
        std::string delSack = "DELETE FROM SACKITEM WHERE dwItemID = " + std::to_string(dwItemID);
        std::string delData = "DELETE FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwItemID);
        std::string delItem = "DELETE FROM ITEM WHERE dwItemID = " + std::to_string(dwItemID);
        ItemDB::GetInstance().RemoveFromSack(charID, dwItemID);
        ItemDB::GetInstance().DeleteItemData(dwItemID);
        ItemDB::GetInstance().DeleteItem(dwItemID);

        std::vector<BYTE> ackBuf(4);
        ackBuf.push_back(bSackID);
        ackBuf.push_back(bSackPos);
        pushDWord(ackBuf, dwItemID);
        ackBuf.push_back(0); // reason
        PACKET_HEADER* ah = (PACKET_HEADER*)ackBuf.data();
        ah->id = 0x4208; // CS_IM_REMOVESACK_ACK
        ah->payloadSize = ackBuf.size() - 4;
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (char*)ackBuf.data(), ackBuf.size(), 0);
    }

    // 2. Apply Effect
    if (tpl.bType == 19 && tpl.bKind == 4) {
        sItemTemplate& tplOpen = g_ItemTemplates[openRefID];
        BYTE actualSackID = 1;
        BYTE freePos = FindFreeSackPos(charID, 1, tplOpen.bCX, tplOpen.bCY);
        if (freePos == 255) {
            freePos = FindFreeSackPos(charID, 2, tplOpen.bCX, tplOpen.bCY);
            actualSackID = 2;
        }
        if (freePos == 255) {
            freePos = FindFreeSackPos(charID, 3, tplOpen.bCX, tplOpen.bCY);
            actualSackID = 3;
        }

        if (freePos != 255) {
            DWORD newDbItemID = ItemDB::GetInstance().CreateItemFromTemplate(openRefID);
            if (newDbItemID == 0) {
                newDbItemID = rand() * rand();
            } else {
                if (tplOpen.bType < 10) {
                    int nData[25] = {0};
                    nData[0] = tplOpen.nData1;
                    nData[1] = tplOpen.nData2;
                    nData[2] = tplOpen.nData3;
                    nData[3] = tplOpen.nData4;
                    nData[4] = tplOpen.nData5;
                    nData[5] = tplOpen.nData6;
                    nData[6] = tplOpen.nData7;
                    nData[7] = tplOpen.nData8;
                    nData[8] = tplOpen.nData9;
                    nData[9] = tplOpen.nData10;
                    nData[12] = tplOpen.nData13;
                    ItemDB::GetInstance().InsertItemData(newDbItemID, nData);
                }
                if (boxItemCount > 1) {
                    ItemDB::GetInstance().UpdateItemAmount(newDbItemID, boxItemCount);
                }
            }

            ItemDB::GetInstance().AddToSack(charID, freePos, newDbItemID);

            int startPos = (actualSackID == 1) ? 20 : (actualSackID == 2) ? 60 : 100;
            BYTE relativeSackPos = freePos - startPos;

            std::vector<BYTE> bi;
            bi.resize(4);
            auto pushByte = [&](BYTE v) { bi.push_back(v); };
            auto pushWord = [&](WORD v) { bi.push_back(v & 0xFF); bi.push_back(v >> 8); };
            auto pushDWord = [&](DWORD v) { bi.push_back(v & 0xFF); bi.push_back((v >> 8) & 0xFF); bi.push_back((v >> 16) & 0xFF); bi.push_back(v >> 24); };
            
            pushDWord(newDbItemID);
            pushWord(tplOpen.wRefID);
            pushByte(tplOpen.bType);
            pushByte(tplOpen.bKind);
            pushWord(tplOpen.wVisualID);
            std::string openItemName = tplOpen.szName;
            pushWord((WORD)openItemName.length());
            for (char c : openItemName) pushByte(c);
            pushDWord(tplOpen.dwCost);
            pushWord(tplOpen.wLevel);
            pushByte(tplOpen.bCharType);
            pushWord((WORD)boxItemCount);

            if (tplOpen.bType >= 1 && tplOpen.bType <= 9) {
                pushWord((WORD)tplOpen.nBasicData1);
                pushWord((WORD)tplOpen.nBasicData2);
                pushWord((WORD)tplOpen.nBasicData3);
                pushWord((WORD)tplOpen.nBasicData4);
                pushWord((WORD)tplOpen.nBasicData5);
                pushByte((BYTE)tplOpen.nData1);
                pushWord((WORD)tplOpen.nData2);
                pushWord((WORD)tplOpen.nData3);
                pushDWord(tplOpen.nData4);
                pushDWord(tplOpen.nData5);
                pushDWord(tplOpen.nData6);
                pushWord((WORD)tplOpen.nData7);
                pushWord((WORD)tplOpen.nData8);
                pushDWord(tplOpen.nData9);
                pushDWord(tplOpen.nData10);
                pushWord(0); pushWord(0);
                pushWord((WORD)tplOpen.nData13);
                pushByte(0); pushByte(0); pushByte(0); pushByte(0); pushByte(0); pushByte(0);
                if (tplOpen.bType == 9) {
                    pushDWord(0); pushWord(0); pushWord(0); pushWord(0); pushWord(0);
                } else if (tplOpen.bType == 8) {
                    for (int k = 0; k < 8; k++) pushByte(0);
                } else {
                    pushByte(0);
                }
                if (tplOpen.bType >= 1 && tplOpen.bType <= 4) {
                    pushByte(0); pushByte(0); pushByte(0); pushWord(0);
                }
            } else {
                switch (tplOpen.bType) {
                    case 11: case 12: case 13: case 14: case 17:
                        pushByte(0); pushWord((WORD)tplOpen.nData2); pushWord((WORD)tplOpen.nData3); break;
                    case 15:
                        pushByte(0); pushWord((WORD)tplOpen.nBasicData1); pushWord((WORD)tplOpen.nBasicData1); pushByte(0); pushByte(0); break;
                    case 16:
                        pushWord((WORD)tplOpen.nData1); pushByte((BYTE)tplOpen.nData2); pushWord((WORD)tplOpen.nData3); pushByte((BYTE)tplOpen.nData4); pushDWord((DWORD)tplOpen.nData5); break;
                    case 18:
                        pushByte(0); break;
                    case 19:
                        pushWord(0); pushWord(0); break;
                    case 20:
                        pushByte(0); pushDWord(0); break;
                    case 21: {
                        DWORD mid = tplOpen.nBasicData2;
                        sMugongTemplate* mg = MugongManager::GetInstance()->GetTemplate(mid);
                        pushWord(tplOpen.wLevel); pushDWord(mid); pushByte(mg ? mg->bType : 0); pushByte(mg ? mg->bKind : 0); pushByte(1); break;
                    }
                    case 22:
                        pushDWord((DWORD)tplOpen.nBasicData2); pushByte(0); pushWord(0); pushWord(0); break;
                    case 23:
                        pushDWord(0); pushDWord((DWORD)tplOpen.nBasicData2); pushDWord((DWORD)tplOpen.nBasicData3); pushByte(0); pushByte(0); break;
                    case 25:
                        pushByte(0); pushWord((WORD)tplOpen.nBasicData2); pushWord((WORD)tplOpen.nBasicData3); break;
                    case 32:
                        pushWord((WORD)tplOpen.nData1); pushWord((WORD)tplOpen.nBasicData1); pushWord((WORD)tplOpen.nBasicData1); pushDWord(tplOpen.nData3); break;
                }
            }
            pushWord(0);
            
            PACKET_HEADER* addHead = (PACKET_HEADER*)bi.data();
            addHead->id = 0x420A;
            addHead->payloadSize = (WORD)(bi.size() - 4);
            EncryptPacket(bi.data(), 0x42);
            SafeSend(clientSocket, (const char*)bi.data(), bi.size(), 0);

            std::string announceName = tplOpen.szName;
            SystemMessage::SendHelpMessage(clientSocket, SystemMessage::MsgType::PICK_ITEM, announceName, boxItemCount);
            LOG("[ItemHandler] Box open success! charID=" + std::to_string(charID) + " BoxRefID=" + std::to_string(tpl.wRefID) + " OpenedRefID=" + std::to_string(openRefID));
        }
    } else if (tpl.bType == 19 && tpl.bKind == 17) {
        std::string itemName = tpl.szName;
        WORD premiumRefID = 0;
        BYTE premiumType = 0;
        BYTE premiumKind = 0;
        WORD premiumValue = 0;
        BYTE premiumDay = 0;
        
        // 1. 查询 ITEM_PREMIUM 取得效果配置
        std::string qPremium = "SELECT wRefID, bType, bKind, wValue, bDay FROM ITEM_PREMIUM WHERE szName = '" + itemName + "'";
        DBHelper::GetInstance().ExecuteQuery(qPremium, [&](SQLHSTMT hStmt) {
            SQLGetData(hStmt, 1, SQL_C_USHORT, &premiumRefID, 0, NULL);
            SQLGetData(hStmt, 2, SQL_C_UTINYINT, &premiumType, 0, NULL);
            SQLGetData(hStmt, 3, SQL_C_UTINYINT, &premiumKind, 0, NULL);
            SQLGetData(hStmt, 4, SQL_C_USHORT, &premiumValue, 0, NULL);
            SQLGetData(hStmt, 5, SQL_C_UTINYINT, &premiumDay, 0, NULL);
        });
        
        if (premiumRefID > 0) {
            // 2. 查出角色的账号 szAccount
            char szAccount[32] = {0};
            std::string qAcc = "SELECT szAccount FROM CHARACTER WHERE dwCharID = " + std::to_string(charID);
            DBHelper::GetInstance().ExecuteQuery(qAcc, [&](SQLHSTMT hStmt) {
                SQLGetData(hStmt, 1, SQL_C_CHAR, szAccount, sizeof(szAccount), NULL);
            });
            
            // 3. 计算时间增量
            int addMinutes = 0;
            if (premiumRefID == 1 || premiumRefID == 2 || premiumRefID == 3 || premiumRefID == 5 || 
                premiumRefID == 8 || premiumRefID == 21 || premiumRefID == 22 || 
                premiumRefID == 22500 || premiumRefID == 22501 || premiumRefID == 22502 || 
                premiumRefID == 22503 || premiumRefID == 22504) {
                addMinutes = premiumDay * 60; // 小时换算
            } else {
                addMinutes = premiumDay * 1440; // 天数换算
            }
            
            // 4. 时效叠加防刷更新 SQL：如果已有同类有效卡，直接延长 dateEnd，否则以当前 GETDATE() 起始写入
            std::string qCheck = "SELECT COUNT(*) FROM CHAR_PREMIUM WHERE dwCharID = " + std::to_string(charID) + " AND wRefID = " + std::to_string(premiumRefID) + " AND dateEnd > GETDATE()";
            int hasActive = 0;
            DBHelper::GetInstance().ExecuteQuery(qCheck, [&](SQLHSTMT hStmt) {
                SQLGetData(hStmt, 1, SQL_C_LONG, &hasActive, 0, NULL);
            });
            
            std::string qWrite = "";
            if (hasActive > 0) {
                qWrite = "UPDATE CHAR_PREMIUM SET dateEnd = DATEADD(minute, " + std::to_string(addMinutes) + ", dateEnd) WHERE dwCharID = " + std::to_string(charID) + " AND wRefID = " + std::to_string(premiumRefID) + " AND dateEnd > GETDATE()";
            } else {
                qWrite = "INSERT INTO CHAR_PREMIUM (szAccount, dwCharID, wRefID, bType, bKind, dateStart, dateEnd, dwDupID) VALUES ('" 
                        + std::string(szAccount) + "', " + std::to_string(charID) + ", " + std::to_string(premiumRefID) + ", " 
                        + std::to_string(premiumType) + ", " + std::to_string(premiumKind) + ", GETDATE(), DATEADD(minute, " 
                        + std::to_string(addMinutes) + ", GETDATE()), 0)";
            }
            
            DBHelper::GetInstance().ExecuteUpdate(qWrite);
            
            // 5. 瞬间同步刷新客户端 Buff 栏与倒计时提示！
            SendCharPremiumList(clientSocket, charID);
            
            // 6. 系统文字提示（物品使用成功）
            SystemMessage::SendHelpMessage(clientSocket, SystemMessage::MsgType::PICK_ITEM, itemName, 1);
            LOG("[PremiumBuff] Buff activated for charID=" + std::to_string(charID) + " RefID=" + std::to_string(premiumRefID) + " Minutes=" + std::to_string(addMinutes));
        }
    } else if (tpl.bType == 22) { // Scroll
        DWORD destMap = tpl.nBasicData2;
        WORD startX = 1024, startY = 1024;
        bool useInstanceCoords = false;

        // 如果是特殊记录传送符，优先从该物品的具体实例中读取已经记录的目标地图及坐标X/Y
        ItemDB::FullItemRow instRow;
        if (ItemDB::GetInstance().GetFullItemData(dwItemID, instRow)) {
            if (instRow.nBasicData2 > 0 && instRow.nBasicData4 > 0 && instRow.nBasicData5 > 0) {
                destMap = instRow.nBasicData2;
                startX = (WORD)instRow.nBasicData4;
                startY = (WORD)instRow.nBasicData5;
                useInstanceCoords = true;
                LOG("[ItemHandler] Teleporting using scroll saved coords: destMap=" + std::to_string(destMap) + " pos=(" + std::to_string(startX) + "," + std::to_string(startY) + ")");
            }
        }

        if (!useInstanceCoords) {
            // 修复回城符（destMap == 0）：读取当前所在地图的安全坐标，进行返回！
            if (destMap == 0) {
                destMap = SessionMgr::GetInstance().GetMapID(clientSocket);
            }
            int spX = -1, spY = -1;
            CharacterDB::GetInstance().GetSpawnPosition(destMap, spX, spY);

            // 熔断兜底防御：如果在 location 表中没有记录此地图坐标，则直接退回到 map 6 (草原地带新手村) 的出生点坐标
            if (spX == -1 || spY == -1) {
                destMap = 6;
                CharacterDB::GetInstance().GetSpawnPosition(destMap, spX, spY);
            }

            startX = (WORD)spX; startY = (WORD)spY;
            LOG("[ItemHandler] Teleporting using map location: destMap=" + std::to_string(destMap) + " pos=(" + std::to_string(startX) + "," + std::to_string(startY) + ")");
        }

        // 坐标边界安全校验：遁身符目标坐标不得超出目标地图的 wWidth/wHeight
        {
            GameDataDB::MapInfo destBounds;
            if (GameDataDB::GetInstance().GetMapInfo(destMap, destBounds)) {
                if (startX >= destBounds.wWidth || startY >= destBounds.wHeight) {
                    LOG("[ItemHandler] WARNING: Scroll dest (" + std::to_string(startX) + "," + std::to_string(startY) + ") OUT OF BOUNDS for MapID " + std::to_string(destMap) + " (size " + std::to_string(destBounds.wWidth) + "x" + std::to_string(destBounds.wHeight) + "). Resetting to spawn!");
                    int spX2 = 0, spY2 = 0;
                    CharacterDB::GetInstance().GetSpawnPosition(destMap, spX2, spY2);
                    if (spX2 > 0 && spY2 > 0) { startX = (WORD)spX2; startY = (WORD)spY2; }
                    else { startX = destBounds.wWidth / 2; startY = destBounds.wHeight / 2; }
                }
            }
        }

        CharacterDB::GetInstance().SavePosition(charID, startX, startY, destMap);

        DWORD oldMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        PlayerData objToMove;
        bool found = false;
        if (g_MapInstances.count(oldMapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[oldMapID]->GetMutex());
            PlayerData* pObj = g_MapInstances[oldMapID]->GetPlayer(charID + 400000000);
            if (pObj) {
                objToMove = *pObj;
                found = true;
                g_MapInstances[oldMapID]->RemovePlayer(charID + 400000000);
            }
        }
        if (found) {
            objToMove.dwMapID = destMap;
            objToMove.wPosX = startX;
            objToMove.wPosY = startY;
            if (g_MapInstances.count(destMap)) {
                std::lock_guard<std::mutex> lock(g_MapInstances[destMap]->GetMutex());
                g_MapInstances[destMap]->AddPlayer(objToMove);
            }
        }
        SessionMgr::GetInstance().SetMapID(clientSocket, destMap);

        std::vector<BYTE> mvBuf(4);
        mvBuf.push_back(0); // bResult = success
        pushDWord(mvBuf, destMap);
        pushWord(mvBuf, startX);
        pushWord(mvBuf, startY);
        mvBuf.push_back(0); // bLinkMapType

        PACKET_HEADER* mvHead = (PACKET_HEADER*)mvBuf.data();
        mvHead->id = 0x4308; // CS_NV_MAPMOVE_ACK
        mvHead->payloadSize = mvBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(mvBuf.data(), 0x42);
        SafeSend(clientSocket, (char*)mvBuf.data(), mvBuf.size(), 0);

        LOG("[ItemHandler] Teleported char " + std::to_string(charID) + " to Map " + std::to_string(destMap));
    } else if (tpl.bType == 23) { // Potion
        DWORD incrHp = tpl.nBasicData2;  // HP recovery from nBasicData2
        DWORD incrIp = tpl.nBasicData3;  // MP/IP recovery from nBasicData3
        DWORD curHp = 0, maxHp = 0;
        DWORD curIp = 0, maxIp = 0;

        DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        if (g_MapInstances.count(pMapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[pMapID]->GetMutex());
            PlayerData* pObj = g_MapInstances[pMapID]->GetPlayer(charID + 400000000);
            if (pObj) {
                pObj->dwHpCur += incrHp;
                if (pObj->dwHpCur > pObj->dwHpMax) pObj->dwHpCur = pObj->dwHpMax;
                pObj->wIpCur += incrIp;
                if (pObj->wIpCur > pObj->wIpMax) pObj->wIpCur = pObj->wIpMax;

                curHp = pObj->dwHpCur; maxHp = pObj->dwHpMax;
                curIp = pObj->wIpCur; maxIp = pObj->wIpMax;
            }
        }

        CharacterDB::GetInstance().UpdateHpIp(charID, curHp, curIp);

        std::vector<BYTE> hpBuf(4);
        pushDWord(hpBuf, maxHp);
        pushDWord(hpBuf, curHp);
        pushDWord(hpBuf, maxIp);
        pushDWord(hpBuf, curIp);
        hpBuf.push_back(0); // bType
        PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
        hpHead->id = 0x3B0D; // CS_IF_CHARHP_ACK
        hpHead->payloadSize = hpBuf.size() - 4;
        EncryptPacket(hpBuf.data(), 0x42);
        SafeSend(clientSocket, (char*)hpBuf.data(), hpBuf.size(), 0);

        LOG("[ItemHandler] Restored HP/MP for char " + std::to_string(charID));
    } else if (tpl.bType == 21) { // Book (Skill)
        DWORD dwMugongID = tpl.nBasicData2;
        MugongManager::GetInstance()->LearnMugong(clientSocket, charID, dwMugongID, targetLevel);
    } else if (tpl.bType == 32) { // Shop Token / GISDURABLITY 功能令牌
        if (tpl.nBasicData2 == 1) {
            // 远程仓库账簿：直接拉起随身个人仓库 UI 
            OnItemListInBankReq(clientSocket, charID, nullptr, 0);
        } else {
            // 默认摆摊店铺令牌
            ShopHandler::OnOpenShop(clientSocket, charID);
        }
    }
}

void OnPetBongInReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 6 || charID == 0) return;

    DWORD dwPetObjectID = *(DWORD*)(payload);
    BYTE bSackID = payload[4];
    BYTE bSackPos = payload[5];

    LOG("[PetBongIn] Request from charID=" + std::to_string(charID) + " for PetObjectID=" + std::to_string(dwPetObjectID) + " Sack=" + std::to_string(bSackID) + ":" + std::to_string(bSackPos));

    if (dwPetObjectID < 800000000) {
        LOG("[PetBongIn] Invalid PetObjectID: " + std::to_string(dwPetObjectID));
        return;
    }
    DWORD dwPetID = dwPetObjectID - 800000000;

    auto sendAck = [&](BYTE result) {
        std::vector<BYTE> ackBuf(9);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x3548; // CS_NC_PETBONGIN_ACK
        head->payloadSize = 5;
        ackBuf[4] = result;
        memcpy(ackBuf.data() + 5, &dwPetObjectID, 4);
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), (int)ackBuf.size(), 0);
    };

    // 1. 行囊防空安全校验 (查询 PETITEM 是否仍有物品)
    bool hasItem = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT TOP 1 1 FROM PETITEM WHERE dwPetID = " + std::to_string(dwPetID),
        [&](SQLHSTMT) { hasItem = true; });

    if (hasItem) {
        LOG("[PetBongIn] Seal blocked: Pet has items in inventory! dwPetID=" + std::to_string(dwPetID));
        sendAck(3); // ERR_PETBONGIN_HASITEM
        return;
    }

    // 2. 验证宠物归属
    bool petExists = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT TOP 1 1 FROM CHAR_PET WHERE dwID = " + std::to_string(dwPetID) + " AND dwCharID = " + std::to_string(charID),
        [&](SQLHSTMT) { petExists = true; });

    if (!petExists) {
        LOG("[PetBongIn] Seal blocked: Pet not found or not owned by charID=" + std::to_string(charID));
        sendAck(1); // ERR_PETBONGIN_NOTFINDPET
        return;
    }

    // 3. 校验并获取行囊中的龙魂镜
    int realPos = bSackID == 0 ? bSackPos : (bSackID == 1 ? 20 + bSackPos : (bSackID == 2 ? 60 + bSackPos : 100 + bSackPos));
    DWORD dwItemID = 0;
    WORD wRefID = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT S.dwItemID, I.wRefID FROM SACKITEM S INNER JOIN ITEM I ON S.dwItemID = I.dwItemID WHERE S.dwCharID = " + std::to_string(charID) + " AND S.bSackPos = " + std::to_string(realPos),
        [&](SQLHSTMT hStmt) {
            SQLLEN c1, c2;
            SQLGetData(hStmt, 1, SQL_C_ULONG, &dwItemID, 0, &c1);
            SQLGetData(hStmt, 2, SQL_C_USHORT, &wRefID, 0, &c2);
        });

    if (dwItemID == 0 || g_ItemTemplates.count(wRefID) == 0 || g_ItemTemplates[wRefID].bType != 9) {
        LOG("[PetBongIn] Seal blocked: No valid bongin mirror at target sack slot! realPos=" + std::to_string(realPos));
        sendAck(5); // ERR_PETBONGIN_INVALIDITEM
        return;
    }

    // 4. 数据解绑挂起并绑定封印
    // A. 移除地图上的宠物实体并广播离开包 0x3503
    DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (g_MapInstances.count(pMapID)) {
        CMapInstance* mapInst = g_MapInstances[pMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        if (mapInst->GetPlayer(dwPetObjectID)) {
            mapInst->RemovePlayer(dwPetObjectID);

            std::vector<BYTE> leaveBuf(4);
            auto pushDWord = [&](DWORD d) { leaveBuf.push_back(d&0xFF); leaveBuf.push_back((d>>8)&0xFF); leaveBuf.push_back((d>>16)&0xFF); leaveBuf.push_back(d>>24); };
            
            pushDWord(dwPetObjectID);
            leaveBuf.push_back(4); // bObjectType = 4

            PACKET_HEADER* leaveHead = (PACKET_HEADER*)leaveBuf.data();
            leaveHead->id = 0x3503;
            leaveHead->payloadSize = (WORD)(leaveBuf.size() - sizeof(PACKET_HEADER));
            EncryptPacket(leaveBuf.data(), 0x42);
            BroadcastPacketToMap(pMapID, leaveBuf);

            LOG("[PetBongIn] Despawned pet ObjID=" + std::to_string(dwPetObjectID) + " on map " + std::to_string(pMapID));
        }
    }

    // B. 更改宠物所有者为 0 (代表挂起)
    DBHelper::GetInstance().ExecuteUpdate("UPDATE CHAR_PET SET dwCharID = 0 WHERE dwID = " + std::to_string(dwPetID));

    // B. 更新镜子数据 ITEMDATA.nData1 = dwPetID, nData15 = 1 (m_bModifyCnt 封印数)
    bool dataExists = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT 1 FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwItemID),
        [&](SQLHSTMT) { dataExists = true; });

    if (dataExists) {
        DBHelper::GetInstance().ExecuteUpdate(
            "UPDATE ITEMDATA SET nData1 = " + std::to_string(dwPetID) + ", nData15 = 1 WHERE dwItemID = " + std::to_string(dwItemID));
    } else {
        std::string qInsert = "INSERT INTO ITEMDATA (dwItemID, nData1, nData2, nData3, nData4, nData5, nData6, nData7, nData8, nData9, nData10, nData11, nData12, nData13, nData14, nData15, nData16, nData17, nData18, nData19, nData20) VALUES ("
            + std::to_string(dwItemID) + ", " + std::to_string(dwPetID) + ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0)";
        DBHelper::GetInstance().ExecuteUpdate(qInsert);
    }

    LOG("[PetBongIn] Pet sealed successfully! dwPetID=" + std::to_string(dwPetID) + " bound to ItemID=" + std::to_string(dwItemID));

    // 5. 广播成功响应，并强制拉起包裹刷新
    sendAck(0); // ERR_PETBONGIN_SUCCESS

    BYTE reqPayload[1] = { bSackID };
    OnItemListReq(clientSocket, charID, reqPayload, 1);
}

void OnPetBongOutReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 2 || charID == 0) return;

    BYTE bSackID = payload[0];
    BYTE bSackPos = payload[1];

    LOG("[PetBongOut] Request from charID=" + std::to_string(charID) + " Sack=" + std::to_string(bSackID) + ":" + std::to_string(bSackPos));

    auto sendFailAck = [&](BYTE result) {
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x354A; // CS_NC_PETBONGOUT_ACK
        head->payloadSize = 1;
        ackBuf[4] = result;
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), (int)ackBuf.size(), 0);
    };

    // 1. 获取包裹中龙魂镜并校验
    int realPos = bSackID == 0 ? bSackPos : (bSackID == 1 ? 20 + bSackPos : (bSackID == 2 ? 60 + bSackPos : 100 + bSackPos));
    DWORD dwItemID = 0;
    WORD wRefID = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT S.dwItemID, I.wRefID FROM SACKITEM S INNER JOIN ITEM I ON S.dwItemID = I.dwItemID WHERE S.dwCharID = " + std::to_string(charID) + " AND S.bSackPos = " + std::to_string(realPos),
        [&](SQLHSTMT hStmt) {
            SQLLEN c1, c2;
            SQLGetData(hStmt, 1, SQL_C_ULONG, &dwItemID, 0, &c1);
            SQLGetData(hStmt, 2, SQL_C_USHORT, &wRefID, 0, &c2);
        });

    if (dwItemID == 0 || g_ItemTemplates.count(wRefID) == 0 || g_ItemTemplates[wRefID].bType != 9) {
        LOG("[PetBongOut] Release blocked: No valid bongin mirror at target slot! realPos=" + std::to_string(realPos));
        sendFailAck(2); // ERR_PETBONGIN_INVALIDITEM (复用失败码)
        return;
    }

    // 2. 读取镜子所绑定的 dwPetID
    DWORD dwPetID = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT nData1 FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwItemID),
        [&](SQLHSTMT hStmt) {
            SQLLEN c;
            SQLGetData(hStmt, 1, SQL_C_ULONG, &dwPetID, 0, &c);
        });

    if (dwPetID == 0) {
        LOG("[PetBongOut] Release blocked: Mirror is empty! ItemID=" + std::to_string(dwItemID));
        sendFailAck(2); // 镜像为空无法释放
        return;
    }

    // 3. 从 CHAR_PET 中查询宠物全部 30 多项属性
    struct DbPetRow {
        DWORD dwID = 0;
        DWORD dwMapID = 0;
        BYTE bNpcType = 0;
        char szName[32] = {0};
        WORD wLevel = 0;
        WORD wPosX = 0;
        WORD wPosY = 0;
        BYTE bHeight = 0;
        DWORD dwHpMax = 0;
        DWORD dwHpCur = 0;
        WORD wAtkPwr = 0;
        WORD wDefPwr = 0;
        WORD wAtkRating = 0;
        WORD wAvoidRatio = 0;
        BYTE bSpeed = 0;
        WORD wMeleeAtkRange = 0;
        WORD wShotAtkRange = 0;
        BYTE bAtkType = 0;
        DWORD dwRefNpcID = 0;
        BYTE bCurJob = 0;
        long long biExp = 0;
        BYTE bRevolutionStep = 0;
        BYTE bWildRate = 0;
    } pet;

    bool found = false;
    std::string qPet = "SELECT dwID, dwMapID, bNpcType, szName, wLevel, wPosX, wPosY, bHeight, dwHpMax, dwHpCur, wAtkPwr, wDefPwr, wAtkRating, wAvoidRatio, bSpeed, wMeleeAtkRange, wShotAtkRange, bAtkType, dwRefNpcID, bCurJob, biExp, bRevolutionStep, bWildRate FROM CHAR_PET WHERE dwID = " + std::to_string(dwPetID);
    DBHelper::GetInstance().ExecuteQuery(qPet, [&](SQLHSTMT hStmt) {
        found = true;
        SQLLEN c[23];
        int idx = 1;
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &pet.dwID, 0, &c[0]);
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &pet.dwMapID, 0, &c[1]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bNpcType, 0, &c[2]);
        SQLGetData(hStmt, idx++, SQL_C_CHAR, pet.szName, sizeof(pet.szName), &c[3]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wLevel, 0, &c[4]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wPosX, 0, &c[5]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wPosY, 0, &c[6]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bHeight, 0, &c[7]);
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &pet.dwHpMax, 0, &c[8]);
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &pet.dwHpCur, 0, &c[9]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wAtkPwr, 0, &c[10]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wDefPwr, 0, &c[11]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wAtkRating, 0, &c[12]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wAvoidRatio, 0, &c[13]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bSpeed, 0, &c[14]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wMeleeAtkRange, 0, &c[15]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wShotAtkRange, 0, &c[16]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bAtkType, 0, &c[17]);
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &pet.dwRefNpcID, 0, &c[18]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bCurJob, 0, &c[19]);
        SQLGetData(hStmt, idx++, SQL_C_SBIGINT, &pet.biExp, 0, &c[20]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bRevolutionStep, 0, &c[21]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bWildRate, 0, &c[22]);
    });

    if (!found) {
        LOG("[PetBongOut] Release blocked: Bound pet data not found in CHAR_PET! dwPetID=" + std::to_string(dwPetID));
        sendFailAck(1); // ERR_PETBONGIN_NOTFINDPET
        return;
    }

    // 4. 数据更新与解绑
    // A. 宠物归属改回玩家 charID
    DBHelper::GetInstance().ExecuteUpdate("UPDATE CHAR_PET SET dwCharID = " + std::to_string(charID) + " WHERE dwID = " + std::to_string(dwPetID));

    // B. 清空镜子中的宠物绑定
    DBHelper::GetInstance().ExecuteUpdate("UPDATE ITEMDATA SET nData1 = 0, nData15 = 0 WHERE dwItemID = " + std::to_string(dwItemID));

    LOG("[PetBongOut] Pet released successfully! dwPetID=" + std::to_string(dwPetID) + " freed from ItemID=" + std::to_string(dwItemID));

    // C. 地图具现化：创建宠物临时实体并向 AOI 广播 0x3502
    DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (g_MapInstances.count(pMapID)) {
        CMapInstance* mapInst = g_MapInstances[pMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        PlayerData* pCaster = mapInst->GetPlayer(charID + 400000000);
        if (pCaster) {
            PlayerData petEntity;
            petEntity.dwObjectID = pet.dwID + 800000000;
            petEntity.bObjectType = 4; // OBJTYPE_PET
            petEntity.dwMapID = pMapID;
            petEntity.bNpcType = pet.bNpcType;
            petEntity.wPosX = pCaster->wPosX + 2; // 主人坐标偏移
            petEntity.wPosY = pCaster->wPosY + 2;
            petEntity.bHeight = pCaster->bHeight;
            petEntity.fPosX = (float)petEntity.wPosX;
            petEntity.fPosY = (float)petEntity.wPosY;

            petEntity.dwHpCur = pet.dwHpCur;
            petEntity.dwHpMax = pet.dwHpMax;
            petEntity.wWalkSpeed = pet.bSpeed;
            petEntity.wLevel = pet.wLevel;
            petEntity.dwOwnerID = charID;

            petEntity.wWepAtk = pet.wAtkPwr;
            petEntity.dwTotalAtk = pet.wAtkPwr;

            mapInst->AddPlayer(petEntity);

            // 广播 0x3502 MAPENTER_ACK
            std::vector<BYTE> enterBuf(4);
            auto pushDWord = [&](DWORD d) { enterBuf.push_back(d&0xFF); enterBuf.push_back((d>>8)&0xFF); enterBuf.push_back((d>>16)&0xFF); enterBuf.push_back(d>>24); };
            auto pushWord = [&](WORD w) { enterBuf.push_back(w&0xFF); enterBuf.push_back(w>>8); };
            
            pushDWord(pMapID);
            pushDWord(petEntity.dwObjectID);
            enterBuf.push_back(4); // bObjectType = 4
            pushWord(petEntity.wPosX);
            pushWord(petEntity.wPosY);
            enterBuf.push_back(petEntity.bHeight);
            pushWord(0); // wDirection
            enterBuf.push_back(0); // bStatus = Stand
            enterBuf.push_back(petEntity.wWalkSpeed & 0xFF);

            PACKET_HEADER* enterHead = (PACKET_HEADER*)enterBuf.data();
            enterHead->id = 0x3502;
            enterHead->payloadSize = (WORD)(enterBuf.size() - sizeof(PACKET_HEADER));
            EncryptPacket(enterBuf.data(), 0x42);
            BroadcastPacketToMap(pMapID, enterBuf);

            LOG("[PetBongOut] Spawned pet ObjID=" + std::to_string(petEntity.dwObjectID) + " for owner=" + std::to_string(charID) + " at (" + std::to_string(petEntity.wPosX) + "," + std::to_string(petEntity.wPosY) + ")");
        }
    }

    // 5. 拼装大包并回复客户端
    std::vector<BYTE> ackBuf;
    ackBuf.resize(4, 0); // 预留包头
    ackBuf.push_back(0); // bResult = 0

    auto pushDWord = [&](DWORD d) { ackBuf.push_back(d&0xFF); ackBuf.push_back((d>>8)&0xFF); ackBuf.push_back((d>>16)&0xFF); ackBuf.push_back(d>>24); };
    auto pushWord = [&](WORD w) { ackBuf.push_back(w&0xFF); ackBuf.push_back(w>>8); };
    auto pushByte = [&](BYTE b) { ackBuf.push_back(b); };
    auto pushInt64 = [&](long long int d) { pushDWord((DWORD)(d & 0xFFFFFFFF)); pushDWord((DWORD)(d >> 32)); };

    pushDWord(pet.dwID);
    pushDWord(charID); // dwOwnID
    pushDWord(pet.dwMapID);
    pushByte(pet.bNpcType);

    std::string nameStr(pet.szName);
    pushWord((WORD)nameStr.length());
    for (char ch : nameStr) pushByte((BYTE)ch);

    pushWord(pet.wLevel);
    pushWord(pet.wPosX);
    pushWord(pet.wPosY);
    pushByte(pet.bHeight);
    pushWord(pet.wPosX); // wDesPosX
    pushWord(pet.wPosY); // wDesPosY
    pushByte(pet.bHeight); // bDesHeight
    pushWord(0); // wDirection
    pushDWord(pet.dwHpMax);
    pushDWord(pet.dwHpCur);
    pushWord(pet.wAtkPwr);
    pushWord(pet.wDefPwr);
    pushWord(pet.wAtkRating);
    pushWord(pet.wAvoidRatio);
    pushByte(pet.bSpeed);
    pushWord(pet.wMeleeAtkRange);
    pushWord(pet.wShotAtkRange);
    pushByte(pet.bAtkType);
    pushDWord(pet.dwRefNpcID);
    pushByte(pet.bCurJob);
    long long i64LevelExp = PetLevelExpManager::GetInstance().GetLevelStartExp(pet.wLevel);
    long long i64NextLevelUpExp = i64LevelExp + PetLevelExpManager::GetInstance().GetNeedExp(pet.wLevel);
    pushInt64(pet.biExp);
    pushInt64(i64LevelExp);
    pushInt64(i64NextLevelUpExp);
    pushByte(pet.bRevolutionStep);
    pushByte(pet.bWildRate);
    for (int i = 0; i < 6; i++) pushWord(0); // wVisualID[6]

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x354A; // CS_NC_PETBONGOUT_ACK
    head->payloadSize = (WORD)(ackBuf.size() - sizeof(PACKET_HEADER));
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), (int)ackBuf.size(), 0);

    // 刷新包裹
    BYTE reqPayload[1] = { bSackID };
    OnItemListReq(clientSocket, charID, reqPayload, 1);
}

void SendPetListAck(SOCKET clientSocket, DWORD charID) {
    if (charID == 0) return;

    struct DbPetRow {
        DWORD dwID = 0;
        DWORD dwMapID = 0;
        BYTE bNpcType = 0;
        char szName[32] = {0};
        WORD wLevel = 0;
        WORD wPosX = 0;
        WORD wPosY = 0;
        BYTE bHeight = 0;
        DWORD dwHpMax = 0;
        DWORD dwHpCur = 0;
        WORD wAtkPwr = 0;
        WORD wDefPwr = 0;
        WORD wAtkRating = 0;
        WORD wAvoidRatio = 0;
        BYTE bSpeed = 0;
        WORD wMeleeAtkRange = 0;
        WORD wShotAtkRange = 0;
        BYTE bAtkType = 0;
        DWORD dwRefNpcID = 0;
        BYTE bCurJob = 0;
        long long biExp = 0;
        BYTE bRevolutionStep = 0;
        BYTE bWildRate = 0;
    };

    std::vector<DbPetRow> pets;
    std::string qPet = "SELECT dwID, dwMapID, bNpcType, szName, wLevel, wPosX, wPosY, bHeight, dwHpMax, dwHpCur, wAtkPwr, wDefPwr, wAtkRating, wAvoidRatio, bSpeed, wMeleeAtkRange, wShotAtkRange, bAtkType, dwRefNpcID, bCurJob, biExp, bRevolutionStep, bWildRate FROM CHAR_PET WHERE dwCharID = " + std::to_string(charID);
    DBHelper::GetInstance().ExecuteQuery(qPet, [&](SQLHSTMT hStmt) {
        DbPetRow pet;
        SQLLEN c[23];
        int idx = 1;
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &pet.dwID, 0, &c[0]);
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &pet.dwMapID, 0, &c[1]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bNpcType, 0, &c[2]);
        SQLGetData(hStmt, idx++, SQL_C_CHAR, pet.szName, sizeof(pet.szName), &c[3]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wLevel, 0, &c[4]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wPosX, 0, &c[5]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wPosY, 0, &c[6]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bHeight, 0, &c[7]);
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &pet.dwHpMax, 0, &c[8]);
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &pet.dwHpCur, 0, &c[9]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wAtkPwr, 0, &c[10]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wDefPwr, 0, &c[11]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wAtkRating, 0, &c[12]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wAvoidRatio, 0, &c[13]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bSpeed, 0, &c[14]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wMeleeAtkRange, 0, &c[15]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &pet.wShotAtkRange, 0, &c[16]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bAtkType, 0, &c[17]);
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &pet.dwRefNpcID, 0, &c[18]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bCurJob, 0, &c[19]);
        SQLGetData(hStmt, idx++, SQL_C_SBIGINT, &pet.biExp, 0, &c[20]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bRevolutionStep, 0, &c[21]);
        SQLGetData(hStmt, idx++, SQL_C_UTINYINT, &pet.bWildRate, 0, &c[22]);
        pets.push_back(pet);
    });

    LOG("[PetList] Syncing " + std::to_string(pets.size()) + " pets for charID=" + std::to_string(charID));

    std::vector<BYTE> ackBuf;
    ackBuf.resize(4, 0); // 预留包头

    auto pushDWord = [&](DWORD d) { ackBuf.push_back(d&0xFF); ackBuf.push_back((d>>8)&0xFF); ackBuf.push_back((d>>16)&0xFF); ackBuf.push_back(d>>24); };
    auto pushWord = [&](WORD w) { ackBuf.push_back(w&0xFF); ackBuf.push_back(w>>8); };
    auto pushByte = [&](BYTE b) { ackBuf.push_back(b); };
    auto pushInt64 = [&](long long int d) { pushDWord((DWORD)(d & 0xFFFFFFFF)); pushDWord((DWORD)(d >> 32)); };

    pushDWord(charID);
    pushByte((BYTE)pets.size());

    for (const auto& pet : pets) {
        pushDWord(pet.dwID);
        pushDWord(charID); // dwOwnID
        pushDWord(pet.dwMapID);
        pushByte(pet.bNpcType);

        std::string nameStr(pet.szName);
        pushWord((WORD)nameStr.length());
        for (char ch : nameStr) pushByte((BYTE)ch);

        pushWord(pet.wLevel);
        pushWord(pet.wPosX);
        pushWord(pet.wPosY);
        pushByte(pet.bHeight);
        pushWord(pet.wPosX); // wDesPosX
        pushWord(pet.wPosY); // wDesPosY
        pushByte(pet.bHeight); // bDesHeight
        pushWord(0); // wDirection
        pushDWord(pet.dwHpMax);
        pushDWord(pet.dwHpCur);
        pushWord(pet.wAtkPwr);
        pushWord(pet.wDefPwr);
        pushWord(pet.wAtkRating);
        pushWord(pet.wAvoidRatio);
        pushByte(pet.bSpeed);
        pushWord(pet.wMeleeAtkRange);
        pushWord(pet.wShotAtkRange);
        pushByte(pet.bAtkType);
        pushDWord(pet.dwRefNpcID);
        pushByte(pet.bCurJob);
        long long i64LevelExp = PetLevelExpManager::GetInstance().GetLevelStartExp(pet.wLevel);
        long long i64NextLevelUpExp = i64LevelExp + PetLevelExpManager::GetInstance().GetNeedExp(pet.wLevel);
        pushInt64(pet.biExp);
        pushInt64(i64LevelExp);
        pushInt64(i64NextLevelUpExp);
        pushByte(pet.bRevolutionStep);
        pushByte(pet.bWildRate);
        for (int i = 0; i < 6; i++) pushWord(0); // wVisualID[6]
    }

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x3B20; // CS_IF_PETLIST_ACK
    head->payloadSize = (WORD)(ackBuf.size() - sizeof(PACKET_HEADER));
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), (int)ackBuf.size(), 0);
}


// ==========================================
// 遁身符 "记录当前位置" 处理器
// CS_IM_REMARKITEM_REQ (0x4249) → CS_IM_REMARKITEM_ACK (0x424A)
// 客户端 Payload: bAction(1) + dwItemID(4) + bSackID(1) + bSackPos(1) = 7 bytes
// ACK Payload:    bResult(1)
//   0 = SUCCESS, 1 = NOTFOUND, 2 = INVALIDITEM, 3 = INVALIDPOSITION, 4 = FAIL
// ==========================================
void OnRemarkItemReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    // 快捷发送 ACK 的闭包
    auto sendAck = [&](BYTE bResult) {
        std::vector<BYTE> ackBuf(4);
        ackBuf.push_back(bResult);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x424A; // CS_IM_REMARKITEM_ACK
        head->payloadSize = 1;
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), (int)ackBuf.size(), 0);
    };

    if (totalSize < 7) {
        LOG("[ItemHandler] OnRemarkItemReq: payload 过短: " + std::to_string(totalSize));
        sendAck(4); // FAIL
        return;
    }

    BYTE bAction = payload[0];
    DWORD dwItemID = *(DWORD*)(payload + 1);
    BYTE bSackID = payload[5];
    BYTE bSackPos = payload[6];

    LOG("[ItemHandler] OnRemarkItemReq: charID=" + std::to_string(charID)
        + " action=" + std::to_string(bAction)
        + " dwItemID=" + std::to_string(dwItemID));

    // 仅处理 ACT_REMARKITEM_PORTAL (bAction == 1)
    if (bAction != 1) {
        LOG("[ItemHandler] OnRemarkItemReq: 未知 action: " + std::to_string(bAction));
        sendAck(4); // FAIL
        return;
    }

    // 1. 查找物品实例，获取 wRefID
    WORD wRefID = 0;
    {
        ItemDB::ItemBasicInfo ib;
        if (!ItemDB::GetInstance().GetItemBasicInfo(dwItemID, ib)) {
            LOG("[ItemHandler] OnRemarkItemReq: 物品不存在 dwItemID=" + std::to_string(dwItemID));
            sendAck(1); // NOTFOUND
            return;
        }
        wRefID = ib.wRefID;
    }

    // 2. 校验物品类型：必须是 bType=22(PORTAL) 且 bKind=1（遁身符专属子类）
    if (g_ItemTemplates.find(wRefID) == g_ItemTemplates.end()) {
        LOG("[ItemHandler] OnRemarkItemReq: 模板不存在 wRefID=" + std::to_string(wRefID));
        sendAck(1); // NOTFOUND
        return;
    }
    sItemTemplate& tpl = g_ItemTemplates[wRefID];
    if (tpl.bType != 22 || tpl.bKind != 1) {
        LOG("[ItemHandler] OnRemarkItemReq: 物品类型不符 bType=" + std::to_string(tpl.bType)
            + " bKind=" + std::to_string(tpl.bKind));
        sendAck(2); // INVALIDITEM
        return;
    }

    // 3. 获取玩家当前位置
    DWORD curMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    WORD curPosX = 0, curPosY = 0;

    if (g_MapInstances.count(curMapID)) {
        std::lock_guard<std::mutex> lock(g_MapInstances[curMapID]->GetMutex());
        PlayerData* pObj = g_MapInstances[curMapID]->GetPlayer(charID + 400000000);
        if (pObj) {
            curPosX = pObj->wPosX;
            curPosY = pObj->wPosY;
        }
    }

    if (curPosX == 0 && curPosY == 0) {
        LOG("[ItemHandler] OnRemarkItemReq: 无法获取玩家坐标");
        sendAck(4); // FAIL
        return;
    }

    // 4. 禁止在特殊地图记录坐标（副本/Boss房等）
    //    mapID: 9=天皇殿, 10/12/13/14/15 = 特殊副本区域
    if (curMapID == 9 || curMapID == 10 || curMapID == 12 ||
        curMapID == 13 || curMapID == 14 || curMapID == 15) {
        LOG("[ItemHandler] OnRemarkItemReq: 当前地图禁止记录 mapID=" + std::to_string(curMapID));
        sendAck(3); // INVALIDPOSITION
        return;
    }

    // 5. 将当前位置写入 ITEM 表的 nBasicData2(地图ID)、nBasicData4(X)、nBasicData5(Y)
    std::string updateSQL =
        "UPDATE ITEM SET nBasicData2 = " + std::to_string(curMapID) +
        ", nBasicData4 = " + std::to_string(curPosX) +
        ", nBasicData5 = " + std::to_string(curPosY) +
        " WHERE dwItemID = " + std::to_string(dwItemID);

    bool dbOk = DBHelper::GetInstance().ExecuteUpdate(updateSQL);
    if (!dbOk) {
        LOG("[ItemHandler] OnRemarkItemReq: DB更新失败 dwItemID=" + std::to_string(dwItemID));
        sendAck(4); // FAIL
        return;
    }

    LOG("[ItemHandler] OnRemarkItemReq: 坐标记录成功! dwItemID=" + std::to_string(dwItemID)
        + " mapID=" + std::to_string(curMapID)
        + " pos=(" + std::to_string(curPosX) + "," + std::to_string(curPosY) + ")");

    // 6. 刷新客户端物品数据：Remove+Add 模式强制客户端重载物品的 m_dwPotalMapID/m_wPosX/m_wPosY
    SendItemRefresh(clientSocket, dwItemID, bSackID, bSackPos);

    sendAck(0); // SUCCESS
}

// ============================================================
// 觉醒请求处理器 (CS_IM_REBIRTH_REQ = 0x426F)
// 客户端 payload: BYTE bSackID + BYTE bSackPos + DWORD dwItemID
// 服务端应答 (CS_IM_REBIRTH_ACK = 0x4270):
//   成功: BYTE bResult(0) + DWORD dwCharID + BYTE bRebirth
//   失败: BYTE bResult(1~6)
// ============================================================
void OnRebirthReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    // 觉醒 ACK 包 ID: OFFSET_CS_IM(0x4201) + 111 = 0x4270
    const WORD PKT_REBIRTH_ACK = 0x4270;

    // 错误码（与客户端 csprotocol.h 一致）
    const BYTE ERR_SUCCESS  = 0;
    const BYTE ERR_LEVEL    = 1; // 等级不足
    const BYTE ERR_STEP     = 2; // 觉醒阶段不符
    const BYTE ERR_SPACE    = 3; // 背包空间不足（保留，当前未使用）
    const BYTE ERR_INTERNAL = 4; // 内部错误

    // 发送失败应答的辅助函数
    auto sendError = [&](BYTE errCode) {
        std::vector<BYTE> ack(4);
        ack.push_back(errCode);
        PACKET_HEADER* h = (PACKET_HEADER*)ack.data();
        h->id = PKT_REBIRTH_ACK;
        h->payloadSize = ack.size() - sizeof(PACKET_HEADER);
        EncryptPacket(ack.data(), 0x42);
        SafeSend(clientSocket, (const char*)ack.data(), ack.size(), 0);
    };

    if (totalSize < 6) {
        LOG("[ItemHandler] OnRebirthReq: payload too small: " + std::to_string(totalSize));
        sendError(ERR_INTERNAL);
        return;
    }

    BYTE bSackID   = payload[0];
    BYTE bSackPos  = payload[1];
    DWORD dwItemID = *(DWORD*)(payload + 2);

    LOG("[ItemHandler] OnRebirthReq: charID=" + std::to_string(charID) +
        " bSackID=" + std::to_string(bSackID) +
        " bSackPos=" + std::to_string(bSackPos) +
        " dwItemID=" + std::to_string(dwItemID));

    // 1. 从数据库读取物品的 wRefID
    WORD wRefID = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT wRefID FROM ITEM WHERE dwItemID = " + std::to_string(dwItemID),
        [&](SQLHSTMT hStmt) { SQLLEN c; SQLGetData(hStmt, 1, SQL_C_USHORT, &wRefID, 0, &c); });

    if (wRefID == 0) {
        LOG("[ItemHandler] OnRebirthReq: Item not found! dwItemID=" + std::to_string(dwItemID));
        sendError(ERR_INTERNAL);
        return;
    }

    // 2. 确认物品模板是觉醒物品 (bType == 31)
    auto it = g_ItemTemplates.find(wRefID);
    if (it == g_ItemTemplates.end()) {
        LOG("[ItemHandler] OnRebirthReq: Template not found! wRefID=" + std::to_string(wRefID));
        sendError(ERR_INTERNAL);
        return;
    }
    const sItemTemplate& tpl = it->second;
    if (tpl.bType != 31) { // ITEMTYPE_REBIRTH = 31
        LOG("[ItemHandler] OnRebirthReq: Not a rebirth item! bType=" + std::to_string(tpl.bType));
        sendError(ERR_INTERNAL);
        return;
    }

    // 3. 获取角色当前觉醒次数和等级
    BYTE curRebirth = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT bRebirth FROM CHAR_BASIC WHERE dwCharID = " + std::to_string(charID),
        [&](SQLHSTMT hStmt) { SQLLEN c; SQLGetData(hStmt, 1, SQL_C_UTINYINT, &curRebirth, 0, &c); });

    CharacterDB::CharPower cp;
    if (!CharacterDB::GetInstance().GetCharData(charID, cp)) {
        LOG("[ItemHandler] OnRebirthReq: GetCharData failed for charID=" + std::to_string(charID));
        sendError(ERR_INTERNAL);
        return;
    }

    // 4. 校验觉醒阶段（物品的 nBasicData1 必须等于角色当前 bRebirth）
    //    DB 查询结果: nBasicData1 = 觉醒阶段要求 (0=未觉醒者, 1=1次觉醒者, ...)
    BYTE reqStep = (BYTE)tpl.nBasicData1;
    if (curRebirth != reqStep) {
        LOG("[ItemHandler] OnRebirthReq: Step mismatch! curRebirth=" + std::to_string(curRebirth) +
            " reqStep=" + std::to_string(reqStep));
        sendError(ERR_STEP);
        return;
    }

    // 5. 校验等级（物品的 nBasicData3 为需求等级）
    WORD reqLevel = (WORD)tpl.nBasicData3;
    if (cp.wLevel < reqLevel) {
        LOG("[ItemHandler] OnRebirthReq: Level too low! curLevel=" + std::to_string(cp.wLevel) +
            " reqLevel=" + std::to_string(reqLevel));
        sendError(ERR_LEVEL);
        return;
    }

    // 6. 所有校验通过 → 执行觉醒
    BYTE newRebirth = curRebirth + 1;

    // 6a. 更新数据库觉醒次数
    DBHelper::GetInstance().ExecuteUpdate(
        "UPDATE CHAR_BASIC SET bRebirth = " + std::to_string(newRebirth) +
        " WHERE dwCharID = " + std::to_string(charID));

    // 6b. 消耗觉醒物品（级联删除 SACKITEM + ITEMDATA + ITEM 等所有关联记录）
    ItemDB::GetInstance().DeleteItemCascade(dwItemID);

    LOG("[ItemHandler] OnRebirthReq: SUCCESS! charID=" + std::to_string(charID) +
        " rebirth " + std::to_string(curRebirth) + " -> " + std::to_string(newRebirth) +
        " consumed item " + std::to_string(dwItemID));

    // 7. 更新内存中 PlayerManager 的觉醒数据
    DWORD dwObjectID = charID + 400000000;
    DWORD mapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (g_MapInstances.count(mapID)) {
        std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
        sServerObject* pObj = g_MapInstances[mapID]->GetPlayer(dwObjectID);
        if (pObj) {
            pObj->bRebirth = newRebirth;
        }
    }

    // 8. 从客户端背包移除物品视觉（SendRemoveFromSackAck）
    //    正确格式: bSackID(1) + bSackPos(1) + bReason(1) = payload 3字节
    {
        std::vector<BYTE> rmBuf(7);
        PACKET_HEADER* rh = (PACKET_HEADER*)rmBuf.data();
        rh->id = 0x4208; // CS_IM_REMOVEFROMSACK_ACK
        rh->payloadSize = 3;
        rmBuf[4] = bSackID;   // 背包页码
        rmBuf[5] = bSackPos;  // 页内位置
        rmBuf[6] = 0;         // 移除原因 (0 = 正常消耗)
        EncryptPacket(rmBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)rmBuf.data(), rmBuf.size(), 0);
    }

    // 9. 发送成功 ACK（触发觉醒动画 + 设置 bRebirth）
    auto sendSuccess = [&](SOCKET targetSocket) {
        std::vector<BYTE> ack(4);
        ack.push_back(ERR_SUCCESS);
        // dwCharID（客户端用来定位角色对象播放动画）
        ack.push_back(dwObjectID & 0xFF); ack.push_back((dwObjectID >> 8) & 0xFF);
        ack.push_back((dwObjectID >> 16) & 0xFF); ack.push_back((dwObjectID >> 24) & 0xFF);
        // bRebirth（新觉醒次数）
        ack.push_back(newRebirth);
        PACKET_HEADER* h = (PACKET_HEADER*)ack.data();
        h->id = PKT_REBIRTH_ACK;
        h->payloadSize = ack.size() - sizeof(PACKET_HEADER);
        EncryptPacket(ack.data(), 0x42);
        SafeSend(targetSocket, (const char*)ack.data(), ack.size(), 0);
    };

    // 发送给请求者
    sendSuccess(clientSocket);

    // 10. 广播觉醒成功包给 AOI 内所有其他玩家（让其他玩家看到觉醒动画和标记）
    if (g_MapInstances.count(mapID)) {
        WORD myPosX = 0, myPosY = 0;
        {
            std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
            sServerObject* pObj = g_MapInstances[mapID]->GetPlayer(dwObjectID);
            if (pObj) { myPosX = pObj->wPosX; myPosY = pObj->wPosY; }
        }
        if (myPosX > 0 && myPosY > 0) {
            SessionMgr::GetInstance().ForEachSocketInMap(mapID, [&](SOCKET s, DWORD sCharID) {
                if (s != clientSocket) {
                    DWORD sObjID = sCharID + 400000000;
                    bool inRange = false;
                    {
                        std::lock_guard<std::mutex> lockA(g_MapInstances[mapID]->GetMutex());
                        sServerObject* sObj = g_MapInstances[mapID]->GetPlayer(sObjID);
                        if (sObj) {
                            int dx = sObj->wPosX - myPosX;
                            int dy = sObj->wPosY - myPosY;
                            inRange = (dx*dx + dy*dy <= 150*150);
                        }
                    }
                    if (inRange) {
                        sendSuccess(s);
                    }
                }
            });
        }
    }
}

#include "ItemHandler.h"
#include "../DB/CharacterDB.h"
#include "../DB/ItemDB.h"
#include "../GameObjects/MugongManager.h"
#include "../GameObjects/DropManager.h"
#include <set>
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"

extern std::map<DWORD, CMapInstance*> g_MapInstances;
#include "ShopHandler.h"
#include "../Network/SystemMessage.h"
#include "../DBHelper.h"

// 外部声明，用于打开随身仓库
void OnItemListInBankReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize);

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
            int pos = row.bSackPos, itemid = row.dwItemID, vis = row.wVisualID, type = row.bType, kind = row.bKind;
            int lvl = row.wLevel, cost = row.dwCost, dat18 = row.nData18, dat19 = row.nData19;
            int refid = row.wRefID, amount = row.wAmount;
            int dat20 = row.nData20, dat21 = row.nData21, dat25 = row.nData25;
            int d[17]; for (int i=0;i<17;i++) d[i]=row.d[i];
            
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
            for (int i=0;i<17;i++) { if (d[i]==-9999) d[i]=0; }
            
            std::string itemName(row.szName);
            if (itemName.empty() && g_ItemTemplates.count(refid)) itemName = g_ItemTemplates[refid].szName;

            std::vector<BYTE> bi; 
            auto pushDWord = [&](DWORD dw) { bi.push_back(dw & 0xFF); bi.push_back((dw>>8)&0xFF); bi.push_back((dw>>16)&0xFF); bi.push_back((dw>>24)&0xFF); };
            auto pushWord = [&](WORD w) { bi.push_back(w & 0xFF); bi.push_back((w>>8)&0xFF); };
            auto pushByte = [&](BYTE b) { bi.push_back(b); };
            pushByte(sackId == 0 ? pos : (pos - 20 - (sackId - 1) * 40));
            pushDWord(itemid); pushWord(refid);
            pushByte(type); pushByte(kind); pushWord(vis);
            
            pushWord(itemName.length());
            for (char ch : itemName) pushByte(ch);
            
            pushDWord(cost); pushWord(lvl); pushByte(charType); // bNeedCharType
            pushWord(amount);
            
            if (type >= 1 && type <= 9) { // Weapon/Cloth..Bongin
                pushWord(nd1); pushWord(nd2); pushWord(nd3); pushWord(nd4); pushWord(nd5); // BasicData 1-5
                pushByte(d[0]); // DecrDurRate
                pushWord(d[1]); pushWord(d[2]); // CurDur, MaxDur
                pushDWord(d[3]); pushDWord(d[4]); pushDWord(d[5]); pushWord(d[6]); pushWord(d[7]); // Attacks (AtkPwr, DefPwr, AtkRating upgraded to DWORD)
                pushDWord(d[8]); pushDWord(d[9]); pushWord(d[10]); pushWord(d[11]); pushWord(d[12]); // HP/IP (IncrHp, IncrIp upgraded to DWORD)
                pushByte(dat18); pushByte(dat19); // bRarity, bStxType
                pushByte(d[13]); pushByte(d[14]); pushByte(d[15]); pushByte(d[16]); // Limit/Modify/Repair/Discount
                
                if (type == 9) { // BONGIN
                    pushDWord(0); pushWord(0); pushWord(0); pushWord(0); pushWord(0);
                } else if (type == 8) { // SOCKET
                    pushByte(0); pushByte(0); pushByte(0); pushByte(0); pushByte(0); pushByte(0); pushByte(0); pushByte(0);
                } else {
                    pushByte(0); // bPuzzleType
                }
                if (type >= 1 && type <= 4) { pushByte(dat19); pushByte(dat20); pushByte(dat21); pushWord(dat25); } // Sockets
            } else {
                switch (type) {
                    case 11: case 12: case 13: case 14: case 17:
                        pushByte(0); pushWord(d[1]); pushWord(d[2]); break;
                    case 15:
                        pushByte(d[0]); pushWord(d[1]); pushWord(d[2]); pushByte(0); pushByte(0); break;
                    case 16:
                        pushWord(0); pushByte(0); pushWord(0); pushByte(0); pushWord(0); break;
                    case 18:
                        pushByte(0); pushDWord(0); pushWord(d[1]); pushWord(d[2]); pushByte(0); break;
                    case 19:
                        pushWord(0); pushWord(0); break;
                    case 20:
                        pushByte(0); pushDWord(0); break;
                    case 21: {
                        DWORD mid = nd2;
                        sMugongTemplate* mg = MugongManager::GetInstance()->GetTemplate(mid);
                        BYTE typeVal = mg ? mg->bType : 0;
                        BYTE kindVal = mg ? mg->bKind : 0;
                        pushWord(lvl); pushDWord(mid); pushByte(typeVal); pushByte(kindVal); pushByte(1); break;
                    }
                    case 22:
                        pushDWord(nd2); pushByte((BYTE)nd3); pushWord((WORD)nd4); pushWord((WORD)nd5); break;
                    case 23:
                        pushDWord(0); pushDWord(0); pushDWord(0); pushByte(0); pushByte(0); break;
                    case 25:
                        pushByte(0); pushWord(0); pushWord(0); break;
                    case 27:
                        pushByte(0); pushDWord(0); pushByte(0); pushByte(0); pushByte(0); pushByte(0); pushDWord(0); break;
                    case 29:
                        pushWord(0); pushWord(0); break;
                    case 32:
                        pushWord(0); pushWord(d[1]); pushWord(d[2]); pushDWord(0); break;
                    case 31:
                        pushByte(0); pushWord(0); pushWord(0); break;
                    case 34:
                        pushDWord(0); pushByte(0); break;
                }
            }
            pushWord((WORD)dat20); // wRebuithValue
            LOG("[ItemHandler] Item pos=" + std::to_string(pos) + " type=" + std::to_string(type) + " refid=" + std::to_string(refid) + " name=" + itemName + " size=" + std::to_string(bi.size()));
            {   // Hex dump for debugging
                std::string hex;
                for (size_t h = 0; h < bi.size(); h++) {
                    char tmp[8]; sprintf(tmp, "%02X ", bi[h]); hex += tmp;
                }
                LOG("[ItemHandler] HEX: " + hex);
            }
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
                        return;
                    }

                    int pLevel = 0, pCharType = 0, pStr = 0, pDex = 0, pVit = 0, pSus = 0;
                    CharacterDB::CharPower cpEq;
                    if (CharacterDB::GetInstance().GetCharData(dbCharID, cpEq)) {
                        pLevel = cpEq.wLevel; pCharType = cpEq.bCharType;
                        pStr = cpEq.wStr; pDex = cpEq.wDex; pVit = cpEq.wVit; pSus = cpEq.wSus;
                    }

                    if (pLevel < pTpl->nBasicData1 || 
                        (pTpl->bCharType != 0 && pCharType != pTpl->bCharType) ||
                        pDex < pTpl->nBasicData2 || pStr < pTpl->nBasicData3 ||
                        pSus < pTpl->nBasicData4 || pVit < pTpl->nBasicData5) {
                        LOG("[ItemHandler] Requirements not met for dwItemID=" + std::to_string(dwSrcObjID));
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
                        pushWord((WORD)tplOpen.nData1); pushByte((BYTE)tplOpen.nData2); pushWord((WORD)tplOpen.nData3); pushByte((BYTE)tplOpen.nData4); pushWord((WORD)tplOpen.nData5); break;
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

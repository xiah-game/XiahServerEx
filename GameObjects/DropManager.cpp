#include "DropManager.h"
#include "../DBHelper.h"
#include "../DB/GameDataDB.h"
#include "PlayerManager.h"
#include "../DB/ItemDB.h"
#include "../DB/CharacterDB.h"
#include "../Network/SystemMessage.h"
#include "MapInstance.h"
#include "../GameObjects/MugongManager.h"
#include "QuestManager.h"
#include "../Handlers/PartyHandler.h"
#include <iostream>

extern std::map<DWORD, CMapInstance*> g_MapInstances;

static void pushByte(std::vector<BYTE>& buf, BYTE b) { buf.push_back(b); }
static void pushWord(std::vector<BYTE>& buf, WORD w) { buf.push_back(w & 0xFF); buf.push_back((w >> 8) & 0xFF); }
static void pushDWord(std::vector<BYTE>& buf, DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back(d >> 24); }

class BufferWriter {
public:
    std::vector<BYTE> buf;
    BufferWriter() {}
    void write(const void* data, size_t size) {
        const BYTE* p = (const BYTE*)data;
        buf.insert(buf.end(), p, p + size);
    }
    template<typename T> void write(T val) { write(&val, sizeof(T)); }
    void writeString(const std::string& str) {
        std::string trimmed = str;
        size_t endpos = trimmed.find_last_not_of(" \n\r\t");
        if (endpos != std::string::npos) trimmed = trimmed.substr(0, endpos + 1);
        else trimmed = "";
        WORD len = trimmed.size();
        write<WORD>(len);
        if (len > 0) write(trimmed.data(), len);
    }
};

// Shared map item ID counter
static DWORD s_nextMapItemID = 5000000;

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

DropManager* DropManager::s_instance = nullptr;

DropManager* DropManager::GetInstance() {
    if (!s_instance) {
        s_instance = new DropManager();
    }
    return s_instance;
}

void DropManager::LoadDropTables() {
    // Load legacy NPC_ROOTITEM (fallback for NPCs without drop groups)
    GameDataDB::GetInstance().LoadRootItems(m_rootItems);
    LOG("[DropManager] Loaded " + std::to_string(m_rootItems.size()) + " legacy NPC_ROOTITEM NPC types.");
    
    // Load new Drop Group system
    LoadDropGroups();
}

void DropManager::LoadDropGroups() {
    GameDataDB::GetInstance().LoadDropGroups(m_dropGroups);
    GameDataDB::GetInstance().LoadDropGroupItems(m_dropGroups);
    
    int groupCount = 0, itemCount = 0;
    for (auto& pair : m_dropGroups) {
        groupCount += (int)pair.second.size();
        for (auto& g : pair.second) itemCount += (int)g.items.size();
    }
    LOG("[DropManager] Loaded " + std::to_string(groupCount) + " drop groups with " + std::to_string(itemCount) + " items.");
}
// BroadcastPacketToMap now declared in PlayerManager.h

// Helper: drop a single item to the map
void DropManager::DropItemToMap(DWORD killerID, const MonsterData& obj, DWORD itemRefID, bool useRandomOffset) {
    auto tplIt = g_ItemTemplates.find((WORD)itemRefID);
    if (tplIt == g_ItemTemplates.end()) {
        LOG("[Drop] Item RefID " + std::to_string(itemRefID) + " not found in ITEMTEMPLATE!");
        return;
    }
    const sItemTemplate& tpl = tplIt->second;
    
    WORD dropX = obj.wPosX;
    WORD dropY = obj.wPosY;
    if (useRandomOffset) {
        dropX += (rand() % 11) - 5;
        dropY += (rand() % 11) - 5;
    }
    
    BufferWriter bw;
    bw.write<BYTE>(0);      // bResult
    bw.write<WORD>(1);      // wItemNum
    bw.write<DWORD>(obj.dwMapID);
    bw.write<WORD>(dropX);
    bw.write<WORD>(dropY);
    bw.write<BYTE>(0);      // bHeight
    
    s_nextMapItemID++;
    bw.write<DWORD>(s_nextMapItemID);
    
    WORD visualID = tpl.wVisualID;
    if (visualID == 0) {
        visualID = 10050; // Fallback to safe generic money pouch visual ID
    }
    
    bw.write<BYTE>(tpl.bType);
    bw.write<WORD>(visualID);
    bw.writeString(tpl.szName);
    
    DWORD dbItemID = rand() * rand();
    bw.write<DWORD>(dbItemID);
    bw.write<DWORD>(1);         // amount
    bw.write<DWORD>(0);         // ownerID (always send 0 so client bypasses local check)
    bw.write<BYTE>(0);  // bType
    bw.write<BYTE>(0);  // bFESocket
    bw.write<BYTE>(0);  // bChangeItem
    
    // Create map drop entry
    sMapDrop newDrop;
    newDrop.dwMapItemID = s_nextMapItemID;
    newDrop.dbItemID = dbItemID;
    newDrop.wRefID = tpl.wRefID;
    newDrop.amount = 1;
    newDrop.ownerID = killerID;
    newDrop.dropTime = GetTickCount();
    newDrop.bType = tpl.bType;
    newDrop.wVisualID = visualID;
    newDrop.name = tpl.szName;
    newDrop.isMoney = false;
    newDrop.mapID = obj.dwMapID;
    newDrop.wPosX = dropX;
    newDrop.wPosY = dropY;
    memset(newDrop.nData, 0, sizeof(newDrop.nData));
    
    newDrop.nData[0]  = tpl.nData1;   newDrop.nData[1]  = tpl.nData2;
    newDrop.nData[2]  = tpl.nData3;   newDrop.nData[3]  = tpl.nData4;
    newDrop.nData[4]  = tpl.nData5;   newDrop.nData[5]  = tpl.nData6;
    newDrop.nData[6]  = tpl.nData7;   newDrop.nData[7]  = tpl.nData8;
    newDrop.nData[8]  = tpl.nData9;   newDrop.nData[9]  = tpl.nData10;
    newDrop.nData[10] = tpl.nData11;  newDrop.nData[11] = tpl.nData12;
    newDrop.nData[12] = tpl.nData13;  newDrop.nData[13] = tpl.nData14;
    newDrop.nData[14] = tpl.nData15;  newDrop.nData[15] = tpl.nData16;
    newDrop.nData[16] = tpl.nData17;  newDrop.nData[17] = tpl.nData18;
    newDrop.nData[18] = tpl.nData19;  newDrop.nData[19] = tpl.nData20;
    newDrop.nData[20] = tpl.nData21;  newDrop.nData[21] = tpl.nData22;
    newDrop.nData[22] = tpl.nData23;  newDrop.nData[23] = tpl.nData24;
    newDrop.nData[24] = tpl.nData25;
    
    // Xiah Socket Rolling Logic
    if (tpl.bType <= 4) {
        int wRatio = rand() % 6000;
        if (tpl.bType == 1 || tpl.bType == 2) {
            if (wRatio < 30) {
                newDrop.nData[18] = 1; newDrop.nData[19] = 1;
            } else if (wRatio < 630) {
                newDrop.nData[18] = 1;
            }
        } else if (tpl.bType == 3 || tpl.bType == 4) {
            if (wRatio < 30) {
                newDrop.nData[18] = 1;
            }
        }
    }
    
    {
        std::lock_guard<std::mutex> lock(m_dropMutex);
        m_activeDrops[s_nextMapItemID] = newDrop;
    }
    
    std::vector<BYTE> finalBuf(4 + bw.buf.size());
    PACKET_HEADER* head = (PACKET_HEADER*)finalBuf.data();
    head->id = 0x420C;
    head->payloadSize = (WORD)bw.buf.size();
    memcpy(finalBuf.data() + 4, bw.buf.data(), bw.buf.size());
    EncryptPacket(finalBuf.data(), 0x42);
    BroadcastPacketToMap(obj.dwMapID, finalBuf);
    
    LOG("[Drop] Dropped Item: " + tpl.szName + " (RefID: " + std::to_string(tpl.wRefID) + ")");
}

void DropManager::DropCustomItemToMap(DWORD killerID, const MonsterData& obj, const ItemDB::FullItemRow& row, bool useRandomOffset) {
    auto tplIt = g_ItemTemplates.find((WORD)row.wRefID);
    if (tplIt == g_ItemTemplates.end()) {
        LOG("[Drop] Item RefID " + std::to_string(row.wRefID) + " not found in ITEMTEMPLATE!");
        return;
    }
    const sItemTemplate& tpl = tplIt->second;
    
    WORD dropX = obj.wPosX;
    WORD dropY = obj.wPosY;
    if (useRandomOffset) {
        dropX += (rand() % 11) - 5;
        dropY += (rand() % 11) - 5;
    }
    
    BufferWriter bw;
    bw.write<BYTE>(0);      // bResult
    bw.write<WORD>(1);      // wItemNum
    bw.write<DWORD>(obj.dwMapID);
    bw.write<WORD>(dropX);
    bw.write<WORD>(dropY);
    bw.write<BYTE>(0);      // bHeight
    
    s_nextMapItemID++;
    bw.write<DWORD>(s_nextMapItemID);
    
    WORD visualID = row.wVisualID;
    if (visualID == 0) visualID = tpl.wVisualID;
    if (visualID == 0) visualID = 10050; // Fallback
    
    bw.write<BYTE>(row.bType);
    bw.write<WORD>(visualID);
    
    std::string itemName(row.szName);
    if (itemName.empty()) itemName = tpl.szName;
    bw.writeString(itemName);
    
    DWORD dbItemID = row.dwItemID;
    if (dbItemID == 0) dbItemID = rand() * rand();
    bw.write<DWORD>(dbItemID);
    bw.write<DWORD>(row.wAmount); // amount
    bw.write<DWORD>(0);           // ownerID (always send 0 so client bypasses local check)
    bw.write<BYTE>(0);            // bType
    
    // Set bFESocket based on D.nData18 (row.nData18 > 0)
    BYTE bFESocket = (row.nData18 > 0) ? 1 : 0;
    bw.write<BYTE>(bFESocket);    // bFESocket
    bw.write<BYTE>(0);            // bChangeItem
    
    // Create map drop entry
    sMapDrop newDrop;
    newDrop.dwMapItemID = s_nextMapItemID;
    newDrop.dbItemID = dbItemID;
    newDrop.wRefID = row.wRefID;
    newDrop.amount = row.wAmount;
    newDrop.ownerID = killerID;
    newDrop.dropTime = GetTickCount();
    newDrop.bType = row.bType;
    newDrop.wVisualID = visualID;
    newDrop.name = itemName;
    newDrop.isMoney = false;
    newDrop.mapID = obj.dwMapID;
    newDrop.wPosX = dropX;
    newDrop.wPosY = dropY;
    memset(newDrop.nData, 0, sizeof(newDrop.nData));
    
    // Copy all 25 stats from row to newDrop.nData
    for (int i = 0; i < 17; i++) {
        newDrop.nData[i] = row.d[i];
    }
    newDrop.nData[17] = row.nData18; // rarity
    newDrop.nData[18] = row.nData19; // socket 1
    newDrop.nData[19] = row.nData20; // socket 2 / rebuild value / dat20
    newDrop.nData[20] = row.nData21; // socket 3
    newDrop.nData[21] = 0;
    newDrop.nData[22] = 0;
    newDrop.nData[23] = 0;
    newDrop.nData[24] = row.nData25; // socket 4
    
    {
        std::lock_guard<std::mutex> lock(m_dropMutex);
        m_activeDrops[s_nextMapItemID] = newDrop;
    }
    
    std::vector<BYTE> finalBuf(4 + bw.buf.size());
    PACKET_HEADER* head = (PACKET_HEADER*)finalBuf.data();
    head->id = 0x420C;
    head->payloadSize = (WORD)bw.buf.size();
    memcpy(finalBuf.data() + 4, bw.buf.data(), bw.buf.size());
    EncryptPacket(finalBuf.data(), 0x42);
    BroadcastPacketToMap(obj.dwMapID, finalBuf);
    
    LOG("[Drop] Dropped Custom/Upgraded Item: " + itemName + " (RefID: " + std::to_string(row.wRefID) + ")");
}

// Helper: drop money to the map
void DropManager::DropMoneyToMap(DWORD killerID, const MonsterData& obj, DWORD amount) {
    int offsetX = (rand() % 31) - 15;
    int offsetY = (rand() % 31) - 15;
    WORD dropX = obj.wPosX + offsetX;
    WORD dropY = obj.wPosY + offsetY;
    std::string moneyName = "\xC7\xAE";
    
    BufferWriter bw;
    bw.write<BYTE>(0);      // bResult
    bw.write<WORD>(1);      // wItemNum
    bw.write<DWORD>(obj.dwMapID);
    bw.write<WORD>(dropX);
    bw.write<WORD>(dropY);
    bw.write<BYTE>(0);      // bHeight
    
    s_nextMapItemID++;
    bw.write<DWORD>(s_nextMapItemID);
    
    bw.write<BYTE>(32);     // bType = 0x20 (money)
    bw.write<WORD>(10050);  // wVisualID
    bw.writeString(moneyName);
    
    DWORD dbItemID = rand() * rand();
    bw.write<DWORD>(dbItemID);
    bw.write<DWORD>(amount);
    bw.write<DWORD>(0);         // ownerID (always send 0 so client bypasses local check)
    bw.write<BYTE>(0);  // bType
    bw.write<BYTE>(0);  // bFESocket
    bw.write<BYTE>(0);  // bChangeItem
    
    sMapDrop newDrop;
    newDrop.dwMapItemID = s_nextMapItemID;
    newDrop.dbItemID = dbItemID;
    newDrop.wRefID = 20423;
    newDrop.amount = amount;
    newDrop.ownerID = killerID;
    newDrop.dropTime = GetTickCount();
    newDrop.bType = 32;
    newDrop.wVisualID = 10050;
    newDrop.name = moneyName;
    newDrop.isMoney = true;
    newDrop.mapID = obj.dwMapID;
    newDrop.wPosX = dropX;
    newDrop.wPosY = dropY;
    memset(newDrop.nData, 0, sizeof(newDrop.nData));
    
    {
        std::lock_guard<std::mutex> lock(m_dropMutex);
        m_activeDrops[s_nextMapItemID] = newDrop;
    }
    
    std::vector<BYTE> finalBuf(4 + bw.buf.size());
    PACKET_HEADER* head = (PACKET_HEADER*)finalBuf.data();
    head->id = 0x420C;
    head->payloadSize = (WORD)bw.buf.size();
    memcpy(finalBuf.data() + 4, bw.buf.data(), bw.buf.size());
    EncryptPacket(finalBuf.data(), 0x42);
    BroadcastPacketToMap(obj.dwMapID, finalBuf);
    
    LOG("[Drop] Dropped " + std::to_string(amount) + " Money!");
}

void DropManager::GenerateDrops(DWORD killerID, const MonsterData& obj) {
    auto tplIt = g_NpcTemplates.find(obj.bPropType);
    if (tplIt == g_NpcTemplates.end()) return;
    
    // 任务系统：通知怪物击杀事件（杀怪计数与奇缘推进）
    DWORD actualCharID = killerID;
    if (actualCharID >= 800000000) {
        actualCharID -= 400000000;
    }
    DWORD dwNpcListID = 0;
    if (obj.dwObjectID >= 200000) {
        dwNpcListID = (obj.dwObjectID % 1000000) - 200000;
    }
    QuestManager::GetInstance().OnMonsterKilled(actualCharID, obj.bPropType, dwNpcListID);

    DWORD partyID = PartyManager::GetInstance().GetPartyID(actualCharID);
    if (partyID > 0) {
        auto members = PartyManager::GetInstance().GetMembers(partyID);
        for (const auto& m : members) {
            if (m.dwCharID != actualCharID && m.dwMapID == obj.dwMapID) {
                QuestManager::GetInstance().OnMonsterKilled(m.dwCharID, obj.bPropType, dwNpcListID);
            }
        }
    }

    // 获取 VIP 爆率加倍缩减系数
    int vipLevel = CharacterDB::GetInstance().GetVipLevel(killerID);
    double multiplier = 1.0;
    if (vipLevel >= 1 && vipLevel <= 5) {
        multiplier += vipLevel * 0.2; // VIP 1-5 分别提升 20%~100% 爆率
    }

    // 称号系统：累加激活且生效的称号掉宝率加成百分比(wDropPerc)
    int titleDropBonus = 0;
    std::string qTitleDrop = "SELECT COALESCE(SUM(t.wDropPerc), 0) "
                             "FROM CHAR_TITLE ct "
                             "INNER JOIN TITLE_TEMPLATE t ON ct.dwTitleID = t.dwTitleID "
                             "WHERE ct.dwCharID = " + std::to_string(killerID) + " AND ct.bActive = 1";
    DBHelper::GetInstance().ExecuteQuery(qTitleDrop, [&](SQLHSTMT hStmt) {
        SQLGetData(hStmt, 1, SQL_C_LONG, &titleDropBonus, 0, NULL);
    });
    
    if (titleDropBonus > 0) {
        multiplier += (titleDropBonus / 100.0);
    }
    
    // 计算并应用 VIP 与称号爆率提升
    int finalRootItem = obj.wRootItem;
    if (multiplier > 1.0 && finalRootItem > 0) {
        finalRootItem = (int)(finalRootItem / multiplier);
        if (finalRootItem < 1) finalRootItem = 1;
    }
    
    // ===== Item Drop (wRootItem = master gate, 1:N) =====
    if (obj.wRootItem > 0 && (rand() % finalRootItem == 0)) {
        
        // Try new Drop Group system first
        auto groupIt = m_dropGroups.find(obj.bPropType);
        if (groupIt != m_dropGroups.end() && !groupIt->second.empty()) {
            // New system: iterate all groups for this NPC type
            for (const auto& group : groupIt->second) {
                if (group.wDropRate == 0) continue;
                if (group.items.empty()) continue;
                
                // 应用 VIP 组爆率提升
                int finalDropRate = group.wDropRate;
                if (multiplier > 1.0 && finalDropRate > 0) {
                    finalDropRate = (int)(finalDropRate / multiplier);
                    if (finalDropRate < 1) finalDropRate = 1;
                }
                
                // Per-group 1/N probability
                if (rand() % finalDropRate != 0) continue;
                
                // Determine pick count: bMinDrop ~ bMaxDrop
                int picks = group.bMinDrop;
                if (group.bMaxDrop > group.bMinDrop)
                    picks += rand() % (group.bMaxDrop - group.bMinDrop + 1);
                
                // Calculate total weight
                int totalWeight = 0;
                for (const auto& item : group.items)
                    totalWeight += item.wWeight;
                if (totalWeight <= 0) continue;
                
                // Weighted random pick N items
                for (int n = 0; n < picks; n++) {
                    int roll = rand() % totalWeight;
                    int cumulative = 0;
                    for (const auto& item : group.items) {
                        cumulative += item.wWeight;
                        if (roll < cumulative) {
                            DropItemToMap(killerID, obj, item.dwItemID);
                            break;
                        }
                    }
                }
            }
        } else {
            // Legacy fallback: use NPC_ROOTITEM weighted random
            auto dropIt = m_rootItems.find(obj.bPropType);
            if (dropIt != m_rootItems.end() && !dropIt->second.empty()) {
                int totalWeight = 0;
                for (const auto& item : dropIt->second) totalWeight += item.wItemRatio;
                
                if (totalWeight > 0) {
                    int itemRoll = rand() % totalWeight;
                    int currentWeight = 0;
                    DWORD droppedItemID = 0;
                    
                    for (const auto& item : dropIt->second) {
                        currentWeight += item.wItemRatio;
                        if (itemRoll < currentWeight) {
                            droppedItemID = item.dwItemID;
                            break;
                        }
                    }
                    
                    if (droppedItemID > 0) {
                        DropItemToMap(killerID, obj, droppedItemID);
                    }
                }
            }
        }
    }
    
    // ===== Money Drop (wRootMoney, 1:N) =====
    int finalRootMoney = obj.wRootMoney;
    if (multiplier > 1.0 && finalRootMoney > 0) {
        finalRootMoney = (int)(finalRootMoney / multiplier);
        if (finalRootMoney < 1) finalRootMoney = 1;
    }
    if (obj.wRootMoney > 0 && (rand() % finalRootMoney == 0)) {
        DWORD moneyAmount = obj.bPropType * 10 + (rand() % 100);
        DropMoneyToMap(killerID, obj, moneyAmount);
    }
}

void DropManager::HandlePickup(SOCKET clientSocket, DWORD playerID, BYTE* payload, WORD size) {
    if (size < 21) return;
    DWORD mapID = *((DWORD*)(payload + 0));
    DWORD dbItemID = *((DWORD*)(payload + 4));
    WORD x = *((WORD*)(payload + 8));
    WORD y = *((WORD*)(payload + 10));
    DWORD mapItemID = *((DWORD*)(payload + 13)); 
    DWORD amount = *((DWORD*)(payload + 17));
    
    sMapDrop drop;
    {
        std::lock_guard<std::mutex> lock(m_dropMutex);
        auto it = m_activeDrops.end();
        for (auto i = m_activeDrops.begin(); i != m_activeDrops.end(); ++i) {
            if (i->second.dbItemID == dbItemID) {
                it = i;
                break;
            }
        }
        
        if (it == m_activeDrops.end()) {
            LOG("[DropManager] Pick failed, item not found or already picked! dbItemID: " + std::to_string(dbItemID));
            return;
        }
        
        // Owner-exclusive check: within first 25 seconds only the killer can pick up
        DWORD elapsed = GetTickCount() - it->second.dropTime;
        DWORD ownerCharID = it->second.ownerID;
        if (ownerCharID >= 800000000) ownerCharID -= 400000000;
        
        DWORD pickerCharID = playerID;
        if (pickerCharID >= 800000000) pickerCharID -= 400000000;

        if (elapsed < DROP_OWNER_EXCLUSIVE_MS && ownerCharID != 0 && ownerCharID != pickerCharID) {
            LOG("[DropManager] Pick denied: owner-exclusive period. ownerID=" + std::to_string(it->second.ownerID) + " playerID=" + std::to_string(playerID));
            SendSystemWarningChat(clientSocket, "\x5B\xCF\xB5\xCD\xB3\x5D\x20\xB8\xC3\xCE\xEF\xC6\xB7\xB4\xA6\xD3\xDA\xCB\xF9\xD3\xD0\xD5\xDF\xB1\xA3\xBB\xA4\xC6\xDA\xA3\xAC\xC4\xE3\xD4\xDD\xCA\xB1\xCE\xDE\xB7\xA8\xBC\xF1\xC8\xA1\xA1\xA3");
            return;
        }
        
        drop = it->second;
        m_activeDrops.erase(it); // Remove from map
    }
    
    LOG("[DropManager] Picked up item! RefID: " + std::to_string(drop.wRefID) + " Amount: " + std::to_string(drop.amount));
    
    if (drop.isMoney) {
        // Remove from map
        std::vector<BYTE> rmBuf(8);
        PACKET_HEADER* head = (PACKET_HEADER*)rmBuf.data();
        head->id = 0x4206; // CS_IM_REMOVELISTFROMMAP_ACK
        head->payloadSize = 4;
        *((DWORD*)(rmBuf.data() + 4)) = drop.dwMapItemID;
        EncryptPacket(rmBuf.data(), 0x42);
        if (g_MapInstances.count(mapID)) {
            g_MapInstances[mapID]->BroadcastPacketAOI(x, y, rmBuf);
        } else {
            BroadcastPacketToMap(mapID, rmBuf);
        }
        
        // Add money directly to database
        CharacterDB::GetInstance().AddMoney(playerID, drop.amount);
        
        // Fetch new balance
        INT64 currentMoney = (INT64)CharacterDB::GetInstance().GetMoney(playerID);
        
        std::vector<BYTE> moneyBuf(13);
        PACKET_HEADER* mHead = (PACKET_HEADER*)moneyBuf.data();
        mHead->id = 0x3B13; // CS_IF_CHARMONEY_ACK in 1080 client
        mHead->payloadSize = 9;
        *((INT64*)(moneyBuf.data() + 4)) = (INT64)currentMoney;
        moneyBuf[12] = 0; // bFreeUser
        EncryptPacket(moneyBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)moneyBuf.data(), moneyBuf.size(), 0);
        LOG("[DropManager] Picked up money. Sent new balance using 0x3B13.");
        
        // Send the system message for gaining money
        SystemMessage::SendHelpMessage(clientSocket, SystemMessage::MsgType::PICK_ITEM, "\xC1\xBD", drop.amount);
        
    } else {
        // Get new item's grid dimensions
        BYTE bcx = 1, bcy = 1;
        if (g_ItemTemplates.count(drop.wRefID)) {
            bcx = g_ItemTemplates[drop.wRefID].bCX;
            bcy = g_ItemTemplates[drop.wRefID].bCY;
        }
        
        BYTE freePos = 255;
        BYTE actualSackID = 1;
        for (BYTE tryID = 1; tryID <= 3; tryID++) {
            freePos = FindFreeSackPos(playerID, tryID, bcx, bcy);
            if (freePos != 255) {
                actualSackID = tryID;
                break;
            }
        }
        if (freePos == 255) {
            LOG("[DropManager] Bag full for player " + std::to_string(playerID));
            return;
        }
        
        // Calculate relative position within the sack page
        int startPos = (actualSackID == 1) ? 20 : (actualSackID == 2) ? 60 : 100;
        BYTE relativeSackPos = freePos - startPos;
        
        DWORD newDbItemID = 0;
        bool existsInDB = (drop.dbItemID > 0 && ItemDB::GetInstance().GetItemRefID(drop.dbItemID) > 0);
        
        if (existsInDB) {
            // Re-use the existing database item row!
            newDbItemID = drop.dbItemID;
            LOG("[DropManager] Restoring existing player dropped item dwItemID=" + std::to_string(newDbItemID));
        } else {
            // Create a brand new item for monster drops
            newDbItemID = ItemDB::GetInstance().CreateItemFromTemplate(drop.wRefID);
            if (newDbItemID == 0) {
                newDbItemID = rand() * rand(); // Extreme fallback
            } else {
                if (drop.bType < 10) {
                    ItemDB::GetInstance().InsertItemData(newDbItemID, drop.nData);
                }
            }
        }
        
        // Insert into SACKITEM via ItemDB
        ItemDB::GetInstance().AddToSack(playerID, freePos, newDbItemID);
        
        // Remove from map
        std::vector<BYTE> rmBuf(8);
        PACKET_HEADER* head = (PACKET_HEADER*)rmBuf.data();
        head->id = 0x4206; // CS_IM_REMOVELISTFROMMAP_ACK
        head->payloadSize = 4;
        *((DWORD*)(rmBuf.data() + 4)) = drop.dwMapItemID;
        EncryptPacket(rmBuf.data(), 0x42);
        if (g_MapInstances.count(mapID)) {
            g_MapInstances[mapID]->BroadcastPacketAOI(x, y, rmBuf);
        } else {
            BroadcastPacketToMap(mapID, rmBuf);
        }
        
        // Query full item data from DB to ensure all attributes and socket data are correctly initialized
        ItemDB::FullItemRow row;
        bool dbQuerySuccess = ItemDB::GetInstance().GetFullItemData(newDbItemID, row);
        
        std::vector<BYTE> bi;
        bi.resize(4); // header placeholder
        pushByte(bi, actualSackID);
        pushByte(bi, relativeSackPos);

        if (dbQuerySuccess) {
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

            // GetItemData serialization
            pushDWord(bi, newDbItemID); pushWord(bi, refid);
            pushByte(bi, type); pushByte(bi, kind); pushWord(bi, vis);
            std::string itemName(szName);
            if (itemName.empty() && g_ItemTemplates.count(refid)) itemName = g_ItemTemplates[refid].szName;
            pushWord(bi, (WORD)itemName.length());
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
                    pushDWord(bi, d1 == -9999 ? 0 : d1);
                    pushWord(bi, d2 == -9999 ? 0 : d2);
                    pushWord(bi, d3 == -9999 ? 0 : d3);
                    pushWord(bi, d4 == -9999 ? 0 : d4);
                    pushWord(bi, d5 == -9999 ? 0 : d5);
                }
                else if (type == 8) { for(int i=0;i<8;i++) pushByte(bi, 0); }
                else { pushByte(bi, 0); }
                if (type >= 1 && type <= 4) { pushByte(bi, dat19); pushByte(bi, dat20); pushByte(bi, dat21); pushWord(bi, dat25); }
            } else {
                switch (type) {
                    case 11: case 12: case 13: case 14: case 17: pushByte(bi, 0); pushWord(bi, d2); pushWord(bi, d3); break;
                    case 15: pushByte(bi, d1); pushWord(bi, d2); pushWord(bi, d3); pushByte(bi, 0); pushByte(bi, 0); break;
                    case 16: pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
                    case 18: pushByte(bi, 0); pushDWord(bi, 0); pushWord(bi, d2); pushWord(bi, d3); pushByte(bi, 0); break;
                    case 19: pushWord(bi, 0); pushWord(bi, 0); break;
                    case 20: pushByte(bi, 0); pushDWord(bi, 0); break;
                    case 21: { DWORD mid=nd2; sMugongTemplate* mg=MugongManager::GetInstance()->GetTemplate(mid); pushWord(bi, lvl); pushDWord(bi, mid); pushByte(bi, mg?mg->bType:0); pushByte(bi, mg?mg->bKind:0); pushByte(bi, 1); break; }
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
            pushWord(bi, (WORD)dat25); // wRebuithValue - 觉醒值(nData25)
        } else {
            // Extreme fallback: serialize manually using memory drop stats if DB query fails
            pushDWord(bi, newDbItemID); pushWord(bi, drop.wRefID);
            pushByte(bi, drop.bType); pushByte(bi, 0); pushWord(bi, drop.wVisualID);
            pushWord(bi, (WORD)drop.name.length());
            for (char ch : drop.name) pushByte(bi, ch);
            pushDWord(bi, 0); pushWord(bi, 0); pushByte(bi, 1);
            pushWord(bi, (WORD)drop.amount);
            
            if (drop.bType >= 1 && drop.bType <= 9) {
                pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0);
                pushByte(bi, 0);
                pushWord(bi, drop.nData[1]); pushWord(bi, drop.nData[3]);
                pushDWord(bi, 0); pushDWord(bi, 0); pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0);
                pushDWord(bi, 0); pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0);
                pushByte(bi, 0); pushByte(bi, 0);
                pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0);
                if (drop.bType == 9) {
                    pushDWord(bi, drop.nData[0]);
                    pushWord(bi, drop.nData[1]);
                    pushWord(bi, drop.nData[2]);
                    pushWord(bi, drop.nData[3]);
                    pushWord(bi, drop.nData[4]);
                }
                else if (drop.bType == 8) { for(int i=0;i<8;i++) pushByte(bi, 0); }
                else { pushByte(bi, 0); }
                if (drop.bType >= 1 && drop.bType <= 4) { pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushWord(bi, 0); }
            } else {
                switch (drop.bType) {
                    case 11: case 12: case 13: case 14: case 17: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                    case 15: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
                    case 16: pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
                    case 18: pushByte(bi, 0); pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushByte(bi, 0); break;
                    case 19: pushWord(bi, 0); pushWord(bi, 0); break;
                    case 20: pushByte(bi, 0); pushDWord(bi, 0); break;
                    case 21: pushWord(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 1); break;
                    case 22: {
                        DWORD nd2_val = 0; BYTE nd3_val = 0; WORD nd4_val = 0; WORD nd5_val = 0;
                        if (g_ItemTemplates.count(drop.wRefID)) {
                            nd2_val = g_ItemTemplates[drop.wRefID].nBasicData2;
                            nd3_val = (BYTE)g_ItemTemplates[drop.wRefID].nBasicData3;
                            nd4_val = (WORD)g_ItemTemplates[drop.wRefID].nBasicData4;
                            nd5_val = (WORD)g_ItemTemplates[drop.wRefID].nBasicData5;
                        }
                        pushDWord(bi, nd2_val); pushByte(bi, nd3_val); pushWord(bi, nd4_val); pushWord(bi, nd5_val); break;
                    }
                    case 23: pushDWord(bi, 0); pushDWord(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
                    case 25: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                    case 27: pushByte(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
                    case 29: pushWord(bi, 0); pushWord(bi, 0); break;
                    case 32: pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushDWord(bi, 0); break;
                    case 31: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
                    case 34: pushDWord(bi, 0); pushByte(bi, 0); break;
                }
            }
            pushWord(bi, 0); // wRebuithValue
        }
        
        PACKET_HEADER* addHead = (PACKET_HEADER*)bi.data();
        addHead->id = 0x420A; // CS_IM_ADDONSACK_ACK
        addHead->payloadSize = (WORD)(bi.size() - 4);
        
        EncryptPacket(bi.data(), 0x42);
        SafeSend(clientSocket, (const char*)bi.data(), bi.size(), 0);
        
        std::string itemName = "Unknown Item";
        if (g_ItemTemplates.count(drop.wRefID)) {
            itemName = g_ItemTemplates[drop.wRefID].szName;
        }
        SystemMessage::SendHelpMessage(clientSocket, SystemMessage::MsgType::PICK_ITEM, itemName, drop.amount);
    }
}

void DropManager::SendActiveDropsInAOI(SOCKET clientSocket, DWORD mapID, int posX, int posY, int lastUX, int lastUY) {
    std::vector<sMapDrop> nearbyDrops;
    {
        std::lock_guard<std::mutex> lock(m_dropMutex);
        for (const auto& pair : m_activeDrops) {
            const sMapDrop& drop = pair.second;
            if (drop.mapID == mapID) {
                int dx = (int)drop.wPosX - posX;
                int dy = (int)drop.wPosY - posY;
                bool inNew = (dx * dx + dy * dy <= 150 * 150);
                
                bool inOld = false;
                if (lastUX > 0 && lastUY > 0) {
                    int odx = (int)drop.wPosX - lastUX;
                    int ody = (int)drop.wPosY - lastUY;
                    inOld = (odx * odx + ody * ody <= 150 * 150);
                }
                
                if (inNew && !inOld) {
                    nearbyDrops.push_back(drop);
                }
            }
        }
    } // unlock m_dropMutex
    
    if (nearbyDrops.empty()) return;
    
    BufferWriter bw;
    bw.write<BYTE>(0); // bResult
    bw.write<WORD>((WORD)nearbyDrops.size()); // wItemNum
    
    for (const auto& drop : nearbyDrops) {
        bw.write<DWORD>(drop.mapID);
        bw.write<WORD>(drop.wPosX);
        bw.write<WORD>(drop.wPosY);
        bw.write<BYTE>(0); // bHeight
        bw.write<DWORD>(drop.dwMapItemID);
        
        bw.write<BYTE>(drop.bType);
        bw.write<WORD>(drop.wVisualID);
        bw.writeString(drop.name);
        
        bw.write<DWORD>(drop.dbItemID);
        bw.write<DWORD>(drop.amount);
        bw.write<DWORD>(0);             // ownerID (always send 0 so client bypasses local check)
        bw.write<BYTE>(0); // bType
        bw.write<BYTE>(0); // bFESocket
        bw.write<BYTE>(0); // bChangeItem
    }
    
    std::vector<BYTE> finalBuf(4 + bw.buf.size());
    PACKET_HEADER* head = (PACKET_HEADER*)finalBuf.data();
    head->id = 0x420C;
    head->payloadSize = (WORD)bw.buf.size();
    memcpy(finalBuf.data() + 4, bw.buf.data(), bw.buf.size());
    EncryptPacket(finalBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)finalBuf.data(), finalBuf.size(), 0);
}

void DropManager::CleanupExpiredDrops() {
    DWORD now = GetTickCount();
    
    // Collect expired drops under lock, then broadcast removal outside lock
    struct ExpiredDrop {
        DWORD dwMapItemID;
        DWORD mapID;
        WORD wPosX;
        WORD wPosY;
    };
    std::vector<ExpiredDrop> expired;
    
    {
        std::lock_guard<std::mutex> lock(m_dropMutex);
        for (auto it = m_activeDrops.begin(); it != m_activeDrops.end(); ) {
            DWORD elapsed = now - it->second.dropTime;
            if (elapsed >= DROP_LIFETIME_MS) {
                ExpiredDrop ed;
                ed.dwMapItemID = it->second.dwMapItemID;
                ed.mapID = it->second.mapID;
                ed.wPosX = it->second.wPosX;
                ed.wPosY = it->second.wPosY;
                expired.push_back(ed);
                
                // If it is an existing player dropped item, clean it up from database when it expires!
                if (!it->second.isMoney && it->second.dbItemID > 0 && ItemDB::GetInstance().GetItemRefID(it->second.dbItemID) > 0) {
                    ItemDB::GetInstance().DeleteItemCascade(it->second.dbItemID);
                }
                
                it = m_activeDrops.erase(it);
            } else {
                ++it;
            }
        }
    }
    
    // Broadcast REMOVELISTFROMMAP_ACK (0x4206) for each expired drop
    for (const auto& ed : expired) {
        std::vector<BYTE> rmBuf(8);
        PACKET_HEADER* head = (PACKET_HEADER*)rmBuf.data();
        head->id = 0x4206; // CS_IM_REMOVELISTFROMMAP_ACK
        head->payloadSize = 4;
        *((DWORD*)(rmBuf.data() + 4)) = ed.dwMapItemID;
        EncryptPacket(rmBuf.data(), 0x42);
        
        if (g_MapInstances.count(ed.mapID) && g_MapInstances[ed.mapID]) {
            g_MapInstances[ed.mapID]->BroadcastPacketAOI(ed.wPosX, ed.wPosY, rmBuf);
        } else {
            BroadcastPacketToMap(ed.mapID, rmBuf);
        }
    }
    
    if (!expired.empty()) {
        LOG("[DropManager] Cleaned up " + std::to_string(expired.size()) + " expired drops.");
    }
}

#include "DropManager.h"
#include "../DBHelper.h"
#include "../DB/GameDataDB.h"
#include "PlayerManager.h"
#include "../DB/ItemDB.h"
#include "../DB/CharacterDB.h"
#include "../Network/SystemMessage.h"
#include "MapInstance.h"
#include <iostream>

extern std::map<DWORD, CMapInstance*> g_MapInstances;

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
    bw.write<DWORD>(killerID);  // ownerID
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
    
    newDrop.nData[0] = tpl.nData1;  newDrop.nData[1] = tpl.nData2;
    newDrop.nData[2] = tpl.nData3;  newDrop.nData[3] = tpl.nData4;
    newDrop.nData[4] = tpl.nData5;  newDrop.nData[5] = tpl.nData6;
    newDrop.nData[6] = tpl.nData7;  newDrop.nData[7] = tpl.nData8;
    newDrop.nData[8] = tpl.nData9;  newDrop.nData[9] = tpl.nData10;
    
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
    bw.write<DWORD>(killerID);
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
    
    // ===== Item Drop (wRootItem = master gate, 1:N) =====
    if (obj.wRootItem > 0 && (rand() % obj.wRootItem == 0)) {
        
        // Try new Drop Group system first
        auto groupIt = m_dropGroups.find(obj.bPropType);
        if (groupIt != m_dropGroups.end() && !groupIt->second.empty()) {
            // New system: iterate all groups for this NPC type
            for (const auto& group : groupIt->second) {
                if (group.wDropRate == 0) continue;
                if (group.items.empty()) continue;
                
                // Per-group 1/N probability
                if (rand() % group.wDropRate != 0) continue;
                
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
    if (obj.wRootMoney > 0 && (rand() % obj.wRootMoney == 0)) {
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
        if (elapsed < DROP_OWNER_EXCLUSIVE_MS && it->second.ownerID != 0 && it->second.ownerID != playerID) {
            LOG("[DropManager] Pick denied: owner-exclusive period. ownerID=" + std::to_string(it->second.ownerID) + " playerID=" + std::to_string(playerID));
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
        
        // Insert into ITEM table via ItemDB
        DWORD newDbItemID = ItemDB::GetInstance().CreateItemFromTemplate(drop.wRefID);
        
        if (newDbItemID == 0) {
            newDbItemID = rand() * rand(); // Extreme fallback
        } else {
            if (drop.bType < 10) {
                // Insert the memory-rolled stats via ItemDB
                ItemDB::GetInstance().InsertItemData(newDbItemID, drop.nData);
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
        
        // Use BufferWriter for 0x420A (AddOnSack)
        BufferWriter bw;
        bw.write<BYTE>(actualSackID); // bSackID
        bw.write<BYTE>(relativeSackPos); // bSackPos
        
        // GetItemData format
        bw.write<DWORD>(newDbItemID);
        bw.write<WORD>(drop.wRefID);
        bw.write<BYTE>(drop.bType);
        bw.write<BYTE>(0); // bItemKind
        bw.write<WORD>(drop.wVisualID);
        bw.writeString(drop.name);
        bw.write<DWORD>(0); // dwPrice
        bw.write<WORD>(0); // wLevel
        bw.write<BYTE>(0); // bNeedCharType
        bw.write<WORD>((WORD)drop.amount); // wAmount
        
        // Append weapon/armor stats if needed (zeros for now to avoid crash if client reads it)
        if (drop.bType >= 1 && drop.bType <= 9) {
            bw.write<WORD>(0); // m_wNeedLevel
            bw.write<WORD>(0); // m_wNeedDex
            bw.write<WORD>(0); // m_wNeedStr
            bw.write<WORD>(0); // m_wNeedSus
            bw.write<WORD>(0); // m_wNeedVit
            bw.write<BYTE>(0); // m_bDecrDurRate
            bw.write<WORD>(0); // m_wCurDur
            bw.write<WORD>(0); // m_wMaxDur
            bw.write<WORD>(drop.nData[1]); // m_wAtkPwr
            bw.write<WORD>(drop.nData[3]); // m_wDefPwr
            bw.write<WORD>(0); // m_wAtkRating
            bw.write<WORD>(0); // m_wStkSpeed
            bw.write<WORD>(0); // m_wAtkRange
            bw.write<WORD>(drop.nData[5]); // m_wIncrHp
            bw.write<WORD>(0); // m_wIncrIp
            bw.write<WORD>(0); // m_wRestoreHp
            bw.write<WORD>(0); // m_wRestoreIp
            bw.write<WORD>(0); // m_wIncrCritical
            bw.write<BYTE>(0); // m_bRarity
            bw.write<BYTE>(0); // m_bStxType
            bw.write<BYTE>(0); // m_bLimitCnt
            bw.write<BYTE>(0); // m_bModifyCnt
            bw.write<BYTE>(0); // m_bRepairCnt
            bw.write<BYTE>(0); // m_bRepairDiscount
            
            if (drop.bType == 9) { // BONGIN
                bw.write<DWORD>(0); bw.write<WORD>(0); bw.write<WORD>(0); bw.write<WORD>(0); bw.write<WORD>(0);
            } else if (drop.bType == 8) { // SOCKET
                for(int i=0; i<8; i++) bw.write<BYTE>(0);
            } else {
                bw.write<BYTE>(0); // PuzzleType
            }
            
            if (drop.bType >= 1 && drop.bType <= 4) { // WEAPON, CLOTH, HAT, SHOE
                bw.write<BYTE>(0); bw.write<BYTE>(0); bw.write<BYTE>(0); // SocketItems
                bw.write<WORD>(0); // wRBSocketItem (RebuildValue)
            }
        } else {
            switch (drop.bType) {
                case 11: case 12: case 13: case 14: case 17: bw.write<BYTE>(0); bw.write<WORD>(0); bw.write<WORD>(0); break;
                case 15: bw.write<BYTE>(0); bw.write<WORD>(0); bw.write<WORD>(0); bw.write<BYTE>(0); bw.write<BYTE>(0); break;
                case 16: bw.write<WORD>(0); bw.write<BYTE>(0); bw.write<WORD>(0); bw.write<BYTE>(0); bw.write<WORD>(0); break;
                case 18: bw.write<BYTE>(0); bw.write<DWORD>(0); bw.write<WORD>(0); bw.write<WORD>(0); bw.write<BYTE>(0); break;
                case 19: bw.write<WORD>(0); bw.write<WORD>(0); break;
                case 20: bw.write<BYTE>(0); bw.write<DWORD>(0); break;
                case 21: bw.write<WORD>(0); bw.write<DWORD>(0); bw.write<BYTE>(0); bw.write<BYTE>(0); bw.write<BYTE>(0); break;
                case 22: bw.write<DWORD>(0); bw.write<BYTE>(0); bw.write<WORD>(0); bw.write<WORD>(0); break;
                case 23: bw.write<DWORD>(0); bw.write<WORD>(0); bw.write<WORD>(0); bw.write<BYTE>(0); bw.write<BYTE>(0); break;
                case 25: bw.write<BYTE>(0); bw.write<WORD>(0); bw.write<WORD>(0); break;
                case 27: bw.write<BYTE>(0); bw.write<DWORD>(0); bw.write<BYTE>(0); bw.write<BYTE>(0); bw.write<BYTE>(0); bw.write<BYTE>(0); bw.write<DWORD>(0); break;
                case 29: bw.write<WORD>(0); bw.write<WORD>(0); break;
                case 32: bw.write<WORD>(0); bw.write<WORD>(0); bw.write<WORD>(0); bw.write<DWORD>(0); break;
                case 31: bw.write<BYTE>(0); bw.write<WORD>(0); bw.write<WORD>(0); break;
                case 34: bw.write<DWORD>(0); bw.write<BYTE>(0); break;
            }
        }
        
        // wRebuithValue - client reads this WORD after GetItemData() in OnCS_IM_ADDONSACK_ACK
        bw.write<WORD>(0);
        
        std::vector<BYTE> addBuf(4 + bw.buf.size()); 
        head = (PACKET_HEADER*)addBuf.data();
        head->id = 0x420A;
        head->payloadSize = (WORD)bw.buf.size();
        memcpy(addBuf.data() + 4, bw.buf.data(), bw.buf.size());
        
        EncryptPacket(addBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)addBuf.data(), addBuf.size(), 0);
        
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
        bw.write<DWORD>(drop.ownerID);
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

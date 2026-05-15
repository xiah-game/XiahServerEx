#pragma once

#include <vector>
#include <map>
#include "../ServerCore.h"

// Legacy: kept for backward compatibility with NPC_ROOTITEM
struct sRootItem {
    DWORD dwItemID;
    WORD  wItemRatio;
};

// New: Drop Group Item
struct sDropGroupItem {
    DWORD dwItemID;     // wRefID in ITEMTEMPLATE
    WORD  wWeight;      // Weight within group
};

// New: Drop Group
struct sDropGroup {
    BYTE  bNpcType;
    DWORD dwGroupID;
    WORD  wDropRate;    // 1/N chance (1=always, 0=disabled)
    BYTE  bMinDrop;
    BYTE  bMaxDrop;
    std::vector<sDropGroupItem> items;
};

struct sMapDrop {
    DWORD dwMapItemID;
    DWORD dbItemID;
    WORD wRefID;
    DWORD amount;
    DWORD ownerID;
    DWORD dropTime;
    BYTE bType;
    WORD wVisualID;
    std::string name;
    bool isMoney;
    
    // Store rolled stats in memory before pickup!
    int nData[25];
};

class DropManager {
private:
    // Legacy: NPC_ROOTITEM (fallback for NPCs without drop groups)
    std::map<BYTE, std::vector<sRootItem>> m_rootItems;
    
    // New: Drop Group system
    std::map<BYTE, std::vector<sDropGroup>> m_dropGroups; // NpcType -> groups
    
    // Active drops on the map
    std::map<DWORD, sMapDrop> m_activeDrops; // key: dwMapItemID
    
    // Singleton instance
    static DropManager* s_instance;

public:
    static DropManager* GetInstance();
    
    // Load from NPC_ROOTITEM (legacy) and NPC_DROPGROUP/NPC_DROPGROUPITEM (new)
    void LoadDropTables();
    void LoadDropGroups();
    
    // Drop helpers (also used by ItemHandler for throw-item)
    void DropItemToMap(DWORD killerID, const sServerObject& obj, DWORD itemRefID, bool useRandomOffset = true);
    void DropMoneyToMap(DWORD killerID, const sServerObject& obj, DWORD amount);
    
    // Roll and generate drops for a killed monster
    void GenerateDrops(DWORD killerID, const sServerObject& deadMonster);
    
    // Process CS_IM_PICK_REQ (0x4201)
    void HandlePickup(SOCKET clientSocket, DWORD playerID, BYTE* payload, WORD size);
};

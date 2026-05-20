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

    // Position and Map fields for AOI checks
    DWORD mapID;
    WORD wPosX;
    WORD wPosY;
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
    void DropItemToMap(DWORD killerID, const MonsterData& obj, DWORD itemRefID, bool useRandomOffset = true);
    void DropMoneyToMap(DWORD killerID, const MonsterData& obj, DWORD amount);
    
    // Roll and generate drops for a killed monster
    void GenerateDrops(DWORD killerID, const MonsterData& deadMonster);
    
    // Process CS_IM_PICK_REQ (0x4201)
    void HandlePickup(SOCKET clientSocket, DWORD playerID, BYTE* payload, WORD size);

    // Send active drops in the player's AOI (supports both entry full sync and move delta sync)
    void SendActiveDropsInAOI(SOCKET clientSocket, DWORD mapID, int posX, int posY, int lastUX = 0, int lastUY = 0);
};

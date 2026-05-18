#pragma once

#include <map>
#include <vector>
#include <mutex>
#include "../ServerCore.h"
#include <unordered_set>

// Phase 1: Map Instance without global lock
class CMapInstance {
public:
    CMapInstance(DWORD mapID, int width, int height, const std::vector<BYTE>& collisionGrid);
    ~CMapInstance();

    // Map basic info
    DWORD GetMapID() const { return m_dwMapID; }
    int GetWidth() const { return m_width; }
    int GetHeight() const { return m_height; }

    // ==========================================
    // 实体管理 (Entities Management)
    // ==========================================
    void AddPlayer(const PlayerData& player);
    void RemovePlayer(DWORD dwObjectID);
    PlayerData* GetPlayer(DWORD dwObjectID);
    std::map<DWORD, PlayerData>& GetPlayers() { return m_players; }

    void AddMonster(const MonsterData& monster);
    void RemoveMonster(DWORD dwObjectID);
    MonsterData* GetMonster(DWORD dwObjectID);
    std::map<DWORD, MonsterData>& GetMonsters() { return m_monsters; }

    // ==========================================
    // 地图心跳与 AI (Map Tick & AI)
    // ==========================================
    // Replaces global MonsterAIThread
    void Update(DWORD tick);

    // ==========================================
    // AOI (Area of Interest) Grid System
    // ==========================================
    static const int GRID_SIZE = 100;
    
    // Retrieves objects in the 9-grid area (current + 8 surrounding)
    std::vector<PlayerData*> GetPlayersInAOI(int x, int y);
    std::vector<MonsterData*> GetMonstersInAOI(int x, int y);
    
    // Updates object position in the grid. Call this when wPosX/wPosY changes!
    void UpdatePlayerGrid(DWORD dwObjectID, int oldX, int oldY, int newX, int newY);
    void UpdateMonsterGrid(DWORD dwObjectID, int oldX, int oldY, int newX, int newY);

    // ==========================================
    // 网络同步 (Network Broadcasting)
    // ==========================================
    // Phase 1 broadcast to all in map, later switch to AOI
    void BroadcastPacket(const std::vector<BYTE>& packet);
    // Broadcast to AOI
    void BroadcastPacketAOI(int x, int y, const std::vector<BYTE>& packet, SOCKET excludeSocket = INVALID_SOCKET);
    void BroadcastPacketAOI_NoLock(int x, int y, const std::vector<BYTE>& packet, SOCKET excludeSocket = INVALID_SOCKET);

    // Mutex for external thread-safe access
    std::mutex& GetMutex() { return m_mapMutex; }

private:
    DWORD m_dwMapID;
    int m_width;
    int m_height;
    std::vector<BYTE> m_collisionGrid;

    // Map-specific lock (replaces g_ActivePlayersMutex)
    std::mutex m_mapMutex;

    // Entity list within this map
    std::map<DWORD, PlayerData> m_players;
    std::map<DWORD, MonsterData> m_monsters;

    // AOI Grid Data
    int m_gridCols;
    int m_gridRows;
    std::vector<std::vector<DWORD>> m_playerGrid;
    std::vector<std::vector<DWORD>> m_monsterGrid;

    int GetGridIndex(int x, int y) const;
    void RemoveFromGrid(std::vector<std::vector<DWORD>>& grid, int gridIdx, DWORD dwObjectID);
    void AddToGrid(std::vector<std::vector<DWORD>>& grid, int gridIdx, DWORD dwObjectID);

    // Internal AI helpers
    void ProcessMonsterAI(DWORD tick, MonsterData& obj);
    void ProcessBuffs(DWORD tick);
    void InterpolatePlayerPositions(DWORD tick);

public:
    // Pending sync broadcasts (filled under m_mapMutex, sent via SessionMgr outside lock)
    struct PendingSyncMove {
        DWORD dwObjectID;
        WORD wPosX, wPosY;
        BYTE bHeight;
        WORD wDesX, wDesY;
        BYTE bDesH;
        WORD wDirection;
        BYTE bSpeed;
    };
    std::vector<PendingSyncMove> m_pendingSyncs;
    DWORD m_lastSyncBroadcast = 0;
};

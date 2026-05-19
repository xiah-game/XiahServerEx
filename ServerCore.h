#pragma once

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <sql.h>
#include <sqlext.h>
#include <sqltypes.h>
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <mutex>
#include <fstream>
#include <winsock2.h>

int SafeSend(SOCKET s, const char* buf, int len, int flags);
void RemoveSocketMutex(SOCKET s);

struct sChannelInfo {
    int id;
    std::string name;
    std::string desc;
    int capacity;
    int pvp;
    int age;
};

struct sServerConfig {
    std::string dbServer;
    std::string dbUser;
    std::string dbPassword;
    std::string dbGame;
    std::string dbAccount;
    std::string dbServerInfo;
    std::string dbLog;
    std::string externalIP;
    std::string internalIP;
    WORD authPort;
    WORD unitPort;
    
    std::map<int, WORD> channelPorts;
    std::string realmName;
    std::vector<sChannelInfo> cachedChannels;

    std::string GetConnectionString(const std::string& dbName) const {
        return "Driver={SQL Server};Server=" + dbServer + ";Database=" + dbName + ";Uid=" + dbUser + ";Pwd=" + dbPassword + ";";
    }
};

extern sServerConfig g_Config;
void LoadConfig();

struct sFunctionalNpcItem {
    DWORD dwItemID;
    DWORD dwPrice;
    WORD wAmount;
    BYTE bRarity;
    BYTE bPos;
    BYTE bSackCnt;
};

// ============================================================
// Entity Type Hierarchy (Phase 2 refactor)
// EntityBase → PlayerData / MonsterData
// ============================================================

struct EntityBase {
    DWORD dwObjectID = 0;
    BYTE  bObjectType = 0;   // 1=Player, 3=Monster, 5=FuncNPC
    DWORD dwMapID = 0;
    WORD  wPosX = 0;
    WORD  wPosY = 0;
    float fPosX = 0.0f;
    float fPosY = 0.0f;
    BYTE  bHeight = 1;
    BYTE  bPropType = 0;     // bCharType for players, bNpcType for monsters
    std::string szName;
    DWORD dwHpCur = 0;
    DWORD dwHpMax = 0;
    WORD  wWalkSpeed = 11;
    WORD  wLevel = 1;
    DWORD dwDeadTime = 0;
    WORD  wLastUpdateX = 0;  // AoI distance tracking
    WORD  wLastUpdateY = 0;
};

struct PlayerData : EntityBase {
    // Base stats
    WORD  wStr = 10, wDex = 10, wVit = 10, wInt = 10;
    // Weapon stats
    DWORD wWepAtk = 0, wWepDef = 0, wWepMag = 0;
    // IP (Mana)
    WORD  wIpCur = 0, wIpMax = 0;
    // Equipment bonuses
    WORD  wPlusSpeed = 0, wCritical = 0;
    WORD  wEquipHp = 0, wEquipIp = 0;
    WORD  wEquipRestoreHp = 0, wEquipRestoreIp = 0;
    DWORD dwLastRegenTime = 0;
    DWORD dwInvulnerableUntil = 0;  // Death/respawn protection
    // Movement state
    bool  bIsMoving = false;
    WORD  wMoveDesX = 0, wMoveDesY = 0;
    BYTE  bMoveDesH = 0;
    WORD  wMoveDirection = 0;
    BYTE  bMoveState = 0;
    DWORD dwLastMoveTime = 0;
    // Dynamic stat caches (RecalculateStats)
    DWORD dwTotalAtk = 0, dwTotalDef = 0, dwTotalHit = 0, dwTotalDodge = 0;
    // Buffs
    struct sActiveBuff {
        DWORD dwMugongID;
        BYTE bLevel;
        DWORD dwEndTime;
        bool bIsDebuff;
    };
    std::map<DWORD, sActiveBuff> activeBuffs;
    // Skills
    std::map<DWORD, BYTE> learnedMugongs;

    PlayerData() { bObjectType = 1; }
};

struct MonsterData : EntityBase {
    // Spawn
    WORD  wSpawnX = 0, wSpawnY = 0, wSpawnRange = 0;
    WORD  wDestX = 0, wDestY = 0;
    WORD  wWanderRange = 0;
    // Combat
    WORD  wAtkSpeed = 9;
    DWORD wWepAtk = 0;  // Monster base attack power
    WORD  wMeleeAtkRange = 10, wShotAtkRange = 0;
    WORD  wSightRange = 0;
    WORD  wAtkRatio = 0, wAvoidRatio = 0;
    DWORD dwTargetID = 0;
    DWORD dwLastAttackTime = 0;
    DWORD dwMovePattern = 0, dwAttackPattern = 0;
    DWORD dwExp = 0;
    // Movement tracking
    WORD  wLastSentDestX = 0, wLastSentDestY = 0;
    // Display
    BYTE  bGroupOrder = 0;
    BYTE  bWalkSpeedByte = 8;
    // Drop rates
    WORD  wRootItem = 0, wRootMoney = 0, wRootRes = 0, wRootBook = 0;
    DWORD dwLastHealTime = 0;
    // NPC skills
    std::map<DWORD, DWORD> mugongLastCastTime;
    // Functional NPC items (bObjectType=5 only)
    std::vector<sFunctionalNpcItem> npcItems;

    MonsterData() { bObjectType = 3; }
};

// Backward-compat alias — will be removed after all handlers are migrated
typedef PlayerData sServerObject;

struct sNpcMugong {
    DWORD dwMugongID;
    BYTE bMugongLevel;
    BYTE bSelectID;
    WORD wSelectParam1;
    WORD wSelectParam2;
    WORD wAttackRange;
    sNpcMugong() : dwMugongID(0), bMugongLevel(0), bSelectID(0), wSelectParam1(0), wSelectParam2(0), wAttackRange(0) {}
};

struct sNpcTemplate {
    BYTE bType;
    BYTE bLevel;
    BYTE bWalkSpeed; // Client-facing walk speed byte
    std::string szName;
    DWORD dwHpInit;
    DWORD dwPwrInit;
    DWORD dwDefInit;
    DWORD dwExpInit;
    WORD wAtkRatio;
    WORD wAvoidRatio;
    WORD wHealPoint;
    BYTE bIdleRatio;
    WORD wWalkSpeed100;
    WORD wSightRangeInit;
    WORD wWanderRangeInit;
    WORD wMeleeAtkRangeInit;
    WORD wShotAtkRangeInit;
    WORD wRegen; // AI: Respawn time in seconds
    WORD wAtkInterval; // Attack cooldown in ms (default 1500)
    WORD wStaggerTime; // Hit stagger duration in ms (default 500)
    std::vector<sNpcMugong> mugongs;
};

struct sWorldMap {
    DWORD dwMapID;
    int width;
    int height;
    std::vector<BYTE> collisionGrid;
    sWorldMap() : dwMapID(0), width(0), height(0) {}
};

class CMapInstance;


struct sItemTemplate {
    WORD wRefID;
    BYTE bType;
    BYTE bKind;
    WORD wVisualID;
    std::string szName;
    DWORD dwCost;
    WORD wLevel;
    BYTE bCharType;
    WORD wAmount;
    BYTE bCX;
    BYTE bCY;
    int nBasicData1, nBasicData2, nBasicData3, nBasicData4, nBasicData5;
    int nData1, nData2, nData3, nData4, nData5, nData6, nData7, nData8, nData9, nData10, nData13;
};

struct sLevelTemplate {
    WORD wLevel;
    BYTE bCharType;
    long long int dwNeedExp;
    BYTE bSp;
    BYTE bTp;
};

#include <map>


extern std::map<BYTE, sNpcTemplate> g_NpcTemplates;
extern std::map<DWORD, sWorldMap> g_WorldMaps; // Keep for collision loading temporarily
extern std::map<DWORD, CMapInstance*> g_MapInstances; // NEW: Replaces globals

extern std::map<WORD, sItemTemplate> g_ItemTemplates;
extern std::map<WORD, std::map<BYTE, sLevelTemplate>> g_LevelTemplates;

struct sRebuildConfig {
    int wAttr=0, sAttr=0, lMulti=0, baseRate=0, breakChance=0;
};
extern std::map<int, sRebuildConfig> g_RebuildConfig;



void LOG(const std::string& msg);
void EncryptPacket(BYTE* pHead, BYTE bKey);
void DecryptPacket(BYTE* pHead, BYTE bKey);
void LoadWorldObjects();
void LoadGameData();

#pragma pack(push, 1)
struct PACKET_HEADER {
    WORD id;
    WORD payloadSize;
};
#pragma pack(pop)

void LOG(const std::string& msg);
void EncryptPacket(BYTE* pHead, BYTE bKey);
void DecryptPacket(BYTE* pHead, BYTE bKey);
void LoadWorldObjects();

// Inventory Grid Utility: Find first free slot in a 6x6 backpack page
// sackID: 1=page1(pos 20-55), 2=page2(pos 60-95), 3=page3(pos 100-135)
// Returns absolute bSackPos, or 255 if no space
BYTE FindFreeSackPos(DWORD charID, BYTE sackID, BYTE bCX, BYTE bCY);

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

struct sServerObject {
    DWORD dwObjectID;
    BYTE  bObjectType;
    DWORD dwMapID;
    WORD  wPosX;
    WORD  wPosY;
    float fPosX;
    float fPosY;
    BYTE  bHeight;
    BYTE  bPropType;
    WORD  wSpawnX;
    WORD  wSpawnY;
    WORD  wDestX;
    WORD  wDestY;
    WORD  wWanderRange;
    WORD  wAtkSpeed;
    WORD  wAtkRange;
    WORD  wStr;
    WORD  wDex;
    WORD  wVit;
    WORD  wInt;
    DWORD wWepAtk;
    DWORD wWepDef;
    DWORD wWepMag;
    DWORD dwHpCur;
    DWORD dwHpMax;
    WORD wIpCur;
    WORD wIpMax;
    WORD wPlusSpeed;
    WORD wCritical;
    WORD wEquipHp;   // Equipment bonus to max HP (from nData9)
    WORD wEquipIp;   // Equipment bonus to max IP (from nData10)
    WORD wEquipRestoreHp; // Equipment HP regen per tick (from nData11)
    WORD wEquipRestoreIp; // Equipment IP regen per tick (from nData12)
    DWORD dwLastRegenTime; // Timestamp of last player HP/IP regen tick
    DWORD dwTargetID;
    DWORD dwLastAttackTime;
    DWORD dwDeadTime; // AI: Timestamp when the monster died
    DWORD dwInvulnerableUntil; // Player: Tick until which player cannot be targeted (death/respawn protection)
    DWORD dwMovePattern;
    DWORD dwAttackPattern;
    std::string szName;
    WORD wWalkSpeed;
    WORD wLastSentDestX;
    WORD wLastSentDestY;
    WORD wSpawnRange; // AI: For respawning within a radius
    WORD wSightRange; // AI: Calculated Sight Range (Init + Inc)
    WORD wMeleeAtkRange; // AI: Calculated Melee Range (Init + Inc)
    WORD wShotAtkRange;  // AI: Calculated Ranged Attack Range (Init + Inc)
    DWORD dwExp;      // Exp rewarded on death (Init + Inc)
    WORD wLevel;      // NPC Level (NPCLIST override or NPCTEMPLATE fallback)
    WORD wAtkRatio;   // Hit ratio (Init + Inc)
    WORD wAvoidRatio; // Dodge ratio (Init + Inc)
    BYTE bGroupOrder; // Group order (affects client scale, = bOrderID)
    BYTE bWalkSpeedByte; // Walk speed byte for client packet (from NPCTEMPLATE.bWalkSpeed)
    WORD wRootItem;   // AI: Drop chance for Items (0-10000)
    WORD wRootMoney;  // AI: Drop chance for Gold (0-10000)
    WORD wRootRes;    // AI: Drop chance for Resources (0-10000)
    WORD wRootBook;   // AI: Drop chance for Books (0-10000)
    DWORD dwLastHealTime; // AI: Timestamp of last HP regeneration
    WORD wLastUpdateX;    // Tracking: AoI distance checking
    WORD wLastUpdateY;    // Tracking: AoI distance checking
    DWORD dwLastMoveTime; // Tracking: Time of last movement for wDiffTime interpolation
    
    // Movement state tracking for AOI entry re-sync
    bool  bIsMoving;
    WORD  wMoveDesX;
    WORD  wMoveDesY;
    BYTE  bMoveDesH;
    WORD  wMoveDirection;
    BYTE  bMoveState;
    
    // Dynamic Stat Caches (Calculated via RecalculateStats)
    DWORD dwTotalAtk;
    DWORD dwTotalDef;
    DWORD dwTotalHit;
    DWORD dwTotalDodge;

    struct sActiveBuff {
        DWORD dwMugongID;
        BYTE bLevel;
        DWORD dwEndTime;
        bool bIsDebuff;
    };
    std::map<DWORD, sActiveBuff> activeBuffs; // Tracks currently active buffs
    
    std::map<DWORD, DWORD> mugongLastCastTime; // AI: Tracks cooldowns for each skill
    std::map<DWORD, BYTE> learnedMugongs; // Player: Tracks learned MugongID -> Level
    std::vector<sFunctionalNpcItem> npcItems;
    sServerObject(): dwObjectID(0), bObjectType(0), dwMapID(0), wPosX(0), wPosY(0), fPosX(0.0f), fPosY(0.0f), bHeight(1), bPropType(0), wSpawnX(0), wSpawnY(0), wDestX(0), wDestY(0), wWanderRange(0), wAtkSpeed(9), wMeleeAtkRange(10), wShotAtkRange(0), wStr(10), wDex(10), wVit(10), wInt(10), wWepAtk(0), wWepDef(0), wWepMag(0), dwHpCur(0), dwHpMax(0), wIpCur(0), wIpMax(0), wPlusSpeed(0), wCritical(0), wEquipHp(0), wEquipIp(0), wEquipRestoreHp(0), wEquipRestoreIp(0), dwLastRegenTime(0), dwTargetID(0), dwLastAttackTime(0), dwDeadTime(0), dwInvulnerableUntil(0), dwMovePattern(0), dwAttackPattern(0), wWalkSpeed(11), wLastSentDestX(0), wLastSentDestY(0), wSpawnRange(0), wSightRange(0), dwExp(0), wLevel(1), wAtkRatio(0), wAvoidRatio(0), bGroupOrder(0), bWalkSpeedByte(8), wRootItem(0), wRootMoney(0), wRootRes(0), wRootBook(0), dwLastHealTime(0), wLastUpdateX(0), wLastUpdateY(0), dwLastMoveTime(0), dwTotalAtk(0), dwTotalDef(0), dwTotalHit(0), dwTotalDodge(0), bIsMoving(false), wMoveDesX(0), wMoveDesY(0), bMoveDesH(0), wMoveDirection(0), bMoveState(0) {}
};

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

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
    int requiredClientVersion = 1081; // 客户端最低版本号，低于此版本拒绝登录
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
    DWORD dwOwnerID = 0;      // 所有人ObjectID / CharID (用于分身/召唤兽结算)
    DWORD dwBunsinEndTime = 0;// 分身过期时间戳
    DWORD wIpCur = 0, wIpMax = 0; // 统一实体内功 IP 属性支持，使得怪物亦拥有 IP 数据载体
};

struct PlayerData : EntityBase {
    // Base stats
    WORD  wStr = 10, wDex = 10, wVit = 10, wInt = 10;
    // Weapon stats
    DWORD wWepAtk = 0, wWepDef = 0, wWepMag = 0;
    // Equipment bonuses
    WORD  wPlusSpeed = 0, wCritical = 0;
    WORD  wEquipHp = 0;
    DWORD wEquipIp = 0;
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
        DWORD dwMugongID = 0;
        BYTE bLevel = 0;
        DWORD dwEndTime = 0;
        bool bIsDebuff = false;
        DWORD dwLastTickTime = 0; // DoT 上次 tick 时间（bKind=19/21 用）
        DWORD dwSnapshotAtk = 0;  // DoT 施放瞬间攻击力快照（bKind=21 用）
        DWORD dwCasterID = 0;     // 施法者/攻击者 ID（DoT 同步伤害数字用）
    };
    std::map<DWORD, sActiveBuff> activeBuffs;
    // Skills
    std::map<DWORD, BYTE> learnedMugongs;
    // Personal Shop
    DWORD dwShopID = 0;
    BYTE  bShopStatus = 0;        // 0=not selling, 1=shop open
    std::string strShopName;
    std::string strShopDescription;

    // Munpa / Sect properties
    DWORD dwMunpaID = 0;
    DWORD dwMunpaOrder = 0;
    std::string szMunpaName;
    std::string szMunpaNickName;
    DWORD dwMunpaMarkID = 0;

    // 武功攻击与物理PK保护选项：0 = 保护所有角色, 1 = 保护本门派成员, 2 = 无对象/自由PK
    BYTE  bSafeMode = 0;

    // 分身标记：true 表示该 PlayerData 是一个分身实体，不是真实玩家
    // 怪物 AI 可攻击，但战斗系统不向其发送 socket 包
    bool  bIsBunsin = false;

    // 分身模型与外观：bNpcType 对应客户端 NPC_TYPE 表，251=剑影分身
    BYTE  bNpcType = 0;
    WORD  wBunsinVisualID[6] = {0}; // 复制主人装备外观

    // 觉醒次数：0=未觉醒, 1~6=觉醒, 7~12=真觉醒（对应客户端 m_bRebirth）
    BYTE  bRebirth = 0;

    // Visual equipment and Fame sync
    DWORD dwFame = 0;
    WORD  wVisualID[9] = {0};
    DWORD dwPetTargetObjectID = 0; // 战宠/分身当前锁定的攻击怪物目标
    DWORD dwLastPetAttackTime = 0; // 战宠/分身上一次协同攻击时间戳
    DWORD dwLastPetMoveTime = 0;   // 战宠/分身上一次跟随移动时间戳
    bool  bNeedTamingAck = false;  // 刚捕捉成功需下发驯服ACK标志
    BYTE  bRarity[9] = {0};
    BYTE  bStxType[9] = {0};
    BYTE  bNeedCharType[9] = {0};

    // 五行内存临时状态 (方案A)
    BYTE  bCurFiveElm = 0;
    BYTE  bFELevel = 0;
    DWORD dwLastFiveElmChangeTime = 0;
    WORD  wFiveElmPoint = 0;
    WORD  wFiveElmPointCnt = 0;
    DWORD dwFiveElmPower = 0;
    DWORD dwFiveElmGauge = 0;
    WORD  wMetalExp = 0;
    WORD  wWoodExp = 0;
    WORD  wWaterExp = 0;
    WORD  wFireExp = 0;
    WORD  wEarthExp = 0;
    // 暴气临时内存状态
    bool  bSpiritActive = false;
    DWORD dwSpiritEndTime = 0;

    // 五行二期首饰特技与等级对抗缓存
    bool  bIgnoreDefFiveElm = false; // 忽略五行防御
    bool  bIgnoreFiveElm = false;    // 忽略被克制折损

    BYTE GetFiveElmPassiveLevel() const {
        if (bCurFiveElm < 1 || bCurFiveElm > 5) return 0;
        DWORD mugID = 150 + bCurFiveElm - 1;
        auto it = learnedMugongs.find(mugID);
        if (it != learnedMugongs.end()) return it->second;
        return 0;
    }

    // 技能CD跟踪：技能ID → 上次释放时间(GetTickCount)
    std::map<DWORD, DWORD> mugongLastCastTime;

    PlayerData() { 
        bObjectType = 1; 
        memset(wVisualID, 0, sizeof(wVisualID));
        memset(bRarity, 0, sizeof(bRarity));
        memset(bStxType, 0, sizeof(bStxType));
        memset(bNeedCharType, 0, sizeof(bNeedCharType));
        bCurFiveElm = 0;
        bFELevel = 0;
        dwLastFiveElmChangeTime = 0;
        wFiveElmPoint = 0;
        wFiveElmPointCnt = 0;
        dwFiveElmPower = 0;
        dwFiveElmGauge = 0;
        wMetalExp = 0;
        wWoodExp = 0;
        wWaterExp = 0;
        wFireExp = 0;
        wEarthExp = 0;
        bSpiritActive = false;
        dwSpiritEndTime = 0;
        bIgnoreDefFiveElm = false;
        bIgnoreFiveElm = false;
    }
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
    DWORD dwChaseStartTime = 0;   // 追击起跑时刻，用于冲锋到达时触发起手无CD攻击
    DWORD dwLastWanderTime = 0;   // 巡逻漫游与发呆计时器（与攻击CD彻底解耦）
    DWORD dwMovePattern = 0, dwAttackPattern = 0;
    DWORD dwExp = 0;
    // Movement tracking
    WORD  wLastSentDestX = 0, wLastSentDestY = 0;
    WORD  wLastSentPosX = 0, wLastSentPosY = 0;
    DWORD dwLastMoveSendTime = 0; // 移动包发送时间门控：防止过频发包导致客户端"小碎步"动画抽搐
    WORD  wLastSentDirection = 0; // 上次发包时的方向角度：用于方向变化 >25° 判定
    // Display
    BYTE  bGroupOrder = 0;
    BYTE  bWalkSpeedByte = 8;
    BYTE  bRunSpeedByte = 10;
    WORD  wWalkSpeed100 = 400;
    WORD  wRunSpeed100 = 650;
    // Drop rates
    WORD  wRootItem = 0, wRootMoney = 0, wRootRes = 0, wRootBook = 0;
    DWORD dwLastHealTime = 0;
    bool  bIsReturning = false; // Leash return state
    DWORD dwReturnStartTime = 0; // Timestamp when leash return started, used for anti-stuck fallback
    bool  bInAttackRange = false; // Hysteresis state for attack range to prevent high-frequency vibration
    // NPC skills
    std::map<DWORD, DWORD> mugongLastCastTime;
    // Functional NPC items (bObjectType=5 only)
    std::vector<sFunctionalNpcItem> npcItems;

    // Active Buffs / Debuffs on monsters
    std::map<DWORD, PlayerData::sActiveBuff> activeBuffs;

    // 五行属性与增量经验
    BYTE  bFiveElm = 0;
    WORD  wIncFiveElmExp = 0;

    MonsterData() { bObjectType = 3; bIsReturning = false; bFiveElm = 0; wIncFiveElmExp = 0; }
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
    BYTE bRunSpeed = 10; // Client-facing run speed byte
    WORD wRunSpeed100 = 0; // Run speed in grids/sec * 100
    std::vector<sNpcMugong> mugongs;

    // 五行属性与经验
    BYTE bFiveElm = 0;
    WORD wFiveElmExp = 0;

    sNpcTemplate() : bType(0), bLevel(1), bWalkSpeed(8), bRunSpeed(10), dwHpInit(0), dwPwrInit(0), dwDefInit(0), dwExpInit(0),
                     wAtkRatio(0), wAvoidRatio(0), wHealPoint(0), bIdleRatio(50), wWalkSpeed100(0), wRunSpeed100(0),
                     wSightRangeInit(0), wWanderRangeInit(0), wMeleeAtkRangeInit(0), wShotAtkRangeInit(0),
                     wRegen(10), wAtkInterval(1500), wStaggerTime(500), bFiveElm(0), wFiveElmExp(0) {}
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
    int nData1, nData2, nData3, nData4, nData5, nData6, nData7, nData8, nData9, nData10, nData11, nData12, nData13;
    int nData14, nData15, nData16, nData17, nData18, nData19, nData20, nData21, nData22, nData23, nData24, nData25;
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

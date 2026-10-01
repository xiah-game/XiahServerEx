#include "ExpSystem.h"
#include "../ServerCore.h"
#include "../DB/CharacterDB.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../DBHelper.h"
#include "../DB/ItemDB.h"
#include "../Handlers/PartyHandler.h"
#include "PlayerManager.h"
#include "PetLevelExpManager.h"
#include <cmath>
#include <set>
#include <algorithm>

extern std::map<DWORD, CMapInstance*> g_MapInstances;

bool GrantExpToPlayer(DWORD dwCharID, DWORD dwIncrExp, DWORD dwFiveElmExpGained) {
    if (dwIncrExp == 0) return false;
    
    SOCKET clientSocket = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
    if (clientSocket == INVALID_SOCKET) return false;
    
    // Fetch EXP data via CharacterDB abstraction
    CharacterDB::ExpData expData;
    if (!CharacterDB::GetInstance().GetExpData(dwCharID, expData)) return false;
    
    long long int oldExp = expData.dwExp, newExp = 0;
    WORD wLevel = expData.wLevel;
    DWORD dwTotalTp = expData.dwTotalTp;
    WORD wRemainTp = expData.wRemainTp, wRemainSp = expData.wRemainSp;
    DWORD dwTotalSp = expData.dwTotalSp;
    BYTE bCharType = expData.bCharType;
    BYTE bLevelUp = 0, bTpUp = 0;
    
    // 检查并叠加当前正在生效的所有经验特权加成
    int totalBonusPercent = 0; // 累计经验加成比率 (例如: 千里倍A +200%, 四倍卡 +400%)
    std::string qExpBuff = 
        "SELECT COALESCE(SUM(p.wValue), 0) "
        "FROM CHAR_PREMIUM c "
        "INNER JOIN ITEM_PREMIUM p ON c.wRefID = p.wRefID "
        "WHERE c.dwCharID = " + std::to_string(dwCharID) + " "
        "  AND c.dateEnd > GETDATE() "
        "  AND p.bType IN (0, 1, 8)"; // 经验类 Buff
        
    DBHelper::GetInstance().ExecuteQuery(qExpBuff, [&](SQLHSTMT hStmt) {
        SQLGetData(hStmt, 1, SQL_C_LONG, &totalBonusPercent, 0, NULL);
    });

    // 称号系统：累加激活且生效的称号经验加成百分比(wExpPerc)
    int titleExpBonus = 0;
    std::string qTitleExp = "SELECT COALESCE(SUM(t.wExpPerc), 0) "
                            "FROM CHAR_TITLE ct "
                            "INNER JOIN TITLE_TEMPLATE t ON ct.dwTitleID = t.dwTitleID "
                            "WHERE ct.dwCharID = " + std::to_string(dwCharID) + " AND ct.bActive = 1";
    DBHelper::GetInstance().ExecuteQuery(qTitleExp, [&](SQLHSTMT hStmt) {
        SQLGetData(hStmt, 1, SQL_C_LONG, &titleExpBonus, 0, NULL);
    });
    
    double mult = 1.0;
    int vipLevel = CharacterDB::GetInstance().GetVipLevel(dwCharID);
    if (vipLevel >= 1 && vipLevel <= 5) {
        mult += vipLevel * 0.2;
    }
    if (totalBonusPercent > 0) {
        double cardBonus = (totalBonusPercent / 100.0) - 1.0;
        if (cardBonus > 0.0) {
            mult += cardBonus;
        }
    }
    if (titleExpBonus > 0) {
        mult += (titleExpBonus / 100.0);
    }
    
    // 动态拉取并结算云虎符在数据库 nData18 字段配置的额外经验百分比 (当前库配置 20 代表 +20% 额外经验)
    int expBonus = ItemDB::GetInstance().GetEquippedItemDataValue(dwCharID, 5, 18); // 5 = 云虎符
    if (expBonus > 0) {
        mult += (expBonus / 100.0);
    }
    
    DWORD finalIncrExp = dwIncrExp;
    DWORD dwEventExp = 0;
    if (mult > 1.0) {
        finalIncrExp = (DWORD)(dwIncrExp * mult);
        dwEventExp = finalIncrExp - dwIncrExp;
        LOG("[GrantExp] EXP multiplier applied: " + std::to_string(mult) + "x (VIP Level " + std::to_string(vipLevel) + ") | baseExp=" + std::to_string(dwIncrExp) + " + eventExp=" + std::to_string(dwEventExp) + " = finalExp=" + std::to_string(finalIncrExp));
    }
    
    newExp = oldExp + finalIncrExp;
    
    LOG("[GrantExp] charID=" + std::to_string(dwCharID) + " charType=" + std::to_string(bCharType)
        + " oldExp=" + std::to_string(oldExp) + " +incr=" + std::to_string(dwIncrExp) + " =newExp=" + std::to_string(newExp)
        + " curLvl=" + std::to_string(wLevel) + " remainTp=" + std::to_string(wRemainTp) + " remainSp=" + std::to_string(wRemainSp));
    
    // --- Level-up detection ---
    // Lookup LEVELTEMPLATE by [wLevel][bCharType], fallback to first bCharType
    auto getLevelExp = [&](WORD lvl) -> long long int {
        if (!g_LevelTemplates.count(lvl)) return 0;
        auto& charMap = g_LevelTemplates[lvl];
        if (charMap.count(bCharType)) return charMap[bCharType].dwNeedExp;
        return charMap.begin()->second.dwNeedExp;
    };
    auto getLevelSp = [&](WORD lvl) -> BYTE {
        if (!g_LevelTemplates.count(lvl)) return 0;
        auto& charMap = g_LevelTemplates[lvl];
        if (charMap.count(bCharType)) return charMap[bCharType].bSp;
        return charMap.begin()->second.bSp;
    };
    
    long long int levelExp = getLevelExp(wLevel);       // EXP threshold for current level
    long long int nextLevelExp = getLevelExp(wLevel + 1); // EXP threshold for next level
    if (nextLevelExp <= levelExp) nextLevelExp = levelExp + 1000; // Safety
    
    LOG("[GrantExp] levelExp[" + std::to_string(wLevel) + "]=" + std::to_string(levelExp)
        + " nextLevelExp[" + std::to_string(wLevel+1) + "]=" + std::to_string(nextLevelExp)
        + " needLvlUp=" + std::to_string(newExp >= nextLevelExp));
    
    long long int i64NextLevelUpExp = nextLevelExp;
    
    // Check level-up
    if (newExp >= nextLevelExp && g_LevelTemplates.count(wLevel + 1)) {
        bLevelUp = 1;
        BYTE spGain = getLevelSp(wLevel + 1); // SP granted by reaching this new level
        wLevel++;
        wRemainSp += spGain;
        dwTotalSp += spGain;
        // Update thresholds for the new level
        levelExp = getLevelExp(wLevel);
        nextLevelExp = getLevelExp(wLevel + 1);
        if (nextLevelExp <= levelExp) nextLevelExp = levelExp + 1000;
        i64NextLevelUpExp = nextLevelExp;
        LOG("[GrantExp] LEVEL UP! newLvl=" + std::to_string(wLevel) + " spGain=" + std::to_string(spGain)
            + " newRemainSp=" + std::to_string(wRemainSp) + " newLevelExp=" + std::to_string(levelExp)
            + " newNextLevelExp=" + std::to_string(nextLevelExp));
    }
    
    // --- TP segment detection (6 segments per level -> 6 TP per level) ---
    long long int diff = nextLevelExp - levelExp;
    long long int segSize = diff / 6;
    if (segSize <= 0) segSize = 1;
    
    // Calculate which segment the OLD exp was in
    long long int oldOffset = oldExp - levelExp;
    if (oldOffset < 0) oldOffset = 0;
    long long int oldSegIndex = oldOffset / segSize;
    if (oldSegIndex > 5) oldSegIndex = 5;
    
    // Calculate which segment the NEW exp is in
    long long int newOffset = newExp - levelExp;
    if (newOffset < 0) newOffset = 0;
    long long int newSegIndex = newOffset / segSize;
    if (newSegIndex > 5) newSegIndex = 5;
    
    // If we crossed segment boundaries, grant TP
    long long int i64NextTpUpExp = 0;
    if (!bLevelUp && newSegIndex > oldSegIndex) {
        WORD tpGained = (WORD)(newSegIndex - oldSegIndex);
        bTpUp = 1;
        wRemainTp += tpGained;
        dwTotalTp += tpGained;
    }
    
    // If level-up happened, grant remaining TP from old level + new level segments
    if (bLevelUp) {
        WORD oldLevelRemaining = (WORD)(6 - oldSegIndex);
        if (oldLevelRemaining > 0 && oldLevelRemaining <= 6) {
            bTpUp = 1;
            wRemainTp += oldLevelRemaining;
            dwTotalTp += oldLevelRemaining;
        }
        // Reset segment tracking for new level (segment 0)
        newSegIndex = 0;
        if (newOffset > 0) {
            long long int newLevelOffset = newExp - levelExp;
            if (newLevelOffset > 0) {
                long long int newLevelDiff = nextLevelExp - levelExp;
                long long int newSegSz = newLevelDiff / 6;
                if (newSegSz > 0) {
                    newSegIndex = newLevelOffset / newSegSz;
                    if (newSegIndex > 5) newSegIndex = 5;
                    if (newSegIndex > 0) {
                        bTpUp = 1;
                        wRemainTp += (WORD)newSegIndex;
                        dwTotalTp += (WORD)newSegIndex;
                    }
                }
            }
        }
    }
    
    // Compute TP segment boundaries for the client
    long long int curSegBase = levelExp + (newSegIndex * segSize);
    long long int curSegNext = curSegBase + segSize;
    if (newSegIndex == 5) curSegNext = nextLevelExp;
    i64NextTpUpExp = curSegNext;
    
    // --- Update CHAR_POWER via CharacterDB ---
    CharacterDB::GetInstance().UpdateExpAndLevel(dwCharID, newExp, wLevel,
        wRemainSp, dwTotalSp, wRemainTp, dwTotalTp, bLevelUp, bTpUp);
    
    // 累加五行点数计数器并保存
    WORD expCnt = expData.wFiveElmPointCnt;
    WORD elmPoint = expData.wFiveElmPoint;
    
    // 取消原有打怪普通经验兑换五行点数的临时硬凑逻辑 (20000 EXP = 1 Point)
    // 这样普通经验将不再错误地兑换为大量的五行点数。
    WORD finalFiveElmPointCnt = expCnt;
    
    // 2. 怪物五行经验直接结算并累加 (只通过怪物实际五行经验累积至 1000 时进行换点)
    if (dwFiveElmExpGained > 0) {
        expData.dwFiveElmPower += dwFiveElmExpGained;
        if (expData.dwFiveElmPower >= 1000) {
            DWORD powerGained = expData.dwFiveElmPower / 1000;
            elmPoint += (WORD)powerGained;
            expData.dwFiveElmPower = expData.dwFiveElmPower % 1000;
            bLevelUp = 1; // 强制客户端刷新状态
        }
    }
    
    // 内存同步更新 (落盘前读取最新的内存蓄气值)
    DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (g_MapInstances.count(playerMapID)) {
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        // 移除重复加锁，外层包处理已持有地图锁，防止非递归锁发生死锁
        PlayerData* pObj = mapInst->GetPlayer(dwCharID + 400000000);
        if (pObj) {
            pObj->wLevel = wLevel;
            pObj->wFiveElmPoint = elmPoint;
            pObj->wFiveElmPointCnt = finalFiveElmPointCnt;
            pObj->dwFiveElmPower = expData.dwFiveElmPower;
            expData.dwFiveElmGauge = pObj->dwFiveElmGauge; // 同步内存中高频更新的必杀蓄气值进行落盘
        }
    }

    CharacterDB::GetInstance().UpdateFiveElm(dwCharID, elmPoint, finalFiveElmPointCnt, expData.dwFiveElmPower, expData.dwFiveElmGauge,
                                             expData.wFireExp, expData.wWaterExp, expData.wWoodExp, expData.wMetalExp, expData.wEarthExp);
    expData.wFiveElmPoint = elmPoint;
    
    // --- Build 0x3B10 (CS_IF_CHAREXP_ACK) packet ---
    std::vector<BYTE> buf; buf.reserve(64);
    buf.resize(4); 
    
    auto pushDWord = [&](DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); };
    auto pushInt64 = [&](long long int d) {
        pushDWord((DWORD)(d & 0xFFFFFFFF));
        pushDWord((DWORD)((d >> 32) & 0xFFFFFFFF));
    };
    auto pushWord = [&](WORD w) { buf.push_back(w&0xFF); buf.push_back(w>>8); };
    
    pushDWord(dwIncrExp);        // dwIncrExp (发送原始基础经验值)
    pushInt64(newExp);           // n64Exp (current total)
    buf.push_back(bLevelUp);    // bLevelUp
    buf.push_back(bTpUp);       // bTpUp
    buf.push_back(0);           // bFiveElmLevelUp
    pushInt64(i64NextLevelUpExp); // i64NextLevelUpExp
    pushInt64(i64NextTpUpExp);   // i64NextTpUpExp
    pushWord(expData.wFiveElmPoint);                // wFiveElmPoint
    pushDWord(expData.dwFiveElmPower);               // dwFiveElmPower
    pushDWord(1000);               // dwFiveElmPowerMax
    
    // 优先采用内存中最新的蓄气值
    DWORD curGauge = expData.dwFiveElmGauge;
    if (g_MapInstances.count(playerMapID)) {
        // 移除重复加锁，外层包处理已持有地图锁，防止非递归锁发生死锁
        PlayerData* pObj = g_MapInstances[playerMapID]->GetPlayer(dwCharID + 400000000);
        if (pObj) curGauge = pObj->dwFiveElmGauge;
    }
    pushDWord(curGauge);               // dwFiveElmGauge
    pushDWord(dwEventExp);       // dwEventExp
    pushWord((WORD)dwFiveElmExpGained);                // wFiveElmExp (同步回传本次获得的经验值)
    
    PACKET_HEADER* head = (PACKET_HEADER*)buf.data();
    head->id = 0x3B10;
    head->payloadSize = buf.size() - sizeof(PACKET_HEADER); 
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), buf.size(), 0);
    SendStaminaSync(clientSocket, curGauge);
    
    // NOTE: Do NOT call SendCharStatusInfoAck here!
    // Caller must call it AFTER releasing the map mutex.
    
    LOG("[ExpSystem] Granted " + std::to_string(dwIncrExp) + " EXP & " + std::to_string(dwFiveElmExpGained) + " FiveElmEXP to player " + std::to_string(dwCharID) 
        + " | LvlUp=" + std::to_string(bLevelUp) + " TpUp=" + std::to_string(bTpUp) 
        + " NewExp=" + std::to_string(newExp) + " Level=" + std::to_string(wLevel) + " Power=" + std::to_string(expData.dwFiveElmPower) + " Point=" + std::to_string(expData.wFiveElmPoint));
    return bLevelUp || bTpUp;
}

// 辅助状态同步函数：发送 0x3B10 给客户端同步五行与经验最新状态
void SyncFiveElmStatus(SOCKET clientSocket, DWORD charID, PlayerData* pObj) {
    if (!pObj) return;
    CharacterDB::ExpData expData;
    if (CharacterDB::GetInstance().GetExpData(charID, expData)) {
        auto getLevelExp = [&](WORD lvl) -> long long int {
            if (!g_LevelTemplates.count(lvl)) return 0;
            auto& charMap = g_LevelTemplates[lvl];
            if (charMap.count(expData.bCharType)) return charMap[expData.bCharType].dwNeedExp;
            return charMap.begin()->second.dwNeedExp;
        };
        long long int levelExp = getLevelExp(expData.wLevel);
        long long int nextLevelExp = getLevelExp(expData.wLevel + 1);
        if (nextLevelExp <= levelExp) nextLevelExp = levelExp + 1000;
        long long int i64NextLevelUpExp = nextLevelExp;

        long long int diff = nextLevelExp - levelExp;
        long long int segSize = diff / 6;
        if (segSize <= 0) segSize = 1;
        long long int newOffset = expData.dwExp - levelExp;
        if (newOffset < 0) newOffset = 0;
        long long int newSegIndex = newOffset / segSize;
        if (newSegIndex > 5) newSegIndex = 5;

        long long int curSegBase = levelExp + (newSegIndex * segSize);
        long long int curSegNext = curSegBase + segSize;
        if (newSegIndex == 5) curSegNext = nextLevelExp;
        long long int i64NextTpUpExp = curSegNext;

        std::vector<BYTE> buf; buf.reserve(64);
        buf.resize(4); 
        auto pushDWord = [&](DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); };
        auto pushInt64 = [&](long long int d) {
            pushDWord((DWORD)(d & 0xFFFFFFFF));
            pushDWord((DWORD)((d >> 32) & 0xFFFFFFFF));
        };
        auto pushWord = [&](WORD w) { buf.push_back(w&0xFF); buf.push_back(w>>8); };
        
        pushDWord(0); // dwIncrExp
        pushInt64(expData.dwExp);
        buf.push_back(0); // bLevelUp
        buf.push_back(0); // bTpUp
        buf.push_back(0); // bFiveElmLevelUp
        pushInt64(i64NextLevelUpExp);
        pushInt64(i64NextTpUpExp);
        pushWord(pObj->wFiveElmPoint);
        pushDWord(pObj->dwFiveElmPower);
        pushDWord(1000);
        pushDWord(pObj->dwFiveElmGauge);
        pushDWord(0); // dwEventExp
        pushWord(0);  // wFiveElmExpGained
        
        PACKET_HEADER* head = (PACKET_HEADER*)buf.data();
        head->id = 0x3B10;
        head->payloadSize = buf.size() - sizeof(PACKET_HEADER); 
        EncryptPacket(buf.data(), 0x42);
        SafeSend(clientSocket, (const char*)buf.data(), buf.size(), 0);
        
        LOG("[ExpSystem] SyncFiveElmStatus: Sync player " + std::to_string(charID) + " Gauge=" + std::to_string(pObj->dwFiveElmGauge));
        SendStaminaSync(clientSocket, pObj->dwFiveElmGauge);
    }
}

void SendStaminaSync(SOCKET clientSocket, DWORD dwGauge) {
    if (clientSocket == INVALID_SOCKET) return;
    BYTE bStaminaCnt = (BYTE)(dwGauge / 1000);
    if (bStaminaCnt > 5) bStaminaCnt = 5;
    
    std::vector<BYTE> buf(5);
    PACKET_HEADER* head = (PACKET_HEADER*)buf.data();
    head->id = 0x3B77; // CS_IF_STAMINA_ACK
    head->payloadSize = 1;
    buf[4] = bStaminaCnt;
    
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), buf.size(), 0);
    LOG("[ExpSystem] SendStaminaSync sent bStaminaCnt=" + std::to_string(bStaminaCnt) + " for Gauge=" + std::to_string(dwGauge));
}

bool ProcessPetExpShare(DWORD ownerCharID, DWORD rawExp, DWORD mapID, bool callerHoldsMapLock, DWORD& expForPlayer, DWORD& expForPet) {
    expForPlayer = rawExp;
    expForPet = 0;
    if (rawExp == 0 || ownerCharID == 0) return false;

    if (!g_MapInstances.count(mapID)) return false;
    CMapInstance* mapInst = g_MapInstances[mapID];

    DWORD petObjID = 0;
    WORD petLevel = 0;
    BYTE petNpcType = 0;
    DWORD petHpCur = 0;

    auto scanPet = [&]() {
        for (const auto& pair : mapInst->GetPlayers()) {
            const PlayerData& pl = pair.second;
            // 战宠判定：属于该主人、bObjectType == 4、存活、且不是分身(251)或幻兽龙(250)
            if (pl.dwOwnerID == ownerCharID && pl.bObjectType == 4 && pl.dwHpCur > 0 &&
                pl.bNpcType != 250 && pl.bNpcType != 251) {
                petObjID = pl.dwObjectID;
                petLevel = pl.wLevel;
                petNpcType = pl.bNpcType;
                petHpCur = pl.dwHpCur;
                break;
            }
        }
    };

    if (callerHoldsMapLock) {
        scanPet();
    } else {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        scanPet();
    }

    // 没有出战普通战宠
    if (petObjID == 0) return false;

    int maxPetLvl = PetLevelExpManager::GetInstance().GetMaxLevel();
    // 宠物已达到满级
    if (petLevel >= maxPetLvl) {
        return false;
    }

    // 判断主人角色是否满级（根据 LEVELTEMPLATE 表，角色最高等级为 150 级）
    bool isOwnerMaxLevel = false;
    CharacterDB::CharPower cp;
    if (CharacterDB::GetInstance().GetCharData(ownerCharID, cp)) {
        if (!g_LevelTemplates.count(cp.wLevel + 1) || cp.wLevel >= 150) {
            isOwnerMaxLevel = true;
        }
    }


    // 方案 A 经验分流规则：
    if (isOwnerMaxLevel) {
        // 人物满级：宠物获取 100% 经验，人物获得 0%
        expForPet = rawExp;
        expForPlayer = 0;
    } else {
        // 人物未满级：分掉人物总经验的 50%
        expForPet = rawExp / 2;
        if (expForPet == 0 && rawExp > 0) expForPet = 1;
        expForPlayer = rawExp - expForPet;
    }

    if (expForPet == 0) return false;

    DWORD dwPetID = petObjID - 800000000;

    // 从数据库查询当前宠物的准确数据
    struct {
        char szName[32] = {0};
        WORD wLevel = 1;
        long long biExp = 0;
        DWORD dwHpCur = 0;
        DWORD dwHpMax = 0;
        WORD wAtkPwr = 0;
        WORD wDefPwr = 0;
        WORD wAtkRating = 0;
        WORD wAvoidRatio = 0;
    } petData;

    bool found = false;
    std::string qPet = "SELECT szName, wLevel, biExp, dwHpCur, dwHpMax, wAtkPwr, wDefPwr, wAtkRating, wAvoidRatio FROM CHAR_PET WHERE dwID = " + std::to_string(dwPetID);
    DBHelper::GetInstance().ExecuteQuery(qPet, [&](SQLHSTMT hStmt) {
        SQLLEN c[9];
        int idx = 1;
        SQLGetData(hStmt, idx++, SQL_C_CHAR, petData.szName, sizeof(petData.szName), &c[0]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &petData.wLevel, 0, &c[1]);
        SQLGetData(hStmt, idx++, SQL_C_SBIGINT, &petData.biExp, 0, &c[2]);
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &petData.dwHpCur, 0, &c[3]);
        SQLGetData(hStmt, idx++, SQL_C_ULONG, &petData.dwHpMax, 0, &c[4]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &petData.wAtkPwr, 0, &c[5]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &petData.wDefPwr, 0, &c[6]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &petData.wAtkRating, 0, &c[7]);
        SQLGetData(hStmt, idx++, SQL_C_USHORT, &petData.wAvoidRatio, 0, &c[8]);
        found = true;
    });

    if (!found) return false;

    long long newExp = petData.biExp + expForPet;
    WORD oldLevel = petData.wLevel;
    WORD curLevel = oldLevel;

    // 1. 发送 0x3544 CS_NC_PETEXP_ACK（经验增量提示）
    // 注意：客户端 g_PetList 的 key 是 petObjID (800000000 + dwPetID)，必须传 petObjID 客户端才能 Find 成功
    SOCKET ownerSock = SessionMgr::GetInstance().GetSocketByCharID(ownerCharID);
    if (ownerSock != INVALID_SOCKET) {
        std::vector<BYTE> expBuf(12);
        PACKET_HEADER* expHead = (PACKET_HEADER*)expBuf.data();
        expHead->id = 0x3544; // CS_NC_PETEXP_ACK
        expHead->payloadSize = 8;
        DWORD* pDword = (DWORD*)(expBuf.data() + sizeof(PACKET_HEADER));
        pDword[0] = petObjID; // 800000000 + dwPetID
        pDword[1] = expForPet;
        EncryptPacket(expBuf.data(), 0x42);
        SafeSend(ownerSock, (const char*)expBuf.data(), (int)expBuf.size(), 0);
    }

    // 2. 循环检查升级
    while (curLevel < maxPetLvl) {
        long long i64LevelExp = PetLevelExpManager::GetInstance().GetLevelStartExp(curLevel);
        long long i64NeedExp = PetLevelExpManager::GetInstance().GetNeedExp(curLevel);
        long long i64NextLevelUpExp = i64LevelExp + i64NeedExp;

        if (newExp >= i64NextLevelUpExp) {
            curLevel++;
            sPetLevelData lvlData;
            if (PetLevelExpManager::GetInstance().GetLevelData(curLevel, lvlData)) {
                petData.wAtkPwr += (WORD)lvlData.wAtkGrowth;
                petData.wDefPwr += (WORD)lvlData.wDefGrowth;
                petData.dwHpMax += (DWORD)lvlData.dwHpGrowth;
                petData.dwHpCur += (DWORD)lvlData.dwHpGrowth;
                if (petData.dwHpCur > petData.dwHpMax) petData.dwHpCur = petData.dwHpMax;
            }
        } else {
            break;
        }
    }

    // 3. 升级处理 vs 仅增加经验
    if (curLevel > oldLevel) {
        long long newLevelExp = PetLevelExpManager::GetInstance().GetLevelStartExp(curLevel);
        long long newNextLevelUpExp = newLevelExp + PetLevelExpManager::GetInstance().GetNeedExp(curLevel);

        // 更新数据库
        std::string qUpd = "UPDATE CHAR_PET SET wLevel = " + std::to_string(curLevel)
            + ", biExp = " + std::to_string(newExp)
            + ", dwHpCur = " + std::to_string(petData.dwHpCur)
            + ", dwHpMax = " + std::to_string(petData.dwHpMax)
            + ", wAtkPwr = " + std::to_string(petData.wAtkPwr)
            + ", wDefPwr = " + std::to_string(petData.wDefPwr)
            + " WHERE dwID = " + std::to_string(dwPetID);
        DBHelper::GetInstance().ExecuteUpdate(qUpd);

        // 更新 MapInstance 活体 PlayerData 属性
        auto updatePetLive = [&]() {
            PlayerData* pPet = mapInst->GetPlayer(petObjID);
            if (pPet) {
                pPet->wLevel = curLevel;
                pPet->dwHpCur = petData.dwHpCur;
                pPet->dwHpMax = petData.dwHpMax;
                pPet->dwTotalAtk = petData.wAtkPwr;
                pPet->dwTotalDef = petData.wDefPwr;
            }
        };
        if (callerHoldsMapLock) {
            updatePetLive();
        } else {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            updatePetLive();
        }

        // 发送 0x3542 CS_NC_PETLEVELUP_ACK
        if (ownerSock != INVALID_SOCKET) {
            std::vector<BYTE> lvlBuf;
            lvlBuf.resize(sizeof(PACKET_HEADER));
            auto pushByte = [&](BYTE b) { lvlBuf.push_back(b); };
            auto pushWord = [&](WORD w) { lvlBuf.push_back(w & 0xFF); lvlBuf.push_back(w >> 8); };
            auto pushDWord = [&](DWORD d) {
                lvlBuf.push_back(d & 0xFF); lvlBuf.push_back((d >> 8) & 0xFF);
                lvlBuf.push_back((d >> 16) & 0xFF); lvlBuf.push_back(d >> 24);
            };
            auto pushInt64 = [&](INT64 i) {
                for (int b = 0; b < 8; ++b) lvlBuf.push_back((BYTE)((i >> (b * 8)) & 0xFF));
            };

            pushDWord(ownerCharID);                 // dwOwnID
            pushDWord(petObjID);                    // dwID (必须传 petObjID 800000000+，客户端才能在 g_PetList 查到)
            pushByte((BYTE)curLevel);               // bLevel
            pushWord(petData.wAtkPwr);              // wAtkPwr
            pushWord(petData.wDefPwr);              // wDefPwr
            pushWord(petData.wAtkRating);           // wAtkRating
            pushWord(petData.wAvoidRatio);          // wAvoidRatio
            pushDWord(petData.dwHpCur);             // dwHpCur
            pushDWord(petData.dwHpMax);             // dwHpMax
            pushInt64(newExp);                      // i64Exp
            pushInt64(newLevelExp);                 // i64LevelExp
            pushInt64(newNextLevelUpExp);           // i64NextLevelUpExp


            PACKET_HEADER* lvlHead = (PACKET_HEADER*)lvlBuf.data();
            lvlHead->id = 0x3542; // CS_NC_PETLEVELUP_ACK
            lvlHead->payloadSize = (WORD)(lvlBuf.size() - sizeof(PACKET_HEADER));
            EncryptPacket(lvlBuf.data(), 0x42);
            SafeSend(ownerSock, (const char*)lvlBuf.data(), (int)lvlBuf.size(), 0);

            LOG("[PetExp] Pet LEVEL UP! PetID=" + std::to_string(dwPetID) + " Name=" + petData.szName
                + " Lvl=" + std::to_string(oldLevel) + "->" + std::to_string(curLevel)
                + " Atk=" + std::to_string(petData.wAtkPwr) + " Def=" + std::to_string(petData.wDefPwr)
                + " Hp=" + std::to_string(petData.dwHpCur) + "/" + std::to_string(petData.dwHpMax));
        }
    } else {
        // 未升级，更新数据库 biExp
        std::string qUpd = "UPDATE CHAR_PET SET biExp = " + std::to_string(newExp) + " WHERE dwID = " + std::to_string(dwPetID);
        DBHelper::GetInstance().ExecuteUpdate(qUpd);

        LOG("[PetExp] Pet gained exp: PetID=" + std::to_string(dwPetID) + " +" + std::to_string(expForPet) + " Total=" + std::to_string(newExp));
    }

    return true;
}

void DistributePartyExp(DWORD killerCharID, DWORD deadExp, DWORD monsterFiveElmExp, DWORD mapID, WORD monX, WORD monY, bool callerHoldsMapLock) {
    if (deadExp == 0 && monsterFiveElmExp == 0) return;

    DWORD partyID = PartyManager::GetInstance().GetPartyID(killerCharID);
    BYTE expShareMode = (partyID != 0) ? PartyManager::GetInstance().GetExpShareMode(partyID) : 0;

    // 个人分配模式（0=个人）或者未组队
    if (partyID == 0 || expShareMode == 0) {
        DWORD expForPlayer = deadExp;
        DWORD expForPet = 0;
        ProcessPetExpShare(killerCharID, deadExp, mapID, callerHoldsMapLock, expForPlayer, expForPet);

        bool lvlUp = false;
        if (expForPlayer > 0 || monsterFiveElmExp > 0) {
            lvlUp = GrantExpToPlayer(killerCharID, expForPlayer, monsterFiveElmExp);
        }
        if (lvlUp) {
            SOCKET kSock = SessionMgr::GetInstance().GetSocketByCharID(killerCharID);
            if (kSock != INVALID_SOCKET) {
                UpdatePlayerStatsAndSend(kSock, killerCharID);
            }
            CharacterDB::GetInstance().RestoreHpIpToMax(killerCharID);
            if (g_MapInstances.count(mapID)) {
                CMapInstance* mapInst = g_MapInstances[mapID];
                auto updateLive = [&]() {
                    PlayerData* pl = mapInst->GetPlayer(killerCharID + 400000000);
                    if (pl) {
                        pl->dwHpCur = pl->dwHpMax;
                        pl->wIpCur = pl->wIpMax;
                        CharacterDB::CharPower cp;
                        if (CharacterDB::GetInstance().GetCharData(killerCharID, cp)) {
                            pl->wLevel = (cp.wLevel > 0) ? cp.wLevel : pl->wLevel;
                        }
                        PartyManager::GetInstance().BroadcastMemberPosition(
                            killerCharID, pl->wLevel, pl->dwHpCur, pl->dwHpMax, pl->dwMapID, pl->wPosX, pl->wPosY);
                    }
                };
                if (callerHoldsMapLock) {
                    updateLive();
                } else {
                    std::lock_guard<std::mutex> lock(mapInst->GetMutex());
                    updateLive();
                }
            }
        }
        return;
    }

    // 共同分配模式（1=共同分配）：必须同时满足同一地图 + 距离 <= 150.0f
    auto members = PartyManager::GetInstance().GetMembers(partyID);
    std::vector<DWORD> eligibleMembers;

    if (g_MapInstances.count(mapID)) {
        CMapInstance* mapInst = g_MapInstances[mapID];
        auto scanMembers = [&]() {
            for (auto& m : members) {
                SOCKET mSock = SessionMgr::GetInstance().GetSocketByCharID(m.dwCharID);
                if (mSock == INVALID_SOCKET) continue;
                DWORD mMapID = SessionMgr::GetInstance().GetMapID(mSock);
                if (mMapID != mapID) continue; // 严格同一地图限制！

                PlayerData* pl = mapInst->GetPlayer(m.dwCharID + 400000000);
                if (!pl) continue;

                float dx = (float)pl->wPosX - (float)monX;
                float dy = (float)pl->wPosY - (float)monY;
                float dist = std::sqrt(dx * dx + dy * dy);
                if (dist <= 150.0f) { // 严格 150 格视野交互距离限制！
                    eligibleMembers.push_back(m.dwCharID);
                }
            }
        };

        if (callerHoldsMapLock) {
            scanMembers();
        } else {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            scanMembers();
        }
    }

    if (eligibleMembers.empty()) {
        eligibleMembers.push_back(killerCharID);
    }

    size_t count = eligibleMembers.size();

    // 1. 组队人数经验加成：
    // 2人: 5%, 3人: 10%, 4人: 15%, 5人: 20%, 6人: 25%, 7人: 30%, 8人: 35%
    double sizeBonusRatio = 0.0;
    if (count >= 2) {
        double pct = (double)(count - 1) * 0.05;
        if (pct > 0.35) pct = 0.35; // 最高 8 人 +35%
        sizeBonusRatio = pct;
    }

    // 2. 队伍中 4 个职业（1=剑影, 2=怜琅, 3=武斗, 4=夜叉）同时存在额外经验加成 25%
    std::set<BYTE> activeClasses;
    if (g_MapInstances.count(mapID)) {
        CMapInstance* mapInst = g_MapInstances[mapID];
        auto checkClasses = [&]() {
            for (DWORD memCharID : eligibleMembers) {
                PlayerData* pl = mapInst->GetPlayer(memCharID + 400000000);
                if (pl && pl->bPropType >= 1 && pl->bPropType <= 4) {
                    activeClasses.insert(pl->bPropType);
                } else {
                    CharacterDB::CharPower cp;
                    if (CharacterDB::GetInstance().GetCharData(memCharID, cp)) {
                        if (cp.bCharType >= 1 && cp.bCharType <= 4) {
                            activeClasses.insert(cp.bCharType);
                        }
                    }
                }
            }
        };
        if (callerHoldsMapLock) {
            checkClasses();
        } else {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            checkClasses();
        }
    }

    bool hasAllFourClasses = (activeClasses.count(1) && activeClasses.count(2) &&
                              activeClasses.count(3) && activeClasses.count(4));
    double classBonusRatio = hasAllFourClasses ? 0.25 : 0.0;
    double totalPartyBonusRatio = sizeBonusRatio + classBonusRatio;

    DWORD pooledExp = deadExp;
    if (totalPartyBonusRatio > 0.0) {
        pooledExp = (DWORD)(deadExp * (1.0 + totalPartyBonusRatio));
    }
    DWORD pooledFE = monsterFiveElmExp;
    if (totalPartyBonusRatio > 0.0) {
        pooledFE = (DWORD)(monsterFiveElmExp * (1.0 + totalPartyBonusRatio));
    }

    DWORD sharedExp = pooledExp / (DWORD)eligibleMembers.size();
    if (sharedExp == 0 && deadExp > 0) sharedExp = 1;
    DWORD sharedFE = pooledFE / (DWORD)eligibleMembers.size();

    LOG("[PARTY-EXP] Party " + std::to_string(partyID) +
        " sharing " + std::to_string(deadExp) + "->" + std::to_string(pooledExp) + " EXP & " +
        std::to_string(monsterFiveElmExp) + "->" + std::to_string(pooledFE) + " FE among " +
        std::to_string(eligibleMembers.size()) + " nearby members (" +
        std::to_string(sharedExp) + " each) | Bonus: Size=" +
        std::to_string((int)(sizeBonusRatio * 100)) + "%, 4Classes=" +
        (hasAllFourClasses ? "25%" : "0%") + ", Total=" +
        std::to_string((int)(totalPartyBonusRatio * 100)) + "%");

    for (DWORD memberCharID : eligibleMembers) {
        DWORD expForPlayer = sharedExp;
        DWORD expForPet = 0;
        ProcessPetExpShare(memberCharID, sharedExp, mapID, callerHoldsMapLock, expForPlayer, expForPet);

        bool lvlUp = false;
        if (expForPlayer > 0 || sharedFE > 0) {
            lvlUp = GrantExpToPlayer(memberCharID, expForPlayer, sharedFE);
        }
        if (lvlUp) {
            SOCKET mSock = SessionMgr::GetInstance().GetSocketByCharID(memberCharID);
            if (mSock != INVALID_SOCKET) {
                UpdatePlayerStatsAndSend(mSock, memberCharID);
            }
            CharacterDB::GetInstance().RestoreHpIpToMax(memberCharID);
            if (g_MapInstances.count(mapID)) {
                CMapInstance* mapInst = g_MapInstances[mapID];
                auto updateMemLive = [&]() {
                    PlayerData* pl = mapInst->GetPlayer(memberCharID + 400000000);
                    if (pl) {
                        pl->dwHpCur = pl->dwHpMax;
                        pl->wIpCur = pl->wIpMax;
                        CharacterDB::CharPower cp;
                        if (CharacterDB::GetInstance().GetCharData(memberCharID, cp)) {
                            pl->wLevel = (cp.wLevel > 0) ? cp.wLevel : pl->wLevel;
                        }
                        PartyManager::GetInstance().BroadcastMemberPosition(
                            memberCharID, pl->wLevel, pl->dwHpCur, pl->dwHpMax, pl->dwMapID, pl->wPosX, pl->wPosY);
                    }
                };
                if (callerHoldsMapLock) {
                    updateMemLive();
                } else {
                    std::lock_guard<std::mutex> lock(mapInst->GetMutex());
                    updateMemLive();
                }
            }
        }
    }
}



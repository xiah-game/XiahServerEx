#include "ExpSystem.h"
#include "../ServerCore.h"
#include "../DB/CharacterDB.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../DBHelper.h"
#include "../DB/ItemDB.h"

extern std::map<DWORD, CMapInstance*> g_MapInstances;

bool GrantExpToPlayer(DWORD dwCharID, DWORD dwIncrExp) {
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
    pushWord(0);                // wFiveElmPoint
    pushDWord(0);               // dwFiveElmPower
    pushDWord(0);               // dwFiveElmPowerMax
    pushDWord(0);               // dwFiveElmGauge
    pushDWord(dwEventExp);       // dwEventExp (发送增益活动经验值，触发客户端 基础经验(+加成经验) 提示)
    pushWord(0);                // wFiveElmExp
    
    PACKET_HEADER* head = (PACKET_HEADER*)buf.data();
    head->id = 0x3B10;
    head->payloadSize = buf.size() - sizeof(PACKET_HEADER); 
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), buf.size(), 0);
    
    // NOTE: Do NOT call SendCharStatusInfoAck here!
    // Caller must call it AFTER releasing the map mutex.
    
    LOG("[ExpSystem] Granted " + std::to_string(dwIncrExp) + " EXP to player " + std::to_string(dwCharID) 
        + " | LvlUp=" + std::to_string(bLevelUp) + " TpUp=" + std::to_string(bTpUp) 
        + " NewExp=" + std::to_string(newExp) + " Level=" + std::to_string(wLevel));
    return bLevelUp || bTpUp;
}

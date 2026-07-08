#include "FiveElmHandler.h"
#include "../ServerCore.h"
#include "../DB/CharacterDB.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../GameObjects/PlayerManager.h"
#include "../GameObjects/MugongManager.h"
#include "../DBHelper.h"

// 广播五行环绕光效给全图玩家
static void BroadcastFiveElmStatus(sServerObject* player, BYTE bCurFiveElm, BYTE bFELevel) {
    if (!player) return;
    std::vector<BYTE> buf;
    buf.resize(4, 0); // Header placeholder

    // bType (BYTE) -> ACT_FIVEELM = 8
    buf.push_back(8);

    // dwObjID (DWORD)
    auto pushDWord = [&](DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back(d >> 24); };
    auto pushString = [&](const std::string& str) {
        WORD len = (WORD)str.length();
        buf.push_back(len & 0xFF); buf.push_back(len >> 8);
        for (char c : str) buf.push_back(c);
    };

    pushDWord(player->dwObjectID);

    // dwData1 -> bCurFiveElm (0 = Close, 1~5 = Active Element)
    pushDWord((DWORD)bCurFiveElm);

    // dwData2 -> bFELevel
    pushDWord((DWORD)bFELevel);

    // dwData3 -> 0
    pushDWord(0);

    // strTemp1, strTemp2, strTemp3 -> empty
    pushString("");
    pushString("");
    pushString("");

    // Fill Header
    WORD packetID = 0x3F10; // CS_CD_CHARUPDATE_ACK
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);

    EncryptPacket(buf.data(), 0x42);

    // Broadcast to map
    DWORD mapID = player->dwMapID;
    if (g_MapInstances.count(mapID)) {
        CMapInstance* mapInst = g_MapInstances[mapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        mapInst->BroadcastPacket(buf);
    }
}

// 提升五行属性的 Exp/Level
void OnExecFiveElmReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    if (size < 1) return;
    BYTE bFiveElm = payload[0];
    if (bFiveElm < 1 || bFiveElm > 5) return;

    LOG("[FiveElmHandler] OnExecFiveElmReq: charID=" + std::to_string(charID) + " upgrade=" + std::to_string(bFiveElm));

    CharacterDB::ExpData expData;
    if (!CharacterDB::GetInstance().GetExpData(charID, expData)) return;

    if (expData.wFiveElmPoint < 1) {
        // 失败：点数不足
        std::vector<BYTE> ack(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
        head->id = 0x3B71; // CS_IF_EXECFIVEELM_ACK
        head->payloadSize = 1;
        ack[4] = 1; // ERR_EXECFIVEELM_NEEDPOINT
        EncryptPacket(ack.data(), 0x42);
        SafeSend(s, (const char*)ack.data(), ack.size(), 0);
        return;
    }

    WORD currentLevel = 0;
    if (bFiveElm == 1) currentLevel = expData.wFireExp;
    else if (bFiveElm == 2) currentLevel = expData.wWaterExp;
    else if (bFiveElm == 3) currentLevel = expData.wWoodExp;
    else if (bFiveElm == 4) currentLevel = expData.wMetalExp;
    else if (bFiveElm == 5) currentLevel = expData.wEarthExp;

    if (currentLevel >= 1000) {
        // 失败：已满级 (最高 1000 点)
        std::vector<BYTE> ack(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
        head->id = 0x3B71;
        head->payloadSize = 1;
        ack[4] = 3; // 对应已满级状态提示
        EncryptPacket(ack.data(), 0x42);
        SafeSend(s, (const char*)ack.data(), ack.size(), 0);
        return;
    }

    // 提升属性等级并扣点数
    expData.wFiveElmPoint -= 1;
    currentLevel += 1;

    if (bFiveElm == 1) expData.wFireExp = currentLevel;
    else if (bFiveElm == 2) expData.wWaterExp = currentLevel;
    else if (bFiveElm == 3) expData.wWoodExp = currentLevel;
    else if (bFiveElm == 4) expData.wMetalExp = currentLevel;
    else if (bFiveElm == 5) expData.wEarthExp = currentLevel;

    // 保存至数据库
    CharacterDB::GetInstance().UpdateFiveElm(charID, expData.wFiveElmPoint, expData.wFiveElmPointCnt, expData.dwFiveElmPower, expData.dwFiveElmGauge,
                                             expData.wFireExp, expData.wWaterExp, expData.wWoodExp, expData.wMetalExp, expData.wEarthExp);

    // 同时更新内存中的 PlayerData
    DWORD targetMapID = SessionMgr::GetInstance().GetMapID(s);
    if (g_MapInstances.count(targetMapID)) {
        CMapInstance* mapInst = g_MapInstances[targetMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        PlayerData* player = mapInst->GetPlayer(charID + 400000000);
        if (player) {
            player->wFiveElmPoint = expData.wFiveElmPoint;
            if (bFiveElm == 1) player->wFireExp = currentLevel;
            else if (bFiveElm == 2) player->wWaterExp = currentLevel;
            else if (bFiveElm == 3) player->wWoodExp = currentLevel;
            else if (bFiveElm == 4) player->wMetalExp = currentLevel;
            else if (bFiveElm == 5) player->wEarthExp = currentLevel;
        }
    }

    // 回复成功包 (负载大小 6 字节: bResult(1), bFiveElm(1), wFiveElmExp(2), wFiveElmPoint(2))
    std::vector<BYTE> ack(10);
    PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
    head->id = 0x3B71; // CS_IF_EXECFIVEELM_ACK
    head->payloadSize = 6;
    ack[4] = 0; // ERR_EXECFIVEELM_SUCCESS
    ack[5] = bFiveElm;
    *(WORD*)(ack.data() + 6) = currentLevel;
    *(WORD*)(ack.data() + 8) = expData.wFiveElmPoint;

    EncryptPacket(ack.data(), 0x42);
    SafeSend(s, (const char*)ack.data(), ack.size(), 0);

    LOG("[FiveElmHandler] Upgrade SUCCESS. Element=" + std::to_string(bFiveElm) + " NewLvl=" + std::to_string(currentLevel) + " Points=" + std::to_string(expData.wFiveElmPoint));
}

// 激活或切换五行状态
void OnChangeFiveElmReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    if (size < 1) return;
    BYTE bFiveElm = payload[0];
    if (bFiveElm < 1 || bFiveElm > 5) return;

    LOG("[FiveElmHandler] OnChangeFiveElmReq: charID=" + std::to_string(charID) + " target=" + std::to_string(bFiveElm));

    DWORD dwObjectID = charID + 400000000;
    DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
    if (!g_MapInstances.count(mapID)) return;
    CMapInstance* mapInst = g_MapInstances[mapID];

    sServerObject* player = nullptr;
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        player = mapInst->GetPlayer(dwObjectID);
    }
    if (!player) return;

    // 1. 冷却校验 (30秒限制)
    DWORD curTime = GetTickCount();
    if (curTime - player->dwLastFiveElmChangeTime < 30000) {
        std::vector<BYTE> ack(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
        head->id = 0x3B73; // CS_IF_CHANGEFIVEELM_ACK
        head->payloadSize = 1;
        ack[4] = 4; // ERR_CHANGEFIVEELM_SHORTTIME
        EncryptPacket(ack.data(), 0x42);
        SafeSend(s, (const char*)ack.data(), ack.size(), 0);
        return;
    }

    // 2. 熟练度前置校验 (必须分配了至少 1 点属性点)
    CharacterDB::ExpData expData;
    if (!CharacterDB::GetInstance().GetExpData(charID, expData)) return;

    WORD currentPoints = 0;
    if (bFiveElm == 1) currentPoints = expData.wFireExp;
    else if (bFiveElm == 2) currentPoints = expData.wWaterExp;
    else if (bFiveElm == 3) currentPoints = expData.wWoodExp;
    else if (bFiveElm == 4) currentPoints = expData.wMetalExp;
    else if (bFiveElm == 5) currentPoints = expData.wEarthExp;

    if (currentPoints < 1) {
        std::vector<BYTE> ack(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
        head->id = 0x3B73;
        head->payloadSize = 1;
        ack[4] = 2; // ERR_CHANGEFIVEELM_NOTYET (属性点未分配)
        EncryptPacket(ack.data(), 0x42);
        SafeSend(s, (const char*)ack.data(), ack.size(), 0);
        return;
    }

    // 获取实际武功对应的等级作为物理光效等级
    DWORD targetMugongID = 150 + bFiveElm - 1;
    int mugongLevel = MugongManager::GetInstance()->GetPlayerMugongLevel(charID, targetMugongID);

    // 3. 切换内存状态并注册五行被动 Buff 技能 (150~154)，并向客户端广播 Buff 图标状态
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        
        // 3.1 清除现有的五行 Buff (并发送 0x402E 清除客户端右上角图标)
        for (int i = 0; i < 5; ++i) {
            DWORD oldMugongID = 150 + i;
            if (player->activeBuffs.count(oldMugongID) > 0) {
                std::vector<BYTE> endAck(4 + 11);
                BYTE* ep = endAck.data() + 4;
                ep[0] = 0; // bResult
                *(DWORD*)(ep + 1) = dwObjectID;
                ep[5] = 1; // bObjectType (Player)
                *(DWORD*)(ep + 6) = oldMugongID;
                ep[10] = player->activeBuffs[oldMugongID].bLevel;
                PACKET_HEADER* headE = (PACKET_HEADER*)endAck.data();
                headE->id = 0x402E; // CS_BT_KEEPUPMUGONGEND_ACK
                headE->payloadSize = 11;
                EncryptPacket(endAck.data(), 0x42);
                mapInst->BroadcastPacket(endAck);
            }
            player->activeBuffs.erase(oldMugongID);
        }

        // 3.2 添加新的五行 Buff (并发送 0x402C 挂载客户端右上角图标)
        sServerObject::sActiveBuff newBuff;
        newBuff.dwMugongID = targetMugongID;
        newBuff.bLevel = (BYTE)mugongLevel;
        newBuff.dwEndTime = GetTickCount() + 7200000; // 2 小时作为长期 Buff
        newBuff.bIsDebuff = false;
        player->activeBuffs[targetMugongID] = newBuff;

        std::vector<BYTE> buffAck(4 + 11);
        BYTE* bp = buffAck.data() + 4;
        bp[0] = 0; // bResult
        *(DWORD*)(bp + 1) = dwObjectID;
        bp[5] = 1; // bObjectType (Player)
        *(DWORD*)(bp + 6) = targetMugongID;
        bp[10] = (BYTE)mugongLevel;
        PACKET_HEADER* headB = (PACKET_HEADER*)buffAck.data();
        headB->id = 0x402C; // CS_BT_KEEPUPMUGONGSTART_ACK
        headB->payloadSize = 11;
        EncryptPacket(buffAck.data(), 0x42);
        mapInst->BroadcastPacket(buffAck);

        player->bCurFiveElm = bFiveElm;
        player->bFELevel = (BYTE)mugongLevel;
        player->dwLastFiveElmChangeTime = curTime;
    }

    // 4. 发送成功 ACK 包
    std::vector<BYTE> ack(7);
    PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
    head->id = 0x3B73; // CS_IF_CHANGEFIVEELM_ACK
    head->payloadSize = 3;
    ack[4] = 0;
    ack[5] = bFiveElm;
    ack[6] = 1;
    EncryptPacket(ack.data(), 0x42);
    SafeSend(s, (const char*)ack.data(), ack.size(), 0);

    // 5. 广播状态给 AoI 其他玩家展示光效
    BroadcastFiveElmStatus(player, bFiveElm, (BYTE)mugongLevel);

    LOG("[FiveElmHandler] Active SUCCESS. Element=" + std::to_string(bFiveElm) + " Level=" + std::to_string(mugongLevel));
}

// 结束五行状态
void OnEndFiveElmReq(SOCKET s, DWORD charID, BYTE* /*payload*/, WORD /*size*/) {
    LOG("[FiveElmHandler] OnEndFiveElmReq: charID=" + std::to_string(charID));

    DWORD dwObjectID = charID + 400000000;
    DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
    if (!g_MapInstances.count(mapID)) return;
    CMapInstance* mapInst = g_MapInstances[mapID];

    sServerObject* player = nullptr;
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        player = mapInst->GetPlayer(dwObjectID);
    }
    if (!player) return;

    // 清理内存状态和五行 Buff 技能，并向客户端发送 0x402E 清除 Buff 图标
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        for (int i = 0; i < 5; ++i) {
            DWORD oldMugongID = 150 + i;
            if (player->activeBuffs.count(oldMugongID) > 0) {
                std::vector<BYTE> endAck(4 + 11);
                BYTE* ep = endAck.data() + 4;
                ep[0] = 0; // bResult
                *(DWORD*)(ep + 1) = dwObjectID;
                ep[5] = 1; // bObjectType (Player)
                *(DWORD*)(ep + 6) = oldMugongID;
                ep[10] = player->activeBuffs[oldMugongID].bLevel;
                PACKET_HEADER* headE = (PACKET_HEADER*)endAck.data();
                headE->id = 0x402E; // CS_BT_KEEPUPMUGONGEND_ACK
                headE->payloadSize = 11;
                EncryptPacket(endAck.data(), 0x42);
                mapInst->BroadcastPacket(endAck);
            }
            player->activeBuffs.erase(oldMugongID);
        }
        player->bCurFiveElm = 0;
        player->bFELevel = 0;
        player->dwLastFiveElmChangeTime = GetTickCount();
    }

    // 回复客户端关闭包
    std::vector<BYTE> ack(7);
    PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
    head->id = 0x3B73; // CS_IF_CHANGEFIVEELM_ACK
    head->payloadSize = 3;
    ack[4] = 0;
    ack[5] = 0;
    ack[6] = 0; // 关闭
    EncryptPacket(ack.data(), 0x42);
    SafeSend(s, (const char*)ack.data(), ack.size(), 0);

    // 广播关闭状态以移除特效
    BroadcastFiveElmStatus(player, 0, 0);

    LOG("[FiveElmHandler] Deactivated five element SUCCESS.");
}

extern void SendStaminaSync(SOCKET clientSocket, DWORD dwGauge);

void OnExecStaminaReq(SOCKET s, DWORD charID, BYTE* /*payload*/, WORD /*size*/) {
    LOG("[FiveElmHandler] OnExecStaminaReq: charID=" + std::to_string(charID));

    DWORD dwObjectID = charID + 400000000;
    DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
    if (!g_MapInstances.count(mapID)) return;
    CMapInstance* mapInst = g_MapInstances[mapID];

    sServerObject* player = nullptr;
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        player = mapInst->GetPlayer(dwObjectID);
    }
    if (!player) return;

    // 校验暴气条件：蓄力值必须满 5 格 (>= 5000)，且已激活五行 (bCurFiveElm > 0)
    if (player->dwFiveElmGauge < 5000 || player->bCurFiveElm == 0) {
        LOG("[FiveElmHandler] ExecStamina FAILED: Gauge=" + std::to_string(player->dwFiveElmGauge) + " ActiveElm=" + std::to_string(player->bCurFiveElm));
        
        std::vector<BYTE> ack(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
        head->id = 0x3B79; // CS_IF_EXECSTAMINA_ACK
        head->payloadSize = 1;
        ack[4] = 1; // ERR_EXECSTAMINA_NEEDSTAMINA
        EncryptPacket(ack.data(), 0x42);
        SafeSend(s, (const char*)ack.data(), ack.size(), 0);
        return;
    }

    // 满足条件，开始暴气
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        player->dwFiveElmGauge = 0;
        player->bSpiritActive = true;
        player->dwSpiritEndTime = GetTickCount() + 60000; // 60秒持续时间
    }

    // 更新到数据库
    CharacterDB::ExpData expData;
    if (CharacterDB::GetInstance().GetExpData(charID, expData)) {
        CharacterDB::GetInstance().UpdateFiveElm(charID, expData.wFiveElmPoint, expData.wFiveElmPointCnt, expData.dwFiveElmPower, 0,
                                                 expData.wFireExp, expData.wWaterExp, expData.wWoodExp, expData.wMetalExp, expData.wEarthExp);
    }

    // 同步蓄力值 0 状态给客户端
    SendStaminaSync(s, 0);

    // 广播开启暴气特效给 AoI 全图玩家 (bType = 11, dwData1 = 1, dwData2 = 1)
    std::vector<BYTE> buf;
    buf.resize(4, 0); // Header placeholder
    buf.push_back(11); // bType = 11 (Spirit/Potion)
    
    auto pushDWord = [&](DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back(d >> 24); };
    auto pushString = [&](const std::string& str) {
        WORD len = (WORD)str.length();
        buf.push_back(len & 0xFF); buf.push_back(len >> 8);
        for (char c : str) buf.push_back(c);
    };
    
    pushDWord(dwObjectID);
    pushDWord(1); // dwData1 -> bInstanceType = 1 (Spirit)
    pushDWord(1); // dwData2 -> 1 (Start spirit)
    pushDWord(0); // dwData3 -> 0
    pushString(""); pushString(""); pushString("");

    WORD packetID = 0x3F10; // CS_CD_CHARUPDATE_ACK
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);
    EncryptPacket(buf.data(), 0x42);

    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        mapInst->BroadcastPacket(buf);
    }

    // 重算玩家属性以使暴气属性加成生效，并在内部自动发送属性同步包给玩家
    PlayerManager::GetInstance().RecalculateStats(charID, true);

    // 回复客户端暴气成功包 (CS_IF_EXECSTAMINA_ACK = 0x3B79, Payload = 0)
    std::vector<BYTE> ack(5);
    PACKET_HEADER* head = (PACKET_HEADER*)ack.data();
    head->id = 0x3B79; // CS_IF_EXECSTAMINA_ACK
    head->payloadSize = 1;
    ack[4] = 0; // ERR_EXECSTAMINA_SUCCESS
    EncryptPacket(ack.data(), 0x42);
    SafeSend(s, (const char*)ack.data(), ack.size(), 0);

    LOG("[FiveElmHandler] ExecStamina SUCCESS for player " + std::to_string(charID));
}

// 判定五行相克关系：A 克制 B
static bool IsFEStrongCounter(BYTE attFE, BYTE defFE) {
    if (attFE == 1 && defFE == 4) return true; // 火克金
    if (attFE == 2 && defFE == 1) return true; // 水克火
    if (attFE == 3 && defFE == 5) return true; // 木克土
    if (attFE == 4 && defFE == 3) return true; // 金克木
    if (attFE == 5 && defFE == 2) return true; // 土克水
    return false;
}

// 二期：五行克制乘数动态计算
float CalculateFiveElmCounter(BYTE attFE, BYTE defFE, BYTE attFELevel, BYTE defFELevel, bool ignoreCounter) {
    if (attFE == 0 || defFE == 0) {
        return 1.00f; // 凡胎/无五行
    }
    if (attFE == defFE) {
        return 0.95f; // 同属性抵抗
    }
    if (IsFEStrongCounter(attFE, defFE)) {
        return 1.00f + 0.10f * attFELevel; // 强克制优势：每级被动增伤 10% (1级1.10, 3级1.30)
    }
    if (IsFEStrongCounter(defFE, attFE)) {
        if (ignoreCounter) {
            return 1.00f; // 首饰特技 8 消除被克制折损，保底 1.00
        }
        return 0.85f + 0.05f * defFELevel; // 被克制劣势：防御方被动等级抵消折损 (从 0.85 恢复至最高 1.00)
    }
    return 1.00f; // 中性五行
}

// 二期：五行伤害数值计算
DWORD CalculateFiveElmDamage(DWORD baseAtkExp, BYTE attFELevel, float fCounter, DWORD defenderLevel, WORD defFEExp, bool ignoreDef) {
    // 1. 计算五行攻击力
    double dAtk = (double)baseAtkExp * (1.0 + (double)attFELevel * 0.20);
    
    // 2. 计算五行防御力
    double dDef = 0.0;
    if (!ignoreDef) {
        if (defFEExp > 0 && defenderLevel == 0) {
            // 玩家防御分支 (传 level 为 0 时，根据玩家熟练度进行 / 10 判定)
            dDef = (double)defFEExp / 10.0;
        } else {
            // 怪物防御分支 (defenderLevel 为怪物等级，defFEExp 为怪物模板抗性值)
            dDef = (double)defenderLevel * 2.0 + (double)defFEExp;
        }
    }
    
    // 3. 伤害扣减
    double rawDmg = dAtk * (double)fCounter - dDef;
    if (rawDmg < 0.0) {
        rawDmg = 0.0;
    }
    
    return (DWORD)(rawDmg + 0.5); // 四舍五入后输出
}



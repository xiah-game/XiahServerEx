#include "MugongHandler.h"
#include "MugongAttackContext.h"
#include "../GameObjects/MugongManager.h"
#include "../GameObjects/PlayerManager.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../DBHelper.h"
#include "ItemHandler.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

// 系统提示发送助手函数
static void SendSystemMessage(SOCKET clientSocket, const std::string& text) {
    std::vector<BYTE> buf; buf.resize(4, 0);
    DWORD senderObjID = 0;
    buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
    buf.push_back(8); // CT_TIMEMESSAGE
    WORD len = (WORD)text.size();
    buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
    buf.insert(buf.end(), text.begin(), text.end());
    WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
}

// 宠物驯服确认包发送函数 (CS_NC_TAMING_ACK = 0x3512)
void SendTamingAck(SOCKET clientSocket, BYTE bResult, DWORD dwObjectID, BYTE bType) {
    std::vector<BYTE> buf;
    buf.resize(4, 0);
    buf.push_back(bResult);
    buf.push_back(dwObjectID & 0xFF);
    buf.push_back((dwObjectID >> 8) & 0xFF);
    buf.push_back((dwObjectID >> 16) & 0xFF);
    buf.push_back((dwObjectID >> 24) & 0xFF);
    buf.push_back(bType);

    PACKET_HEADER* head = (PACKET_HEADER*)buf.data();
    head->id = 0x3512;
    head->payloadSize = (WORD)(buf.size() - sizeof(PACKET_HEADER));
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
}

// 内部捕捉成功后的当场具现化与数据库登记
static bool OnCaptureSuccess(MugongAttackContext& ctx, MonsterData* pMon) {
    DWORD dwCharID = ctx.charID - 400000000;
    std::string monName = g_NpcTemplates[pMon->bPropType].szName;
    monName.erase(std::remove_if(monName.begin(), monName.end(), ::isspace), monName.end());

    // 1. 插入宠物数据至 CHAR_PET (分拆为 ExecuteUpdate 执行插入，避免 ODBC 多结果集 @@IDENTITY 漏洞)
    std::string qInsert = "INSERT INTO CHAR_PET ("
        "dwCharID, dwMapID, bNpcType, szName, wBasicLevel, wLevel, wPosX, wPosY, bHeight, wDirection, "
        "dwHpMax, dwHpCur, wAtkPwr, wDefPwr, wAtkRating, bAtkType, bSpeed, wMeleeAtkRange, wShotAtkRange, "
        "wAvoidRatio, dwJobPattern, dwMovePattern, dwAttackPattern, dwRefNpcID, bRevolutionStep, biExp, bCurJob, bWildRate, dateChange"
        ") VALUES ("
        + std::to_string(dwCharID) + ", "
        + std::to_string(ctx.playerMapID) + ", "
        + std::to_string(pMon->bPropType) + ", "
        + "'" + monName + "', "
        + "1, 1, "
        + std::to_string(pMon->wPosX) + ", "
        + std::to_string(pMon->wPosY) + ", "
        + std::to_string(pMon->bHeight) + ", 0, "
        + "200, 200, 20, 10, 50, 0, 8, 1, 0, 30, "
        + "0, 0, 0, "
        + std::to_string((pMon->dwObjectID - 200000) % 1000000) + ", "
        + "0, 0, 0, 0, GETDATE());";

    if (!DBHelper::GetInstance().ExecuteUpdate(qInsert)) {
        LOG("[PetCapture] Failed to execute INSERT INTO CHAR_PET!");
        return false;
    }

    DWORD dwPetID = 0;
    std::string qSelect = "SELECT MAX(dwID) FROM CHAR_PET WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteQuery(qSelect, [&](SQLHSTMT hStmt) {
        SQLLEN len;
        SQLGetData(hStmt, 1, SQL_C_ULONG, &dwPetID, 0, &len);
    });

    LOG("[PetCapture] Query last inserted pet: dwPetID=" + std::to_string(dwPetID));
    if (dwPetID == 0) return false;

    // 2. 将被捕获怪物生命值归 0 并安全注销
    CMapInstance* mapInst = g_MapInstances[ctx.playerMapID];
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        pMon->dwHpCur = 0;
        mapInst->HandleMonsterDoTDeath(GetTickCount(), *pMon, ctx.dwAttackID, ctx.dwMugongID, ctx.bMugongLevel);
    }

    // 3. 在地图原地实例化一个战宠 PlayerData 实体
    PlayerData petEntity;
    petEntity.dwObjectID = dwPetID + 800000000;
    petEntity.bObjectType = 4; // OBJTYPE_PET
    petEntity.dwMapID = ctx.playerMapID;
    petEntity.dwOwnerID = dwCharID;
    petEntity.bNpcType = pMon->bPropType;
    petEntity.wPosX = pMon->wPosX;
    petEntity.wPosY = pMon->wPosY;
    petEntity.bHeight = pMon->bHeight;
    petEntity.fPosX = (float)petEntity.wPosX;
    petEntity.fPosY = (float)petEntity.wPosY;
    petEntity.dwHpMax = 200;
    petEntity.dwHpCur = 200;
    petEntity.wWalkSpeed = 8;
    petEntity.wLevel = 1;
    petEntity.bRebirth = 0;
    petEntity.bPropType = pMon->bPropType;
    petEntity.bIsBunsin = false;
    petEntity.bNeedTamingAck = true;
    for (int v = 0; v < 6; v++) petEntity.wBunsinVisualID[v] = 0;

    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        mapInst->AddPlayer(petEntity);
    }

    // 4. 广播 0x3502 (进入地图) 让全地图及周围玩家看到这只被驯化的战宠
    {
        std::vector<BYTE> enterBuf(4);
        enterBuf.push_back(0); // bResult = 0
        DWORD mapID = ctx.playerMapID;
        enterBuf.push_back(mapID & 0xFF); enterBuf.push_back((mapID >> 8) & 0xFF);
        enterBuf.push_back((mapID >> 16) & 0xFF); enterBuf.push_back((mapID >> 24) & 0xFF);
        
        DWORD petObjID = petEntity.dwObjectID;
        enterBuf.push_back(petObjID & 0xFF); enterBuf.push_back((petObjID >> 8) & 0xFF);
        enterBuf.push_back((petObjID >> 16) & 0xFF); enterBuf.push_back((petObjID >> 24) & 0xFF);
        
        enterBuf.push_back(4); // bObjectType = 4 (PET)
        enterBuf.push_back(petEntity.wPosX & 0xFF); enterBuf.push_back(petEntity.wPosX >> 8);
        enterBuf.push_back(petEntity.wPosY & 0xFF); enterBuf.push_back(petEntity.wPosY >> 8);
        enterBuf.push_back(petEntity.bHeight);
        enterBuf.push_back(0); enterBuf.push_back(0); // wDirection
        enterBuf.push_back(0); // bStatus = Stand (0)
        enterBuf.push_back(petEntity.wWalkSpeed & 0xFF);

        PACKET_HEADER* enterHead = (PACKET_HEADER*)enterBuf.data();
        enterHead->id = 0x3502; // CS_NC_MAPENTER_ACK (0x3501), 修正此处 0x3502 的笔误，解决客户端解析崩溃
        enterHead->payloadSize = enterBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(enterBuf.data(), 0x42);
        BroadcastPacketToMap(ctx.playerMapID, enterBuf);
    }

    // 5. 向客户端发送 taming 成功确认，拉起客户端本地战宠实例化逻辑并播放音效和文字浮动
    // SendTamingAck(ctx.clientSocket, 0, petEntity.dwObjectID, 0); // 时序安全修正：移至 OnPetInfoReq 中发送

    LOG("[PetCapture] Successfully captured monster! dwPetID=" + std::to_string(dwPetID) + " NpcType=" + std::to_string(pMon->bPropType) + " Caster=" + std::to_string(dwCharID));
    return true;
}

// 捕捉普通宠物核心分发处理
bool HandlePetCapture(MugongAttackContext& ctx) {
    DWORD dwMugongID = ctx.dwMugongID;
    DWORD dwAttackID = ctx.dwAttackID;
    DWORD dwDefenseID = ctx.dwDefenseID;
    BYTE bDefenseType = ctx.bDefenseType;
    SOCKET clientSocket = ctx.clientSocket;
    DWORD playerMapID = ctx.playerMapID;
    BYTE bMugongLevel = ctx.bMugongLevel;
    sMugongList* pMugongData = ctx.pMugongData;

    // 1. 目标校验：必须是怪物
    if (bDefenseType != 3 || dwDefenseID == 0) {
        SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：只能对野生动物使用捕捉技能！");
        return true;
    }

    CMapInstance* mapInst = g_MapInstances[playerMapID];
    if (!mapInst) return true;

    // 出战宠物数量限制：只能同时出战一只战宠
    DWORD tempOwnerID = dwAttackID - 400000000;
    bool hasActivePet = false;
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        for (auto& pair : mapInst->GetPlayers()) {
            PlayerData& pl = pair.second;
            if (pl.dwOwnerID == tempOwnerID && pl.dwObjectID >= 800000000 && pl.dwObjectID < 850000000 && pl.dwHpCur > 0) {
                hasActivePet = true;
                break;
            }
        }
    }
    if (hasActivePet) {
        extern void SendTamingAck(SOCKET clientSocket, BYTE bResult, DWORD dwObjectID, BYTE bType);
        SendTamingAck(clientSocket, 3, 0, 0); // 3 = IDS_NOMORE_PET ("战宠数量已达上限")
        return true;
    }

    MonsterData* pMon = nullptr;
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        pMon = mapInst->GetMonster(dwDefenseID);
    }

    if (!pMon || pMon->dwHpCur == 0) {
        SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：目标已不存在或已死亡！");
        return true;
    }

    // 2. 自适应野生动物判定（放开名字校验限制，防范 GBK/UTF-8 字符匹配漏洞）
    bool isWildAnimal = true;

    // 【体验优化】施法消耗扣除与 IP 同步前置
    if (pMugongData && pMugongData->dwCostMp > 0) {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pPlayer = mapInst->GetPlayer(dwAttackID);
        if (pPlayer) {
            if (pPlayer->wIpCur < pMugongData->dwCostMp) {
                SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：你的内力不足，无法施展驯兽术！");
                return true;
            }
            pPlayer->wIpCur -= pMugongData->dwCostMp;
            
            std::vector<BYTE> hpBuf(4);
            auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
            push4(pPlayer->dwHpMax); push4(pPlayer->dwHpCur); push4(pPlayer->wIpMax); push4(pPlayer->wIpCur);
            hpBuf.push_back(0);
            PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
            hpHead->id = 0x3B0D; hpHead->payloadSize = hpBuf.size() - 4;
            EncryptPacket(hpBuf.data(), 0x42);
            SafeSend(clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);
        }
    }

    // 【体验优化】广播 0x4016 施法动画前置，防动作卡死
    std::vector<BYTE> ackBuf(4 + 38, 0);
    BYTE* p = ackBuf.data() + 4;
    p[0] = 0;
    *(DWORD*)(p + 1) = dwMugongID;
    p[5] = bMugongLevel;
    p[6] = 1;
    *(DWORD*)(p + 7) = dwAttackID;
    *(WORD*)(p + 11) = ctx.wAttackPosX;
    *(WORD*)(p + 13) = ctx.wAttackPosY;
    p[15] = ctx.bAttackHeight;
    p[16] = bDefenseType;
    *(DWORD*)(p + 17) = dwDefenseID;
    *(DWORD*)(p + 21) = pMon->dwHpMax;
    *(DWORD*)(p + 25) = pMon->dwHpCur;
    *(DWORD*)(p + 29) = 0;
    *(DWORD*)(p + 33) = 0;
    p[37] = 0;
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4016; head->payloadSize = 38;
    EncryptPacket(ackBuf.data(), 0x42);
    BroadcastPacketToMap(playerMapID, ackBuf);

    if (!isWildAnimal) {
        SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：该怪物无法被驯化为宠物！");
        SendTamingAck(clientSocket, 4, dwDefenseID, 0);
        return true;
    }

    DWORD dwCharID = ctx.charID - 400000000;
    DWORD dwMirrorItemID = 0;
    BYTE bMirrorSackPos = 0;
    std::string qMirror = "SELECT TOP 1 S.dwItemID, S.bSackPos FROM SACKITEM S INNER JOIN ITEM I ON S.dwItemID = I.dwItemID LEFT JOIN ITEMDATA D ON S.dwItemID = D.dwItemID WHERE S.dwCharID = " + std::to_string(dwCharID) + " AND I.wRefID IN (SELECT wRefID FROM ITEMTEMPLATE WHERE bType = 9) AND (D.nData15 IS NULL OR D.nData15 = 0)";
    DBHelper::GetInstance().ExecuteQuery(qMirror, [&](SQLHSTMT hStmt) {
        SQLLEN c1, c2;
        SQLGetData(hStmt, 1, SQL_C_ULONG, &dwMirrorItemID, 0, &c1);
        SQLGetData(hStmt, 2, SQL_C_UTINYINT, &bMirrorSackPos, 0, &c2);
    });

    if (dwMirrorItemID == 0) {
        SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：你的包裹里没有空的龙魂镜，无法驯服宠物！");
        SendTamingAck(clientSocket, 4, dwDefenseID, 0);
        return true;
    }

    double baseRate = (pMugongData && pMugongData->wSuccessRatePerc > 0) ? 
        (double)pMugongData->wSuccessRatePerc : (50.0 + 5.0 * bMugongLevel);
    double hpRatio = (double)pMon->dwHpCur / (double)pMon->dwHpMax;
    double bonus = (1.0 - hpRatio);
    if (hpRatio < 0.2) bonus += 0.3;
    double finalRate = baseRate * (1.0 + bonus);
    if (finalRate > 95.0) finalRate = 95.0;

    int randVal = rand() % 100;
    bool isSuccess = (randVal < finalRate);

    LOG("[PetCaptureDebug] dwMugongID=" + std::to_string(dwMugongID)
        + " bMugongLevel=" + std::to_string(bMugongLevel)
        + " baseRate=" + std::to_string(baseRate) 
        + " hpCur=" + std::to_string(pMon->dwHpCur) 
        + " hpMax=" + std::to_string(pMon->dwHpMax) 
        + " hpRatio=" + std::to_string(hpRatio)
        + " bonus=" + std::to_string(bonus)
        + " finalRate=" + std::to_string(finalRate)
        + " randVal=" + std::to_string(randVal)
        + " isSuccess=" + std::to_string(isSuccess));

    if (!isSuccess) {
        // 向客户端发送驯服失败包，客户端会自动飘红字 "宠物驯服失败" 并播放音效
        SendTamingAck(clientSocket, 4, dwDefenseID, 0);
        return true;
    }

    // 7. 执行当场具现化宠物成功
    if (!OnCaptureSuccess(ctx, pMon)) {
        SendTamingAck(clientSocket, 4, dwDefenseID, 0);
        SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：驯化数据写入异常！");
    }

    return true;
}

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

// 内部捕捉成功后的数据库绑定与实体销毁，控制单个函数圈复杂度在 40 行内
static bool OnCaptureSuccess(MugongAttackContext& ctx, MonsterData* pMon, DWORD dwMirrorItemID, BYTE bMirrorSackPos) {
    DWORD dwCharID = ctx.charID - 400000000;
    std::string monName = g_NpcTemplates[pMon->bPropType].szName;
    monName.erase(std::remove_if(monName.begin(), monName.end(), ::isspace), monName.end());

    // 1. 插入宠物数据至 CHAR_PET (所有者设为 0 表示镜内挂起)
    DWORD dwPetID = 0;
    std::string qInsert = "SET NOCOUNT ON; INSERT INTO CHAR_PET (dwCharID, dwMapID, bNpcType, szName, wLevel, wPosX, wPosY, bHeight, dwHpMax, dwHpCur, wAtkPwr, wDefPwr, wAtkRating, wAvoidRatio, bSpeed, wMeleeAtkRange, wShotAtkRange, bAtkType, dwRefNpcID, bCurJob, biExp, bRevolutionStep, bWildRate) VALUES (0, 0, "
        + std::to_string(pMon->bPropType) + ", '" + monName + "', 1, 0, 0, 0, 200, 200, 20, 10, 50, 30, 8, 1, 0, 0, "
        + std::to_string(pMon->bPropType) + ", 0, 0, 0, 0); SELECT @@IDENTITY;";
    DBHelper::GetInstance().ExecuteQuery(qInsert, [&](SQLHSTMT hStmt) {
        SQLLEN len;
        SQLGetData(hStmt, 1, SQL_C_ULONG, &dwPetID, 0, &len);
    });

    if (dwPetID == 0) return false;

    // 2. 将龙魂镜与宠物 dwPetID 绑定
    bool dataExists = false;
    DBHelper::GetInstance().ExecuteQuery("SELECT 1 FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwMirrorItemID), [&](SQLHSTMT) { dataExists = true; });
    if (dataExists) {
        DBHelper::GetInstance().ExecuteUpdate("UPDATE ITEMDATA SET nData1 = " + std::to_string(dwPetID) + ", nData15 = 1 WHERE dwItemID = " + std::to_string(dwMirrorItemID));
    } else {
        std::string qItemData = "INSERT INTO ITEMDATA (dwItemID, nData1, nData2, nData3, nData4, nData5, nData6, nData7, nData8, nData9, nData10, nData11, nData12, nData13, nData14, nData15, nData16, nData17, nData18, nData19, nData20) VALUES ("
            + std::to_string(dwMirrorItemID) + ", " + std::to_string(dwPetID) + ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0)";
        DBHelper::GetInstance().ExecuteUpdate(qItemData);
    }

    // 3. 将被捕获怪物生命值归 0 并安全注销
    CMapInstance* mapInst = g_MapInstances[ctx.playerMapID];
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        pMon->dwHpCur = 0;
        mapInst->HandleMonsterDoTDeath(GetTickCount(), *pMon, ctx.dwAttackID, ctx.dwMugongID, ctx.bMugongLevel);
    }

    // 4. 强制刷新玩家包裹以显示已封印镜子
    BYTE bSackID = bMirrorSackPos / 20;
    BYTE reqPayload[1] = { bSackID };
    OnItemListReq(ctx.clientSocket, dwCharID, reqPayload, 1);

    SendSystemMessage(ctx.clientSocket, "[驯兽] 捕捉成功！你成功将 [" + monName + "] 驯化并封印在龙魂镜中！");
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

    MonsterData* pMon = nullptr;
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        pMon = mapInst->GetMonster(dwDefenseID);
    }

    if (!pMon || pMon->dwHpCur == 0) {
        SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：目标已不存在或已死亡！");
        return true;
    }

    // 2. 自适应野生动物名字判断
    std::string monName = g_NpcTemplates[pMon->bPropType].szName;
    bool isWildAnimal = (
        monName.find("兔") != std::string::npos || monName.find("蛇") != std::string::npos ||
        monName.find("狼") != std::string::npos || monName.find("猪") != std::string::npos ||
        monName.find("雕") != std::string::npos || monName.find("豹") != std::string::npos ||
        monName.find("虎") != std::string::npos || monName.find("熊") != std::string::npos ||
        monName.find("猫") != std::string::npos || monName.find("犬") != std::string::npos ||
        monName.find("狗") != std::string::npos || monName.find("马") != std::string::npos ||
        monName.find("猴") != std::string::npos || monName.find("鹿") != std::string::npos ||
        monName.find("牛") != std::string::npos
    );

    if (!isWildAnimal) {
        SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：该怪物无法被驯化为宠物！");
        return true;
    }

    DWORD dwCharID = ctx.charID - 400000000;

    // 3. 包裹中寻找空的龙魂镜 (bType=9 且 nData1=0)
    DWORD dwMirrorItemID = 0;
    BYTE bMirrorSackPos = 0;
    std::string qMirror = "SELECT TOP 1 S.dwItemID, S.bSackPos FROM SACKITEM S INNER JOIN ITEM I ON S.dwItemID = I.dwItemID LEFT JOIN ITEMDATA D ON S.dwItemID = D.dwItemID WHERE S.dwCharID = " + std::to_string(dwCharID) + " AND I.wRefID IN (SELECT wRefID FROM ITEMTEMPLATE WHERE bType = 9) AND (D.nData1 IS NULL OR D.nData1 = 0)";
    DBHelper::GetInstance().ExecuteQuery(qMirror, [&](SQLHSTMT hStmt) {
        SQLLEN c1, c2;
        SQLGetData(hStmt, 1, SQL_C_ULONG, &dwMirrorItemID, 0, &c1);
        SQLGetData(hStmt, 2, SQL_C_UTINYINT, &bMirrorSackPos, 0, &c2);
    });

    if (dwMirrorItemID == 0) {
        SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：你的包裹里没有空的龙魂镜！");
        return true;
    }

    // 4. 施法消耗扣除与 IP 同步
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

    // 5. 动态成功率计算 (血量越低，成功率越高)
    double baseRate = (pMugongData && pMugongData->wSuccessRatePerc > 0) ? (double)pMugongData->wSuccessRatePerc : 20.0;
    double hpRatio = (double)pMon->dwHpCur / (double)pMon->dwHpMax;
    double bonus = (1.0 - hpRatio);
    if (hpRatio < 0.2) bonus += 0.3;
    double finalRate = baseRate * (1.0 + bonus);
    if (finalRate > 95.0) finalRate = 95.0;

    bool isSuccess = ((rand() % 100) < finalRate);

    // 6. 广播 0x4016 施法动画，防止客户端动作卡死
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

    if (!isSuccess) {
        SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：野生动物挣脱了束缚逃跑了！");
        return true;
    }

    // 7. 执行捕捉成功
    if (!OnCaptureSuccess(ctx, pMon, dwMirrorItemID, bMirrorSackPos)) {
        SendSystemMessage(clientSocket, "[驯兽] 捕捉失败：驯化数据写入异常！");
    }

    return true;
}

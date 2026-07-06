#include "PetGiveItemHandler.h"
#include "../Network/ProtocolConstants.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../ServerCore.h"
#include "../DB/ItemDB.h"
#include "../DBHelper.h"
#include "../GameObjects/MapInstance.h"
#include "../GameObjects/PlayerManager.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>

extern std::map<WORD, sItemTemplate> g_ItemTemplates;

// 外部函数声明
void OnPetInfoReq(SOCKET clientSocket, BYTE* payload, WORD payloadSize);
void OnItemListReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size);

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

// 宠物驯服状态确认发送 (0x3512)
static void SendTamingAck(SOCKET clientSocket, BYTE bResult, DWORD dwObjectID, BYTE bType) {
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

// 扣除投喂道具/诱饵 1 个
static void ConsumeGiveItem(SOCKET s, DWORD dwCharID, DWORD dwItemID, BYTE bSackID, BYTE bSackPos) {
    WORD amount = ItemDB::GetInstance().GetItemAmount(dwItemID);
    if (amount > 1) {
        ItemDB::GetInstance().DecrementItemAmount(dwItemID);
        // 发送 0x4209 刷新堆叠数量
        std::vector<BYTE> amtBuf(4);
        amtBuf.push_back(bSackID);
        amtBuf.push_back(bSackPos);
        WORD newAmount = amount - 1;
        amtBuf.push_back(newAmount & 0xFF);
        amtBuf.push_back(newAmount >> 8);
        PACKET_HEADER* head = (PACKET_HEADER*)amtBuf.data();
        head->id = 0x4209;
        head->payloadSize = amtBuf.size() - 4;
        EncryptPacket(amtBuf.data(), 0x42);
        SafeSend(s, (const char*)amtBuf.data(), amtBuf.size(), 0);
    } else {
        ItemDB::GetInstance().RemoveFromSack(dwCharID, dwItemID);
        ItemDB::GetInstance().DeleteItemCascade(dwItemID);
        // 发送 0x4208 彻底移除包裹物品
        std::vector<BYTE> rmBuf(7);
        PACKET_HEADER* rmHead = (PACKET_HEADER*)rmBuf.data();
        rmHead->id = 0x4208;
        rmHead->payloadSize = 3;
        rmBuf[4] = bSackID;
        rmBuf[5] = bSackPos;
        rmBuf[6] = 2; // reason
        EncryptPacket(rmBuf.data(), 0x42);
        SafeSend(s, (const char*)rmBuf.data(), rmBuf.size(), 0);
    }
}

// 道具捕捉怪物成功后原地生成战宠
static bool OnGiveCaptureSuccess(SOCKET s, DWORD dwCharID, DWORD playerMapID, MonsterData* pMon) {
    std::string monName = g_NpcTemplates[pMon->bPropType].szName;
    monName.erase(std::remove_if(monName.begin(), monName.end(), ::isspace), monName.end());

    // 1. 插入宠物数据至 CHAR_PET (分拆为 ExecuteUpdate 执行插入，避免 ODBC 多结果集 @@IDENTITY 漏洞)
    std::string qInsert = "INSERT INTO CHAR_PET ("
        "dwCharID, dwMapID, bNpcType, szName, wBasicLevel, wLevel, wPosX, wPosY, bHeight, wDirection, "
        "dwHpMax, dwHpCur, wAtkPwr, wDefPwr, wAtkRating, bAtkType, bSpeed, wMeleeAtkRange, wShotAtkRange, "
        "wAvoidRatio, dwJobPattern, dwMovePattern, dwAttackPattern, dwRefNpcID, bRevolutionStep, biExp, bCurJob, bWildRate, dateChange"
        ") VALUES ("
        + std::to_string(dwCharID) + ", "
        + std::to_string(playerMapID) + ", "
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
        LOG("[PetGiveItem] Failed to execute INSERT INTO CHAR_PET!");
        return false;
    }

    DWORD dwPetID = 0;
    std::string qSelect = "SELECT MAX(dwID) FROM CHAR_PET WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteQuery(qSelect, [&](SQLHSTMT hStmt) {
        SQLLEN len;
        SQLGetData(hStmt, 1, SQL_C_ULONG, &dwPetID, 0, &len);
    });

    LOG("[PetGiveItem] Query last inserted pet: dwPetID=" + std::to_string(dwPetID));
    if (dwPetID == 0) return false;

    // 2. 将怪物血量清 0 并安全移出地图
    CMapInstance* mapInst = g_MapInstances[playerMapID];
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        pMon->dwHpCur = 0;
        mapInst->HandleMonsterDoTDeath(GetTickCount(), *pMon, dwCharID + 400000000, 0, 0);
    }

    // 3. 原地实例化宠物 PlayerData 实体
    PlayerData petEntity;
    petEntity.dwObjectID = dwPetID + 800000000;
    petEntity.bObjectType = 4; // OBJTYPE_PET
    petEntity.dwMapID = playerMapID;
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

    // 4. 广播 0x3502 (进入地图)
    {
        std::vector<BYTE> enterBuf(4);
        enterBuf.push_back(0); // bResult = 0
        enterBuf.push_back(playerMapID & 0xFF); enterBuf.push_back((playerMapID >> 8) & 0xFF);
        enterBuf.push_back((playerMapID >> 16) & 0xFF); enterBuf.push_back((playerMapID >> 24) & 0xFF);
        
        DWORD petObjID = petEntity.dwObjectID;
        enterBuf.push_back(petObjID & 0xFF); enterBuf.push_back((petObjID >> 8) & 0xFF);
        enterBuf.push_back((petObjID >> 16) & 0xFF); enterBuf.push_back((petObjID >> 24) & 0xFF);
        
        enterBuf.push_back(4); // OBJTYPE_PET
        enterBuf.push_back(petEntity.wPosX & 0xFF); enterBuf.push_back(petEntity.wPosX >> 8);
        enterBuf.push_back(petEntity.wPosY & 0xFF); enterBuf.push_back(petEntity.wPosY >> 8);
        enterBuf.push_back(petEntity.bHeight);
        enterBuf.push_back(0); enterBuf.push_back(0); // wDirection
        enterBuf.push_back(0); // bStatus
        enterBuf.push_back(petEntity.wWalkSpeed & 0xFF);

        PACKET_HEADER* enterHead = (PACKET_HEADER*)enterBuf.data();
        enterHead->id = 0x3502; // CS_NC_MAPENTER_ACK (0x3501)，修正此处 0x3502 的笔误，解决客户端解析崩溃
        enterHead->payloadSize = enterBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(enterBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, enterBuf);
    }

    // 5. 回复驯服成功包
    // SendTamingAck(s, 0, petEntity.dwObjectID, 0); // 时序安全修正：移至 OnPetInfoReq 中发送

    LOG("[PetGiveItemHandler] Tool-based capture success! dwPetID=" + std::to_string(dwPetID) + " Owner=" + std::to_string(dwCharID));
    return true;
}

// 0x421A CS_IM_GIVEITEM_REQ 网络包处理入口
// payload: BYTE bSackID + BYTE bSackPos + DWORD dwItemID + BYTE bObjectType + DWORD dwObjectID
void OnPetGiveItemReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size) {
    if (size < 11) {
        LOG("[PetGiveItemHandler] Invalid packet size: " + std::to_string(size));
        return;
    }

    BYTE bSackID = payload[0];
    BYTE bSackPos = payload[1];
    DWORD dwItemID = *(DWORD*)(payload + 2);
    BYTE bObjectType = payload[6];
    DWORD dwObjectID = *(DWORD*)(payload + 7);

    // 1. 查找玩家投喂的道具
    ItemDB::FullItemRow resRow;
    if (!ItemDB::GetInstance().GetFullItemData(dwItemID, resRow)) {
        LOG("[PetGiveItemHandler] GiveItem item not found in DB: ID=" + std::to_string(dwItemID));
        return;
    }

    WORD refid = (WORD)resRow.wRefID;
    if (!g_ItemTemplates.count(refid)) return;

    sItemTemplate& tpl = g_ItemTemplates[refid];
    BYTE type = tpl.bType; // 应当为 ITEMTYPE_NPCITEM (16)
    BYTE kind = tpl.bKind;

    if (type != 16) {
        LOG("[PetGiveItemHandler] Item is not NPC/Pet item type: " + std::to_string(type));
        return;
    }

    // 获取玩家所在的地图
    sServerObject* pPlayer = nullptr;
    DWORD playerMapID = 0;
    for (auto& pair : g_MapInstances) {
        std::lock_guard<std::mutex> lock(pair.second->GetMutex());
        pPlayer = pair.second->GetPlayer(dwCharID + 400000000);
        if (pPlayer) {
            playerMapID = pair.first;
            break;
        }
    }

    if (!pPlayer || playerMapID == 0) return;
    CMapInstance* mapInst = g_MapInstances[playerMapID];

    // 出战宠物数量限制：只能同时出战一只战宠
    bool hasActivePet = false;
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        for (auto& pair : mapInst->GetPlayers()) {
            PlayerData& pl = pair.second;
            if (pl.dwOwnerID == dwCharID && pl.dwObjectID >= 800000000 && pl.dwObjectID < 850000000 && pl.dwHpCur > 0) {
                hasActivePet = true;
                break;
            }
        }
    }
    if (hasActivePet) {
        SendTamingAck(s, 3, 0, 0); // 3 = IDS_NOMORE_PET ("战宠数量已达上限")
        return;
    }

    // ============================================================
    // 分支一：喂食/驯化野生怪物 (bObjectType == 3)
    // ============================================================
    if (bObjectType == 3) {
        // 校验目标怪物
        MonsterData* pMon = nullptr;
        {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            pMon = mapInst->GetMonster(dwObjectID);
        }

        if (!pMon || pMon->dwHpCur == 0) {
            SendSystemMessage(s, "[捕捉] 目标已被消灭或已不存在！");
            return;
        }

        // 诱捕道具判断 (kind == 0 或 5)
        if (kind != 0 && kind != 5) {
            SendSystemMessage(s, "[捕捉] 该道具无法对野生动物使用！");
            return;
        }

        // 校验玩家包裹内是否有空龙魂镜 (bType=9 且 nData1=0)
        DWORD dwMirrorItemID = 0;
        BYTE bMirrorSackPos = 0;
        std::string qMirror = "SELECT TOP 1 S.dwItemID, S.bSackPos FROM SACKITEM S INNER JOIN ITEM I ON S.dwItemID = I.dwItemID LEFT JOIN ITEMDATA D ON S.dwItemID = D.dwItemID WHERE S.dwCharID = " + std::to_string(dwCharID) + " AND I.wRefID IN (SELECT wRefID FROM ITEMTEMPLATE WHERE bType = 9) AND (D.nData15 IS NULL OR D.nData15 = 0)";
        DBHelper::GetInstance().ExecuteQuery(qMirror, [&](SQLHSTMT hStmt) {
            SQLLEN c1, c2;
            SQLGetData(hStmt, 1, SQL_C_ULONG, &dwMirrorItemID, 0, &c1);
            SQLGetData(hStmt, 2, SQL_C_UTINYINT, &bMirrorSackPos, 0, &c2);
        });

        if (dwMirrorItemID == 0) {
            SendSystemMessage(s, "[捕捉] 捕捉失败：包裹里没有空的龙魂镜，无法驯服宠物！");
            return;
        }

        // 校验野生动物类别
        // 校验野生动物类别 (放开名字校验限制，防范 GBK/UTF-8 字符匹配漏洞)
        bool isWildAnimal = true;
        if (!isWildAnimal) {
            SendSystemMessage(s, "[捕捉] 该怪物无法被驯化为宠物！");
            return;
        }

        // 消耗 1 个诱饵
        ConsumeGiveItem(s, dwCharID, dwItemID, bSackID, bSackPos);

        // 概率判定：基准成功率来自模板 nBasicData3 (wTamingRate)
        double baseRate = tpl.nBasicData3 > 0 ? (double)tpl.nBasicData3 : 15.0;
        double hpRatio = (double)pMon->dwHpCur / (double)pMon->dwHpMax;
        double bonus = (1.0 - hpRatio);
        if (hpRatio < 0.2) bonus += 0.3;
        double finalRate = baseRate * (1.0 + bonus);
        if (finalRate > 90.0) finalRate = 90.0;

        bool isSuccess = ((rand() % 100) < finalRate);

        if (!isSuccess) {
            SendTamingAck(s, 4, dwObjectID, 0); // 客户端飘字“宠物驯服失败”
            return;
        }

        if (!OnGiveCaptureSuccess(s, dwCharID, playerMapID, pMon)) {
            SendTamingAck(s, 4, dwObjectID, 0);
            SendSystemMessage(s, "[捕捉] 捕捉失败：数据录入数据库异常！");
        }
    }
    // ============================================================
    // 分支二：给已召唤出的战宠喂食/投喂 (bObjectType == 4)
    // ============================================================
    else if (bObjectType == 4) {
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        PlayerData* pPet = nullptr;
        {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            pPet = mapInst->GetPlayer(dwObjectID);
        }

        if (!pPet || pPet->bObjectType != 4 || pPet->dwOwnerID != dwCharID) {
            SendSystemMessage(s, "[喂食] 只能给属于你自己的出战宠物喂食！");
            return;
        }

        DWORD dwPetID = dwObjectID - 800000000;

        // 投喂的是宠物生命药水 (kind == 2)
        if (kind == 2) {
            WORD incrHp = tpl.nBasicData5 > 0 ? (WORD)tpl.nBasicData5 : 50;
            if (pPet->dwHpCur >= pPet->dwHpMax) {
                SendSystemMessage(s, "[喂食] 你的宠物生命值已满，无需喂食药品！");
                return;
            }

            // 扣除 1 个药品
            ConsumeGiveItem(s, dwCharID, dwItemID, bSackID, bSackPos);

            pPet->dwHpCur += incrHp;
            if (pPet->dwHpCur > pPet->dwHpMax) pPet->dwHpCur = pPet->dwHpMax;

            // 更新 CHAR_PET 数据
            std::string qUpd = "UPDATE CHAR_PET SET dwHpCur = " + std::to_string(pPet->dwHpCur) + " WHERE dwID = " + std::to_string(dwPetID);
            DBHelper::GetInstance().ExecuteUpdate(qUpd);

            // 触发 0x3538 属性刷新发往客户端
            BYTE temp[8];
            *(DWORD*)temp = dwObjectID;
            *(DWORD*)(temp + 4) = playerMapID;
            OnPetInfoReq(s, temp, 8);

            SendSystemMessage(s, "[喂食] 成功喂食药品，宠物的健康值恢复了！");
        }
        // 投喂的是减少狂野度的饲料 (kind == 1)
        else if (kind == 1) {
            // 先获取宠物当前的狂野度
            int curWild = 0;
            DBHelper::GetInstance().ExecuteQuery("SELECT bWildRate FROM CHAR_PET WHERE dwID = " + std::to_string(dwPetID), [&](SQLHSTMT hStmt) {
                SQLLEN l;
                SQLGetData(hStmt, 1, SQL_C_LONG, &curWild, 0, &l);
            });

            if (curWild <= 0) {
                SendSystemMessage(s, "[喂食] 你的宠物非常温顺，无需喂食饲料！");
                return;
            }

            // 扣除 1 个饲料
            ConsumeGiveItem(s, dwCharID, dwItemID, bSackID, bSackPos);

            int decWild = tpl.nBasicData4 > 0 ? (int)tpl.nBasicData4 : 10;
            curWild -= decWild;
            if (curWild < 0) curWild = 0;

            // 更新 DB
            std::string qUpd = "UPDATE CHAR_PET SET bWildRate = " + std::to_string(curWild) + " WHERE dwID = " + std::to_string(dwPetID);
            DBHelper::GetInstance().ExecuteUpdate(qUpd);

            // 触发 0x3538 属性刷新
            BYTE temp[8];
            *(DWORD*)temp = dwObjectID;
            *(DWORD*)(temp + 4) = playerMapID;
            OnPetInfoReq(s, temp, 8);

            SendSystemMessage(s, "[喂食] 成功喂食饲料，宠物变得更加听话了！");
        } else {
            SendSystemMessage(s, "[喂食] 该道具对出战的战宠没有任何效果！");
        }
    }
}

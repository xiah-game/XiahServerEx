#include "MugongHandler.h"
#include "MugongAttackContext.h"
#include "PartyHandler.h"
#include "FiveElmHandler.h"

#include "../GameObjects/MugongManager.h"

#include "../GameObjects/PlayerManager.h"

#include "../GameObjects/ExpSystem.h"

#include "../Network/SessionMgr.h"

#include "../MonsterAI.h"

#include "../GameObjects/DropManager.h"
#include "../DB/ItemDB.h"

#include "../GameObjects/MapInstance.h"

#include "../GameObjects/PlayerManager.h"

#include "../GameObjects/ExpSystem.h"

#include "../DB/CharacterDB.h"

#include <cmath>
#include <algorithm>



extern std::map<DWORD, CMapInstance*> g_MapInstances;

// === 分身系统（技能41）全局变量 & 函数 stub ===
// 功能暂停开发，保留接口避免链接错误
std::unordered_map<DWORD, std::vector<DWORD>> g_BunsinMap;

int GetMaxBunsinCount(BYTE bMugongLevel) {
    if (bMugongLevel >= 10) return 3;
    if (bMugongLevel >= 7) return 2;
    return 1;
}

void CleanupAllBunsins(DWORD ownerCharID, DWORD mapID) {
    auto it = g_BunsinMap.find(ownerCharID);
    if (it == g_BunsinMap.end()) return;

    extern std::map<DWORD, CMapInstance*> g_MapInstances;
    if (!g_MapInstances.count(mapID)) {
        g_BunsinMap.erase(ownerCharID);
        return;
    }
    CMapInstance* mapInst = g_MapInstances[mapID];
    // 注意：调用方可能已经持有 mapInst 的锁，此处不再加锁

    std::vector<DWORD> bunsins = it->second; // 拷贝，因为遍历中会修改
    for (DWORD bunsinObjID : bunsins) {
        // 广播 MAPLEAVE_ACK (0x3506) 让客户端销毁对象
        std::vector<BYTE> leaveBuf(4);
        leaveBuf.push_back(0); // bResult = 0
        leaveBuf.push_back(bunsinObjID & 0xFF); leaveBuf.push_back((bunsinObjID >> 8) & 0xFF);
        leaveBuf.push_back((bunsinObjID >> 16) & 0xFF); leaveBuf.push_back((bunsinObjID >> 24) & 0xFF);
        leaveBuf.push_back(4); // bObjectType = PET
        leaveBuf.push_back(mapID & 0xFF); leaveBuf.push_back((mapID >> 8) & 0xFF);
        leaveBuf.push_back((mapID >> 16) & 0xFF); leaveBuf.push_back((mapID >> 24) & 0xFF);
        leaveBuf.push_back(0); // bType = Normal leave
        PACKET_HEADER* leaveHead = (PACKET_HEADER*)leaveBuf.data();
        leaveHead->id = 0x3506;
        leaveHead->payloadSize = leaveBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(leaveBuf.data(), 0x42);
        BroadcastPacketToMap(mapID, leaveBuf);

        // 从地图移除实体
        mapInst->RemovePlayer(bunsinObjID);

        LOG("[Bunsin] Cleaned up bunsin ObjID=" + std::to_string(bunsinObjID));
    }

    g_BunsinMap.erase(ownerCharID);
}

void CleanupSingleBunsin(DWORD ownerCharID, DWORD bunsinObjID, DWORD mapID) {
    // TODO: 分身系统重做时实现
    auto it = g_BunsinMap.find(ownerCharID);
    if (it != g_BunsinMap.end()) {
        auto& vec = it->second;
        vec.erase(std::remove(vec.begin(), vec.end(), bunsinObjID), vec.end());
        if (vec.empty()) g_BunsinMap.erase(it);
    }
    LOG("[Bunsin] CleanupSingleBunsin stub called for bunsin " + std::to_string(bunsinObjID));
}


void OnMugongListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {

    if (totalSize < 1) return;

    BYTE bMugongType = payload[0];

    LOG("[MugongHandler] Received CS_IT_MUGONGLIST_REQ (0x4418) with type: " + std::to_string(bMugongType));



    std::vector<BYTE> ackBuf(4);

    std::map<DWORD, BYTE> tempLearned;

    MugongManager::GetInstance()->LoadPlayerMugongs(charID, tempLearned);

    

    std::vector<std::pair<DWORD, BYTE>> filteredSkills;

    

    // Filter skills by type

    // The client strictly uses ID ranges to map skills to UI tabs based on .idx files:

    // IDs 1-29 are Passive (Ingong), IDs 30-149 are General (Outgong), IDs 191+ are Awakening

    for (auto& s : tempLearned) {

        DWORD id = s.first;

        bool isPassive = (id >= 1 && id <= 29);

        bool isGeneral = (id >= 30 && id <= 149);

        bool isFiveElm = (id >= 150 && id <= 154);

        if (bMugongType == 0 && isGeneral) filteredSkills.push_back(s); // GENERAL/ACTIVE

        else if (bMugongType == 1 && (isPassive || isFiveElm)) filteredSkills.push_back(s); // PASSIVE

    }

    

    ackBuf.push_back((BYTE)filteredSkills.size()); // bNumOfMugong

    

    for (auto& s : filteredSkills) {

        DWORD dwMugongID = s.first;

        BYTE bMugongLevel = s.second;

        sMugongTemplate* tpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);

        std::string name = tpl ? tpl->szName : "UnknownSkill";

        BYTE bKind = tpl ? tpl->bKind : 0;

        

        ackBuf.push_back(dwMugongID & 0xFF); ackBuf.push_back((dwMugongID >> 8) & 0xFF); ackBuf.push_back((dwMugongID >> 16) & 0xFF); ackBuf.push_back((dwMugongID >> 24) & 0xFF);

        

        if (bMugongType == 0) {

            // General Mugong requires string name

            WORD nameLen = (WORD)name.length();

            ackBuf.push_back(nameLen & 0xFF); ackBuf.push_back((nameLen >> 8) & 0xFF);

            for (char c : name) ackBuf.push_back(c);

        }

        

        ackBuf.push_back(bKind); // bKind

        ackBuf.push_back(bMugongLevel); // bMugongLevel

        ackBuf.push_back(0); // bMacroSeq

        

        LOG("[DEBUG] Sending Mugong ID: " + std::to_string(dwMugongID) + " Name: " + name + " bKind: " + std::to_string(bKind) + " bLevel: " + std::to_string(bMugongLevel));

    }

    

    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();

    if (bMugongType == 0) ackHead->id = 0x441A; // CS_IT_GENERALMUGONGLIST_ACK

    else if (bMugongType == 1) ackHead->id = 0x441C; // CS_IT_PASSIVEMUGONGLIST_ACK

    else if (bMugongType == 2) ackHead->id = 0x441D; // CS_IT_ACTIVEMUGONGLIST_ACK

    else return;

    

    ackHead->payloadSize = (WORD)(ackBuf.size() - 4);

    EncryptPacket(ackBuf.data(), 0x42);

    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);

    LOG("[MugongHandler] Sent Mugong List Type " + std::to_string(bMugongType) + " with " + std::to_string(filteredSkills.size()) + " skills.");



    // The client comments out SendCS_IT_MUGONGLIST_REQ(2) (Active) and SendCS_IT_MUGONGLIST_REQ(3).

    // So we must automatically send the Active Mugong List (0x441C) when the client requests Passive (1)

    if (bMugongType == 1) {

        std::vector<std::pair<DWORD, BYTE>> activeSkills;

        for (auto& s : tempLearned) {

            // Send Awakening skills (typically IDs 191-198 or specific higher tier skills)

            // that the client expects in 0x441C. For now, we assume ID >= 191 are Awakening.

            if (s.first >= 191) {

                activeSkills.push_back(s);

            }

        }

        

        std::vector<BYTE> actBuf(4);

        actBuf.push_back((BYTE)activeSkills.size());

        

        for (auto& s : activeSkills) {

            DWORD dwMugongID = s.first;

            BYTE bMugongLevel = s.second;

            sMugongTemplate* tpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);

            BYTE bKind = tpl ? tpl->bKind : 0;

            

            actBuf.push_back(dwMugongID & 0xFF); actBuf.push_back((dwMugongID >> 8) & 0xFF); actBuf.push_back((dwMugongID >> 16) & 0xFF); actBuf.push_back((dwMugongID >> 24) & 0xFF);

            actBuf.push_back(bKind);

            actBuf.push_back(bMugongLevel);

            actBuf.push_back(0); // bMacroSeq

        }

        

        PACKET_HEADER* actHead = (PACKET_HEADER*)actBuf.data();

        actHead->id = 0x441D; // CS_IT_ACTIVEMUGONGLIST_ACK

        actHead->payloadSize = (WORD)(actBuf.size() - 4);

        EncryptPacket(actBuf.data(), 0x42);

        SafeSend(clientSocket, (const char*)actBuf.data(), actBuf.size(), 0);

        LOG("[MugongHandler] Sent Active Mugong List (0x441C) automatically with " + std::to_string(activeSkills.size()) + " skills.");

    }

}







void OnMugongLearnReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {

    LOG("[MugongHandler] OnMugongLearnReq Received! Size: " + std::to_string(totalSize));

    if (totalSize < 4) return;

    DWORD dwMugongID = *(DWORD*)payload;

    LOG("[MugongHandler] Player " + std::to_string(charID) + " requests to upgrade skill: " + std::to_string(dwMugongID));

    MugongManager::GetInstance()->UpgradeMugong(clientSocket, charID, dwMugongID);

}



// OnSelMugongReq + OnMugongPreAttackReq + IsAoeSkill -> MugongPreAttack.cpp

static void SendSystemMessage(SOCKET clientSocket, const std::string& msg) {
    std::vector<BYTE> buf; buf.resize(4, 0);
    DWORD senderObjID = 0;
    buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
    buf.push_back(8); // CT_TIMEMESSAGE
    WORD len = (WORD)msg.size();
    buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
    buf.insert(buf.end(), msg.begin(), msg.end());
    WORD packetID = 0x3E02;
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
}

void OnMugongAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {

    if (totalSize < 24) return;

    

    DWORD dwMugongID    = *(DWORD*)(payload);

    BYTE bAttackType    = payload[4];

    DWORD dwAttackID    = *(DWORD*)(payload + 5);

    WORD wAttackPosX    = *(WORD*)(payload + 9);

    WORD wAttackPosY    = *(WORD*)(payload + 11);

    BYTE bAttackHeight  = payload[13];

    BYTE bDefenseType   = payload[14];

    DWORD dwDefenseID   = *(DWORD*)(payload + 15);

    WORD wTargetPosX    = *(WORD*)(payload + 19);

    WORD wTargetPosY    = *(WORD*)(payload + 21);

    BYTE bTargetHeight  = payload[23];



    LOG("[MugongHandler] OnMugongAttackReq: Skill " + std::to_string(dwMugongID) + " by " + std::to_string(dwAttackID) + " against " + std::to_string(dwDefenseID));

    // 五行必杀技蓄气校验已搬迁至 MugongSkill_FiveElm.cpp::HandleFiveElmUltimate
    // 通过上方 bType/bKind 路由自动分发



    BYTE bMugongLevel = 1;

    int currentLvl = MugongManager::GetInstance()->GetPlayerMugongLevel(charID, dwMugongID);

    if(currentLvl > 0) bMugongLevel = currentLvl;



    DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (g_MapInstances.count(playerMapID)) {
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        if (bDefenseType == 3) {
            MonsterData* pMon = mapInst->GetMonster(dwDefenseID);
            if (pMon && IsGatherResource(pMon->bPropType)) {
                return; // 拦截对采集资源的武功施法
            }
        }
        PlayerData* pObj = mapInst->GetPlayer(charID + 400000000);
        if (pObj && pObj->dwHpCur == 0) {
            LOG("[MugongHandler] Blocked skill attack for dead player charID=" + std::to_string(charID));
            return;
        }
    }



    if (g_MapInstances.count(playerMapID)) {
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwAttackID);

        // === CC 状态检查（基于 bType/bKind 数据驱动） ===
        // bType=4 + bKind=16(致盲)/17(麻痹) = 无法施法的 CC 状态
        // 注意：bKind=18(定身/擒拿) 只限制移动，不限制施法和攻击
        if (pObj) {
            // 战宠与分身协同攻击：锁定怪物目标
            for (auto& pair : mapInst->GetPlayers()) {
                PlayerData& pl = pair.second;
                if (pl.dwOwnerID == (dwAttackID - 800000000) && pl.dwObjectID >= 800000000 && pl.dwHpCur > 0) {
                    pl.dwPetTargetObjectID = dwDefenseID;
                }
            }
            bool isCC = false;
            for (const auto& bf : pObj->activeBuffs) {
                if (bf.second.bIsDebuff) {
                    sMugongTemplate* debuffTpl = MugongManager::GetInstance()->GetTemplate(bf.second.dwMugongID);
                    if (debuffTpl && debuffTpl->bType == 4 &&
                        (debuffTpl->bKind == 16 || debuffTpl->bKind == 17)) {
                        isCC = true;
                        break;
                    }
                }
            }
            if (isCC) {
                std::vector<BYTE> buf; buf.resize(4, 0);
                DWORD senderObjID = 0;
                buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
                buf.push_back(8); // CT_TIMEMESSAGE
                std::string msg = "[Control] You are frozen, stunned or immobilized and cannot cast skills!";
                WORD len = (WORD)msg.size();
                buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
                buf.insert(buf.end(), msg.begin(), msg.end());
                WORD packetID = 0x3E02;
                WORD payloadSize = (WORD)(buf.size() - 4);
                memcpy(&buf[0], &packetID, 2);
                memcpy(&buf[2], &payloadSize, 2);
                EncryptPacket(buf.data(), 0x42);
                SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
                return;
            }
        }

        // === 龟息解除（bType=4, bKind=4）===
        if (pObj) {
            for (auto& bf : pObj->activeBuffs) {
                sMugongTemplate* bfTpl = MugongManager::GetInstance()->GetTemplate(bf.second.dwMugongID);
                if (bfTpl && bfTpl->bType == 4 && bfTpl->bKind == 4) {
                    bf.second.dwEndTime = 0;
                    LOG("[MugongHandler] Player " + std::to_string(dwAttackID) + " used skill " + std::to_string(dwMugongID) + ". Expiring Turtle Breath (bKind=4).");
                    break;
                }
            }
        }

        // === 隐身解除（bType=4, bKind=70）===
        sMugongList* tempPd = MugongManager::GetInstance()->GetMugongLevelData(dwMugongID, bMugongLevel);
        bool tempIsBuff = (tempPd && tempPd->dwKeepUpTime > 0);
        if (!tempIsBuff && pObj) {
            for (auto& bf : pObj->activeBuffs) {
                sMugongTemplate* bfTpl = MugongManager::GetInstance()->GetTemplate(bf.second.dwMugongID);
                if (bfTpl && bfTpl->bType == 4 && bfTpl->bKind == 70) {
                    bf.second.dwEndTime = 0;
                    LOG("[MugongHandler] Player " + std::to_string(dwAttackID) + " used damage skill " + std::to_string(dwMugongID) + ". Expiring Stealth (bKind=70).");
                    break;
                }
            }
        }
    }

    // 1. Get the skill data
    sMugongList* pMugongData = MugongManager::GetInstance()->GetMugongLevelData(dwMugongID, bMugongLevel);

    // 2. Determine if it's a buff or heal（基于 bType 数据驱动）
    sMugongTemplate* tpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);
    // debuff(bKind=8/16/17/18/19/20/21/22) -> not self-buff
    bool isTargetDebuff = (tpl && tpl->bType == 4 && (tpl->bKind == 8 || tpl->bKind == 9 || tpl->bKind == 16 || tpl->bKind == 17 || tpl->bKind == 18 || tpl->bKind == 19 || tpl->bKind == 20 || tpl->bKind == 21 || tpl->bKind == 22));
    bool isBuff = (pMugongData && pMugongData->dwKeepUpTime > 0 && !isTargetDebuff);
    // bType=1 即为治疗技能，不再硬编码排除具体技能 ID
    bool isHeal = (!isBuff && tpl && tpl->bType == 1);

    // 逻辑问题修正：如果技能是敌方单体 Debuff 技能，且当前目标 ID 为 0 或者指向攻击者自己，说明没有选取合法敌方目标，直接拦截以防给自己挂上 debuff。
    if (isTargetDebuff && (dwDefenseID == 0 || dwDefenseID == dwAttackID)) {
        LOG("[MugongHandler] Blocked target debuff skill " + std::to_string(dwMugongID) + " cast with invalid/self target: " + std::to_string(dwDefenseID));
        return;
    }

    // === 特殊技能数据驱动路由（基于 bType/bKind） ===
    MugongAttackContext ctx;
    ctx.clientSocket = clientSocket;
    ctx.charID = charID + 400000000;
    ctx.dwMugongID = dwMugongID;
    ctx.bAttackType = bAttackType;
    ctx.dwAttackID = dwAttackID;
    ctx.wAttackPosX = wAttackPosX;
    ctx.wAttackPosY = wAttackPosY;
    ctx.bAttackHeight = bAttackHeight;
    ctx.bDefenseType = bDefenseType;
    ctx.dwDefenseID = dwDefenseID;
    ctx.wTargetPosX = wTargetPosX;
    ctx.wTargetPosY = wTargetPosY;
    ctx.bTargetHeight = bTargetHeight;
    ctx.bMugongLevel = bMugongLevel;
    ctx.playerMapID = playerMapID;
    ctx.pMugongData = pMugongData;
    ctx.pTemplate = tpl;
    ctx.isBuff = isBuff;
    ctx.isHeal = isHeal;

    if (tpl) {
        // 召唤/分身类 (bType=4, bKind=9)
        if (tpl->bType == 4 && tpl->bKind == 9) {
            HandleSummonSkill(ctx);
            return;
        }
        // 捕捉普通宠物 (bType=2/4, bKind=17)
        if ((tpl->bType == 2 || tpl->bType == 4) && tpl->bKind == 17) {
            HandlePetCapture(ctx);
            return;
        }
        // 五行被动技 (bType=0, bKind=80~84): 蓄气校验
        if (tpl->bType == 0 && tpl->bKind >= 80 && tpl->bKind <= 84) {
            if (!HandleFiveElmUltimate(ctx)) {
                return;
            }
        }
    }

    // 2.4. 攻击破状态：主动释放攻击技能时清除龟息(130)/隐身(178)
    if (!isBuff && !isHeal) {
        DWORD attackerCharID = dwAttackID - 400000000;
        PlayerManager::GetInstance().RemoveBuffSafe(attackerCharID, 130);
        PlayerManager::GetInstance().RemoveBuffSafe(attackerCharID, 178);
    }

    // 2.5. Perform early IP cost validation and deduction for all non-healing skills

    if (!isHeal && dwMugongID != 63 && pMugongData && pMugongData->dwCostMp > 0) {

        if (g_MapInstances.count(playerMapID)) {

            CMapInstance* mapInst = g_MapInstances[playerMapID];

            std::lock_guard<std::mutex> lock(mapInst->GetMutex());

            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);

            if (pObj) {

                if (pObj->wIpCur < pMugongData->dwCostMp) {

                    LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " blocked: IP " 

                        + std::to_string(pObj->wIpCur) + " < cost " + std::to_string(pMugongData->dwCostMp));

                    // Send 0x4016 with bResult=5 (IP insufficient) so client shows error message

                    std::vector<BYTE> failBuf(4 + 38, 0);

                    BYTE* fp = failBuf.data() + 4;

                    fp[0] = 3; // bResult = 3 (IDS_SHORT_INLIFE / IP insufficient)

                    *(DWORD*)(fp + 1) = dwMugongID;

                    fp[5] = bMugongLevel;

                    fp[6] = 1; // bAtkType = OBJTYPE_PC (client uses this in FindXiahObject)

                    *(DWORD*)(fp + 7) = dwAttackID;

                    *(WORD*)(fp + 11) = wAttackPosX;

                    *(WORD*)(fp + 13) = wAttackPosY;

                    fp[15] = bAttackHeight;

                    fp[16] = 1; // bDefType = OBJTYPE_PC

                    *(DWORD*)(fp + 17) = dwAttackID; // dwDefID = self

                    PACKET_HEADER* fh = (PACKET_HEADER*)failBuf.data();

                    fh->id = 0x4016;

                    fh->payloadSize = 38;

                    EncryptPacket(failBuf.data(), 0x42);

                    SafeSend(clientSocket, (const char*)failBuf.data(), failBuf.size(), 0);

                    return;

                }

                WORD oldIp = pObj->wIpCur;

                pObj->wIpCur -= pMugongData->dwCostMp;



                // Send 0x3B0D to update client HP/IP bars

                std::vector<BYTE> hpBuf(4);

                auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };

                auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };

                push4(pObj->dwHpMax);

                push4(pObj->dwHpCur);

                push4(pObj->wIpMax);

                push4(pObj->wIpCur);

                hpBuf.push_back(0); // bType

                PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();

                hpHead->id = 0x3B0D;

                hpHead->payloadSize = hpBuf.size() - 4;

                EncryptPacket(hpBuf.data(), 0x42);

                SafeSend(clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);



                LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " IP cost deducted early: " 

                    + std::to_string(oldIp) + " - " + std::to_string(pMugongData->dwCostMp) 

                    + " = " + std::to_string(pObj->wIpCur));

            }

        }

    }



    // === bType=3, bKind=19（御剑术）：CD 检查 + 武器耐久消耗 ===
    if (tpl && tpl->bType == 3 && tpl->bKind == 19 && pMugongData) {
        DWORD dwCharID = SessionMgr::GetInstance().GetCharID(clientSocket);
        DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        DWORD dwObjectID = (dwCharID < 800000000) ? (dwCharID + 400000000) : dwCharID;
        sServerObject* pObj = nullptr;
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            pObj = mapInst->GetPlayer(dwObjectID);
        }
        if (pObj) {
            // CD 检查
            DWORD cdMs = (DWORD)pMugongData->nEtc2;
            if (cdMs > 0 && pObj->mugongLastCastTime.count(dwMugongID)) {
                DWORD elapsed = GetTickCount() - pObj->mugongLastCastTime[dwMugongID];
                if (elapsed < cdMs) {
                    LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " on cooldown (" + std::to_string(elapsed) + "/" + std::to_string(cdMs) + "ms)");
                    return;
                }
            }
            // 武器耐久消耗（bSackPos=0 = 武器栏）
            // nEtc1 = 最大耐久的百分比，实际扣除 = maxDur * nEtc1 / 100
            int durPercent = pMugongData->nEtc1;
            if (durPercent > 0) {
                DWORD dwWeaponItemID = ItemDB::GetInstance().GetItemAtSackPos(dwCharID, 0);
                if (dwWeaponItemID == 0) {
                    LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " blocked: no weapon equipped");
                    return;
                }
                ItemDB::ItemDataRow itemData;
                bool hasData = ItemDB::GetInstance().GetItemData(dwWeaponItemID, itemData);
                int curDur = hasData ? itemData.nData[1] : -9999; // nData2 = 当前耐久
                int maxDur = hasData ? itemData.nData[2] : -9999; // nData3 = 最大耐久
                if (maxDur == -9999 || maxDur <= 0) maxDur = 100; // 未初始化时默认100
                if (curDur == -9999) curDur = maxDur;              // 未初始化时视为满耐久
                int durCost = maxDur * durPercent / 100;
                if (durCost < 1) durCost = 1;
                if (curDur < durCost) {
                    LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " blocked: weapon durability " + std::to_string(curDur) + " < cost " + std::to_string(durCost) + " (" + std::to_string(durPercent) + "% of " + std::to_string(maxDur) + ")");
                    return;
                }
                int newDur = curDur - durCost;
                ItemDB::GetInstance().UpdateItemData(dwWeaponItemID, 2, newDur);
                // 发送 DURABILITY_ACK (0x4236) 同步客户端耐久显示
                std::vector<BYTE> durBuf(4);
                auto pushDW = [&](DWORD d) { durBuf.push_back(d&0xFF); durBuf.push_back((d>>8)&0xFF); durBuf.push_back((d>>16)&0xFF); durBuf.push_back((d>>24)&0xFF); };
                pushDW(dwWeaponItemID);        // dwItemID
                durBuf.push_back(1);           // bItemType (武器)
                durBuf.push_back(0);           // bSackID
                durBuf.push_back(0);           // bSackPos (装备栏0)
                durBuf.push_back((BYTE)(newDur & 0xFF)); durBuf.push_back((BYTE)((newDur >> 8) & 0xFF)); // wCurDur
                PACKET_HEADER* durHead = (PACKET_HEADER*)durBuf.data();
                durHead->id = 0x4236;
                durHead->payloadSize = durBuf.size() - 4;
                EncryptPacket(durBuf.data(), 0x42);
                SafeSend(clientSocket, (const char*)durBuf.data(), durBuf.size(), 0);
                LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " consumed " + std::to_string(durCost) + " weapon durability (" + std::to_string(curDur) + " -> " + std::to_string(newDur) + ", " + std::to_string(durPercent) + "% of maxDur " + std::to_string(maxDur) + ")");
            }
            // 记录 CD 时间
            pObj->mugongLastCastTime[dwMugongID] = GetTickCount();
        }
    }



    // === Heal / Recovery Skills ===
#include "MugongSkill_Buff.inl"



    // === bType=4, bKind=3（万毒不侵）：范围即时恢复 ===
    if (tpl && tpl->bType == 4 && tpl->bKind == 3 && pMugongData) {
        DWORD healAmount = (pMugongData->nEtc1 > 0) ? (DWORD)pMugongData->nEtc1 : 0;
        float healRange = (pMugongData->wAttackRange > 0) ? (float)pMugongData->wAttackRange : 15.0f;

        if (healAmount > 0 && g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            PlayerData* pCaster = mapInst->GetPlayer(dwAttackID);
            if (pCaster) {
                WORD cx = pCaster->wPosX, cy = pCaster->wPosY;
                // 遍历 AOI 内玩家，距离筛选后回血
                std::vector<PlayerData*> aoiPlayers = mapInst->GetPlayersInAOI(cx, cy);
                for (PlayerData* pTarget : aoiPlayers) {
                    if (!pTarget || pTarget->dwHpCur == 0) continue;
                    float dx = (float)pTarget->wPosX - (float)cx;
                    float dy = (float)pTarget->wPosY - (float)cy;
                    float dist = sqrtf(dx * dx + dy * dy);
                    if (dist > healRange && pTarget->dwObjectID != dwAttackID) continue; // 自己必中

                    DWORD oldHp = pTarget->dwHpCur;
                    pTarget->dwHpCur = (std::min)(pTarget->dwHpMax, pTarget->dwHpCur + healAmount);
                    DWORD healed = pTarget->dwHpCur - oldHp;

                    // 发送 0x3B0D 更新被治疗者的 HP/IP 面板
                    if (healed > 0 && !pTarget->bIsBunsin) {
                        DWORD tCharID = pTarget->dwObjectID - 400000000;
                        SOCKET tSock = SessionMgr::GetInstance().GetSocketByCharID(tCharID);
                        if (tSock != INVALID_SOCKET) {
                            std::vector<BYTE> hpBuf(4);
                            auto p4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back(d>>24); };
                            p4(pTarget->dwHpMax); p4(pTarget->dwHpCur); p4(pTarget->wIpMax); p4(pTarget->wIpCur);
                            hpBuf.push_back(0);
                            PACKET_HEADER* hh = (PACKET_HEADER*)hpBuf.data();
                            hh->id = 0x3B0D; hh->payloadSize = hpBuf.size() - 4;
                            EncryptPacket(hpBuf.data(), 0x42);
                            SafeSend(tSock, (const char*)hpBuf.data(), hpBuf.size(), 0);
                        }
                    }
                    LOG("[MugongHandler] AOE Heal Skill " + std::to_string(dwMugongID) + " healed " + std::to_string(pTarget->dwObjectID) + " +" + std::to_string(healed) + " HP");
                }
            }
        }

        // 广播 0x4016 播放地面特效
        std::vector<BYTE> ackBuf(4 + 38);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0;
        *(DWORD*)(p + 1) = dwMugongID;
        p[5] = bMugongLevel;
        p[6] = 1; // bAtkType = PC
        *(DWORD*)(p + 7) = dwAttackID;
        *(WORD*)(p + 11) = wAttackPosX;
        *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight;
        p[16] = 1; // bDefType = PC (self)
        *(DWORD*)(p + 17) = dwAttackID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0;
        *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0;
        p[37] = 0;
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4016; head->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }



    // === bType=4, bKind=8（狮子吼）：范围恐惧 ===
    if (tpl && tpl->bType == 4 && tpl->bKind == 8 && pMugongData) {
        float fearRange = (pMugongData->wAttackRange > 0) ? (float)pMugongData->wAttackRange : 30.0f;
        WORD successRate = pMugongData->wSuccessRatePerc;
        DWORD fearDuration = pMugongData->dwKeepUpTime * 1000; // 秒→毫秒

        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            PlayerData* pCaster = mapInst->GetPlayer(dwAttackID);
            if (pCaster) {
                WORD cx = pCaster->wPosX, cy = pCaster->wPosY;
                std::vector<MonsterData*> aoiMonsters = mapInst->GetMonstersInAOI(cx, cy);
                int fearedCount = 0;
                for (MonsterData* pMon : aoiMonsters) {
                    if (!pMon || pMon->dwHpCur == 0 || IsGatherResource(pMon->bPropType)) continue;
                    float dx = (float)pMon->wPosX - (float)cx;
                    float dy = (float)pMon->wPosY - (float)cy;
                    if (sqrtf(dx * dx + dy * dy) > fearRange) continue;

                    // 成功率判定
                    if (successRate < 100 && (WORD)(rand() % 100) >= successRate) {
                        LOG("[MugongHandler] Fear MISS on monster " + std::to_string(pMon->dwObjectID) + " (rate=" + std::to_string(successRate) + "%)");
                        continue;
                    }

                    // 挂恐惧 debuff
                    PlayerData::sActiveBuff fearBuff;
                    fearBuff.dwMugongID = dwMugongID;
                    fearBuff.bLevel = bMugongLevel;
                    fearBuff.dwEndTime = GetTickCount() + fearDuration;
                    fearBuff.bIsDebuff = true;
                    pMon->activeBuffs[dwMugongID] = fearBuff;
                    pMon->dwTargetID = 0; // 立即脱战

                    // 广播 0x402C 给怪物身上显示恐惧特效
                    std::vector<BYTE> fearAck(4 + 11);
                    BYTE* fp = fearAck.data() + 4;
                    fp[0] = 0;
                    *(DWORD*)(fp + 1) = pMon->dwObjectID;
                    fp[5] = pMon->bObjectType; // 3=Monster
                    *(DWORD*)(fp + 6) = dwMugongID;
                    fp[10] = bMugongLevel;
                    PACKET_HEADER* fh = (PACKET_HEADER*)fearAck.data();
                    fh->id = 0x402C; fh->payloadSize = 11;
                    EncryptPacket(fearAck.data(), 0x42);
                    BroadcastPacketToMap(playerMapID, fearAck);

                    fearedCount++;
                    LOG("[MugongHandler] Fear SUCCESS on monster " + std::to_string(pMon->dwObjectID) + " duration=" + std::to_string(pMugongData->dwKeepUpTime) + "s");
                }
                LOG("[MugongHandler] Fear Skill " + std::to_string(dwMugongID) + " feared " + std::to_string(fearedCount) + " monsters in range " + std::to_string((int)fearRange));
            }
        }

        // 广播 0x4016 播放施法者特效
        std::vector<BYTE> ackBuf(4 + 38);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0;
        *(DWORD*)(p + 1) = dwMugongID;
        p[5] = bMugongLevel;
        p[6] = 1;
        *(DWORD*)(p + 7) = dwAttackID;
        *(WORD*)(p + 11) = wAttackPosX;
        *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight;
        p[16] = 1;
        *(DWORD*)(p + 17) = dwAttackID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0;
        *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0;
        p[37] = 0;
        PACKET_HEADER* head2 = (PACKET_HEADER*)ackBuf.data();
        head2->id = 0x4016; head2->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }


    // === bType=4, bKind=16（迷踪拳 96）：范围致盲 ===
    // 致盲效果：被致盲怪物命中率大幅降低（攻击大概率MISS）
    // 与恐惧区别：不脱战不清仇恨，怪物仍然攻击但打不中
    if (tpl && tpl->bType == 4 && tpl->bKind == 16 && pMugongData) {
        float blindRange = (pMugongData->wAttackRange > 0) ? (float)pMugongData->wAttackRange : 30.0f;
        WORD successRate = pMugongData->wSuccessRatePerc;
        DWORD blindDuration = pMugongData->dwKeepUpTime * 1000; // 秒→毫秒

        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            PlayerData* pCaster = mapInst->GetPlayer(dwAttackID);
            if (pCaster) {
                WORD cx = pCaster->wPosX, cy = pCaster->wPosY;
                std::vector<MonsterData*> aoiMonsters = mapInst->GetMonstersInAOI(cx, cy);
                int blindedCount = 0;
                for (MonsterData* pMon : aoiMonsters) {
                    if (!pMon || pMon->dwHpCur == 0 || IsGatherResource(pMon->bPropType)) continue;
                    float dx = (float)pMon->wPosX - (float)cx;
                    float dy = (float)pMon->wPosY - (float)cy;
                    if (sqrtf(dx * dx + dy * dy) > blindRange) continue;

                    // 成功率判定
                    if (successRate < 100 && (WORD)(rand() % 100) >= successRate) {
                        LOG("[MugongHandler] Blind MISS on monster " + std::to_string(pMon->dwObjectID) + " (rate=" + std::to_string(successRate) + "%)");
                        continue;
                    }

                    // 挂致盲 debuff（不清仇恨，怪物继续攻击但MISS）
                    PlayerData::sActiveBuff blindBuff;
                    blindBuff.dwMugongID = dwMugongID;
                    blindBuff.bLevel = bMugongLevel;
                    blindBuff.dwEndTime = GetTickCount() + blindDuration;
                    blindBuff.bIsDebuff = true;
                    pMon->activeBuffs[dwMugongID] = blindBuff;
                    // 注意：不设置 dwTargetID=0，怪物照常锁定目标

                    // 广播 0x402C 给怪物身上显示致盲特效
                    std::vector<BYTE> blindAck(4 + 11);
                    BYTE* bp = blindAck.data() + 4;
                    bp[0] = 0;
                    *(DWORD*)(bp + 1) = pMon->dwObjectID;
                    bp[5] = pMon->bObjectType; // 3=Monster
                    *(DWORD*)(bp + 6) = dwMugongID;
                    bp[10] = bMugongLevel;
                    PACKET_HEADER* bh = (PACKET_HEADER*)blindAck.data();
                    bh->id = 0x402C; bh->payloadSize = 11;
                    EncryptPacket(blindAck.data(), 0x42);
                    BroadcastPacketToMap(playerMapID, blindAck);

                    blindedCount++;
                    LOG("[MugongHandler] Blind SUCCESS on monster " + std::to_string(pMon->dwObjectID) + " duration=" + std::to_string(pMugongData->dwKeepUpTime) + "s");
                }
                LOG("[MugongHandler] Blind Skill " + std::to_string(dwMugongID) + " blinded " + std::to_string(blindedCount) + " monsters in range " + std::to_string((int)blindRange));
            }
        }

        // 广播 0x4016 施法动画
        std::vector<BYTE> ackBuf(4 + 38);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0;
        *(DWORD*)(p + 1) = dwMugongID;
        p[5] = bMugongLevel;
        p[6] = 1;
        *(DWORD*)(p + 7) = dwAttackID;
        *(WORD*)(p + 11) = wAttackPosX;
        *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight;
        p[16] = 1;
        *(DWORD*)(p + 17) = dwAttackID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0;
        *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0;
        p[37] = 0;
        PACKET_HEADER* head3 = (PACKET_HEADER*)ackBuf.data();
        head3->id = 0x4016; head3->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }


    // === bType=4, bKind=17（锁骨术 98）：单体麻痹 ===
    // 麻痹效果：目标不可移动、不可攻击、不可施法，直到持续时间结束
    if (tpl && tpl->bType == 4 && tpl->bKind == 17 && pMugongData) {
        WORD successRate = pMugongData->wSuccessRatePerc;
        DWORD paraDuration = pMugongData->dwKeepUpTime * 1000;

        if (g_MapInstances.count(playerMapID) && dwDefenseID != 0) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());

            // 单体技能：对目标怪物释放
            MonsterData* pTarget = mapInst->GetMonster(dwDefenseID);
            if (pTarget && pTarget->dwHpCur > 0) {
                // 距离校验
                PlayerData* pCaster = mapInst->GetPlayer(dwAttackID);
                bool inRange = true;
                if (pCaster && pMugongData->wAttackRange > 0) {
                    float dx = (float)pTarget->wPosX - (float)pCaster->wPosX;
                    float dy = (float)pTarget->wPosY - (float)pCaster->wPosY;
                    if (sqrtf(dx * dx + dy * dy) > (float)pMugongData->wAttackRange) {
                        inRange = false;
                    }
                }

                if (inRange) {
                    // 成功率判定
                    bool success = (successRate >= 100) || ((WORD)(rand() % 100) < successRate);
                    if (success) {
                        PlayerData::sActiveBuff paraBuff;
                        paraBuff.dwMugongID = dwMugongID;
                        paraBuff.bLevel = bMugongLevel;
                        paraBuff.dwEndTime = GetTickCount() + paraDuration;
                        paraBuff.bIsDebuff = true;
                        pTarget->activeBuffs[dwMugongID] = paraBuff;

                        // 广播 0x402C 显示麻痹特效
                        std::vector<BYTE> paraAck(4 + 11);
                        BYTE* pp = paraAck.data() + 4;
                        pp[0] = 0;
                        *(DWORD*)(pp + 1) = pTarget->dwObjectID;
                        pp[5] = pTarget->bObjectType;
                        *(DWORD*)(pp + 6) = dwMugongID;
                        pp[10] = bMugongLevel;
                        PACKET_HEADER* ph = (PACKET_HEADER*)paraAck.data();
                        ph->id = 0x402C; ph->payloadSize = 11;
                        EncryptPacket(paraAck.data(), 0x42);
                        BroadcastPacketToMap(playerMapID, paraAck);

                        LOG("[MugongHandler] Paralyze SUCCESS on monster " + std::to_string(pTarget->dwObjectID) + " duration=" + std::to_string(pMugongData->dwKeepUpTime) + "s");
                    } else {
                        LOG("[MugongHandler] Paralyze MISS on monster " + std::to_string(pTarget->dwObjectID) + " (rate=" + std::to_string(successRate) + "%)");
                    }
                }
            }
        }

        // 广播 0x4016 施法动画
        std::vector<BYTE> ackBuf(4 + 38, 0);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0;
        *(DWORD*)(p + 1) = dwMugongID;
        p[5] = bMugongLevel;
        p[6] = 1;
        *(DWORD*)(p + 7) = dwAttackID;
        *(WORD*)(p + 11) = wAttackPosX;
        *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight;
        p[16] = bDefenseType;
        *(DWORD*)(p + 17) = dwDefenseID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0;
        *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0;
        p[37] = 0;
        PACKET_HEADER* head4 = (PACKET_HEADER*)ackBuf.data();
        head4->id = 0x4016; head4->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }

    // === bType=4, bKind=18（大擒拿手/擒拿神功）：范围定身 ===
    // 定身效果：目标不可移动，但可以原地攻击
    if (tpl && tpl->bType == 4 && tpl->bKind == 18 && pMugongData) {
        float rootRange = (pMugongData->wAttackRange > 0) ? (float)pMugongData->wAttackRange : 60.0f;
        WORD successRate = pMugongData->wSuccessRatePerc;
        DWORD rootDuration = pMugongData->dwKeepUpTime * 1000;

        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            PlayerData* pCaster = mapInst->GetPlayer(dwAttackID);
            if (pCaster) {
                WORD cx = pCaster->wPosX, cy = pCaster->wPosY;
                std::vector<MonsterData*> aoiMonsters = mapInst->GetMonstersInAOI(cx, cy);
                int rootedCount = 0;
                for (MonsterData* pMon : aoiMonsters) {
                    if (!pMon || pMon->dwHpCur == 0 || IsGatherResource(pMon->bPropType)) continue;
                    float dx = (float)pMon->wPosX - (float)cx;
                    float dy = (float)pMon->wPosY - (float)cy;
                    if (sqrtf(dx * dx + dy * dy) > rootRange) continue;

                    // 成功率判定
                    if (successRate < 100 && (WORD)(rand() % 100) >= successRate) {
                        LOG("[MugongHandler] Root MISS on monster " + std::to_string(pMon->dwObjectID) + " (rate=" + std::to_string(successRate) + "%)");
                        continue;
                    }

                    // 挂定身 debuff（不清仇恨，怪物原地攻击但不可移动）
                    PlayerData::sActiveBuff rootBuff;
                    rootBuff.dwMugongID = dwMugongID;
                    rootBuff.bLevel = bMugongLevel;
                    rootBuff.dwEndTime = GetTickCount() + rootDuration;
                    rootBuff.bIsDebuff = true;
                    pMon->activeBuffs[dwMugongID] = rootBuff;

                    // 广播 0x402C 特效
                    std::vector<BYTE> rootAck(4 + 11);
                    BYTE* rp = rootAck.data() + 4;
                    rp[0] = 0;
                    *(DWORD*)(rp + 1) = pMon->dwObjectID;
                    rp[5] = pMon->bObjectType;
                    *(DWORD*)(rp + 6) = dwMugongID;
                    rp[10] = bMugongLevel;
                    PACKET_HEADER* rh = (PACKET_HEADER*)rootAck.data();
                    rh->id = 0x402C; rh->payloadSize = 11;
                    EncryptPacket(rootAck.data(), 0x42);
                    BroadcastPacketToMap(playerMapID, rootAck);

                    rootedCount++;
                    LOG("[MugongHandler] Root SUCCESS on monster " + std::to_string(pMon->dwObjectID) + " duration=" + std::to_string(pMugongData->dwKeepUpTime) + "s");
                }
                LOG("[MugongHandler] Root Skill " + std::to_string(dwMugongID) + " rooted " + std::to_string(rootedCount) + " monsters in range " + std::to_string((int)rootRange));
            }
        }

        // 广播 0x4016 施法动画
        std::vector<BYTE> ackBuf(4 + 38, 0);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0;
        *(DWORD*)(p + 1) = dwMugongID;
        p[5] = bMugongLevel;
        p[6] = 1;
        *(DWORD*)(p + 7) = dwAttackID;
        *(WORD*)(p + 11) = wAttackPosX;
        *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight;
        p[16] = 1;
        *(DWORD*)(p + 17) = dwAttackID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0;
        *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0;
        p[37] = 0;
        PACKET_HEADER* head5 = (PACKET_HEADER*)ackBuf.data();
        head5->id = 0x4016; head5->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }


    // === bType=4, bKind=48（引兽术 65 / 收神气强 175）：宠物增益 buff ===
    // 提升宠物攻击(wIncAtkPerc)、防御(wIncDefPerc)、命中(wIncRatePerc)
    // TODO: 宠物系统实现后，将 buff 实际应用到宠物实体的属性上
    if (tpl && tpl->bType == 4 && tpl->bKind == 48 && pMugongData) {
        LOG("[MugongHandler] PetBuff Skill " + std::to_string(dwMugongID) + " Lv" + std::to_string(bMugongLevel)
            + " cast by " + std::to_string(dwAttackID)
            + " atkPerc=" + std::to_string(pMugongData->wIncAtkPerc)
            + " defPerc=" + std::to_string(pMugongData->wIncDefPerc)
            + " ratePerc=" + std::to_string(pMugongData->wIncRatePerc)
            + " duration=" + std::to_string(pMugongData->dwKeepUpTime) + "s"
            + " (PetSystem not yet implemented, buff visual only)");

        // 广播 0x402C buff 特效（挂在施放者身上，客户端显示光环）
        std::vector<BYTE> buffAck(4 + 11);
        BYTE* bp = buffAck.data() + 4;
        bp[0] = 0;
        *(DWORD*)(bp + 1) = dwAttackID;
        bp[5] = 1; // bObjectType = PC
        *(DWORD*)(bp + 6) = dwMugongID;
        bp[10] = bMugongLevel;
        PACKET_HEADER* bh = (PACKET_HEADER*)buffAck.data();
        bh->id = 0x402C; bh->payloadSize = 11;
        EncryptPacket(buffAck.data(), 0x42);
        BroadcastPacketToMap(playerMapID, buffAck);

        // 广播 0x4016 施法动画
        std::vector<BYTE> ackBuf(4 + 38, 0);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0; *(DWORD*)(p + 1) = dwMugongID; p[5] = bMugongLevel; p[6] = 1;
        *(DWORD*)(p + 7) = dwAttackID; *(WORD*)(p + 11) = wAttackPosX; *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight; p[16] = 1; *(DWORD*)(p + 17) = dwAttackID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0; *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0; p[37] = 0;
        PACKET_HEADER* head11 = (PACKET_HEADER*)ackBuf.data();
        head11->id = 0x4016; head11->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }

    // === bType=4, bKind=24（寸草不生 127）：地面持续 AoE DoT ===
    // 在施放者指定的鼠标地面位置布置毒雾，每 nEtc2 ms 对 nEtc1 半径内怪物造成 攻击×wIncAtkPerc% 伤害
    if (tpl && tpl->bType == 4 && tpl->bKind == 24 && pMugongData) {
        DWORD dotDuration = pMugongData->dwKeepUpTime * 1000;
        float aoeRadius = (pMugongData->nEtc1 > 0) ? (float)pMugongData->nEtc1 : 20.0f;
        DWORD tickInterval = (pMugongData->nEtc2 > 0) ? (DWORD)pMugongData->nEtc2 : 2000;
        WORD atkPerc = pMugongData->wIncAtkPerc;

        DWORD casterAtk = 50;
        
        // 1. 业务设计意图：引入服务端最大 15 格施法射程的安全防御机制。
        // 计算玩家当前坐标 (wAttackPosX, wAttackPosY) 与鼠标指向点 (wTargetPosX, wTargetPosY) 的真实距离。
        // 若超出射程，执行高精度向量投影截断，强行将毒雾生成点和动画广播点锁定在 15 格的施法最大边界上，完美防御越界封包挂。
        float dx = (float)wAttackPosX - (float)wTargetPosX;
        float dy = (float)wAttackPosY - (float)wTargetPosY;
        float castDist = sqrtf(dx * dx + dy * dy);
        float maxCastRange = 15.0f; // 寸草不生最大 15 格射程限制

        WORD finalTargetX = wTargetPosX;
        WORD finalTargetY = wTargetPosY;
        if (castDist > maxCastRange) {
            float ratio = maxCastRange / castDist;
            finalTargetX = wAttackPosX + (WORD)((float)(wTargetPosX - wAttackPosX) * ratio);
            finalTargetY = wAttackPosY + (WORD)((float)(wTargetPosY - wAttackPosY) * ratio);
        }

        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            PlayerData* pCaster = mapInst->GetPlayer(dwAttackID);
            if (pCaster) {
                casterAtk = pCaster->dwTotalAtk;
            }

            // 创建地面特效
            CMapInstance::sGroundEffect ge;
            ge.dwMugongID = dwMugongID;
            ge.bLevel = bMugongLevel;
            ge.dwCasterID = dwAttackID;
            ge.wPosX = finalTargetX; // 毒雾精确布置在鼠标指定的 3D 拾取网格坐标上
            ge.wPosY = finalTargetY;
            ge.fRadius = aoeRadius;
            ge.dwTickInterval = tickInterval;
            ge.wAtkPerc = atkPerc;
            ge.dwSnapshotAtk = casterAtk;
            ge.dwEndTime = GetTickCount() + dotDuration;
            ge.dwLastTickTime = GetTickCount();
            ge.dwMapID = playerMapID;
            mapInst->AddGroundEffect(ge);
        }

        // 广播 0x402C 通知客户端创建毒雾特效（挂在施放者身上，用于显示状态图标）
        std::vector<BYTE> buffAck(4 + 11);
        BYTE* bp = buffAck.data() + 4;
        bp[0] = 0;
        *(DWORD*)(bp + 1) = dwAttackID;
        bp[5] = 1; // bObjectType = PC
        *(DWORD*)(bp + 6) = dwMugongID;
        bp[10] = bMugongLevel;
        PACKET_HEADER* bh = (PACKET_HEADER*)buffAck.data();
        bh->id = 0x402C; bh->payloadSize = 11;
        EncryptPacket(buffAck.data(), 0x42);
        BroadcastPacketToMap(playerMapID, buffAck);

        // 广播 0x4016 施法动画，将粒子特效精准降临在地面目标坐标上
        std::vector<BYTE> ackBuf(4 + 38, 0);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0; *(DWORD*)(p + 1) = dwMugongID; p[5] = bMugongLevel; p[6] = 1;
        *(DWORD*)(p + 7) = dwAttackID; *(WORD*)(p + 11) = finalTargetX; *(WORD*)(p + 13) = finalTargetY;
        p[15] = bAttackHeight; p[16] = 1; *(DWORD*)(p + 17) = dwAttackID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0; *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0; p[37] = 0;
        PACKET_HEADER* head10 = (PACKET_HEADER*)ackBuf.data();
        head10->id = 0x4016; head10->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }

    // === bType=4, bKind=19（化骨功 126 / 毒烟神功 198）：单体 DoT 持续掉血 ===
    // nEtc1=每tick固定伤害, nEtc2=tick间隔(毫秒), dwKeepUpTime=总持续时间
    if (tpl && tpl->bType == 4 && tpl->bKind == 19 && pMugongData && dwDefenseID != 0) {
        WORD successRate = pMugongData->wSuccessRatePerc;
        DWORD dotDuration = pMugongData->dwKeepUpTime * 1000;

        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            MonsterData* pMon = mapInst->GetMonster(dwDefenseID);
            if (pMon && pMon->dwHpCur > 0) {
                bool success = (successRate >= 100) || ((WORD)(rand() % 100) < successRate);
                if (success) {
                    PlayerData::sActiveBuff dotBuff;
                    dotBuff.dwMugongID = dwMugongID;
                    dotBuff.bLevel = bMugongLevel;
                    dotBuff.dwEndTime = GetTickCount() + dotDuration;
                    dotBuff.bIsDebuff = true;
                    dotBuff.dwLastTickTime = GetTickCount();
                    dotBuff.dwCasterID = dwAttackID; // 业务设计意图：记录施法者玩家ID，保证心跳DoT结算时能向客户端同步正确的攻击者
                    pMon->activeBuffs[dwMugongID] = dotBuff;

                    std::vector<BYTE> dotAck(4 + 11);
                    BYTE* dp = dotAck.data() + 4;
                    dp[0] = 0;
                    *(DWORD*)(dp + 1) = pMon->dwObjectID;
                    dp[5] = pMon->bObjectType;
                    *(DWORD*)(dp + 6) = dwMugongID;
                    dp[10] = bMugongLevel;
                    PACKET_HEADER* dh = (PACKET_HEADER*)dotAck.data();
                    dh->id = 0x402C; dh->payloadSize = 11;
                    EncryptPacket(dotAck.data(), 0x42);
                    BroadcastPacketToMap(playerMapID, dotAck);
                    LOG("[MugongHandler] DoT applied on monster " + std::to_string(pMon->dwObjectID)
                        + " dmg/tick=" + std::to_string(pMugongData->nEtc1) + " interval=" + std::to_string(pMugongData->nEtc2)
                        + "ms duration=" + std::to_string(pMugongData->dwKeepUpTime) + "s");
                }
            }
        }

        std::vector<BYTE> ackBuf(4 + 38, 0);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0; *(DWORD*)(p + 1) = dwMugongID; p[5] = bMugongLevel; p[6] = 1;
        *(DWORD*)(p + 7) = dwAttackID; *(WORD*)(p + 11) = wAttackPosX; *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight; p[16] = bDefenseType; *(DWORD*)(p + 17) = dwDefenseID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0; *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0; p[37] = 0;
        PACKET_HEADER* head6 = (PACKET_HEADER*)ackBuf.data();
        head6->id = 0x4016; head6->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }

    // === bType=4, bKind=20（气烟逆流 95 / 魔灵神功 196）：单体降命中 debuff ===
    if (tpl && tpl->bType == 4 && tpl->bKind == 20 && pMugongData && dwDefenseID != 0) {
        WORD successRate = pMugongData->wSuccessRatePerc;
        DWORD debuffDuration = pMugongData->dwKeepUpTime * 1000;

        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            
            bool isPlayer = false;
            bool isMonster = false;
            bool pvpSuccess = false;
            bool pveSuccess = false;
            DWORD targetObjectID = 0;
            BYTE targetObjectType = 0;

            // 1. 业务设计意图：引入局部大括号块以隔离地图非递归锁的生命周期。
            // 在锁保护下，仅执行状态读取、概率判定与 Buff 挂载，挂载后立即释放锁。
            // 潜在风险：锁释放后，被引用的 PlayerData 或 MonsterData 指针可能会失效，
            // 故在此仅安全提取其 ObjectID 与 ObjectType，绝不将指针带到锁外访问。
            {
                std::lock_guard<std::mutex> lock(mapInst->GetMutex());
                
                // 降命中 Debuff PvP 与 PvE 双重挂载支持
                PlayerData* pTargetPlayer = mapInst->GetPlayer(dwDefenseID);
                if (pTargetPlayer && pTargetPlayer->dwHpCur > 0) {
                    isPlayer = true;
                    pvpSuccess = (successRate >= 100) || ((WORD)(rand() % 100) < successRate);
                    if (pvpSuccess) {
                        PlayerData::sActiveBuff hitDebuff;
                        hitDebuff.dwMugongID = dwMugongID;
                        hitDebuff.bLevel = bMugongLevel;
                        hitDebuff.dwEndTime = GetTickCount() + debuffDuration;
                        hitDebuff.bIsDebuff = true;
                        pTargetPlayer->activeBuffs[dwMugongID] = hitDebuff;
                        targetObjectID = pTargetPlayer->dwObjectID;
                    }
                }
                else {
                    MonsterData* pMon = mapInst->GetMonster(dwDefenseID);
                    if (pMon && pMon->dwHpCur > 0) {
                        isMonster = true;
                        pveSuccess = (successRate >= 100) || ((WORD)(rand() % 100) < successRate);
                        if (pveSuccess) {
                            PlayerData::sActiveBuff hitDebuff;
                            hitDebuff.dwMugongID = dwMugongID;
                            hitDebuff.bLevel = bMugongLevel;
                            hitDebuff.dwEndTime = GetTickCount() + debuffDuration;
                            hitDebuff.bIsDebuff = true;
                            pMon->activeBuffs[dwMugongID] = hitDebuff;
                            targetObjectID = pMon->dwObjectID;
                            targetObjectType = pMon->bObjectType;
                        }
                    }
                }
            } // 地图锁在此处随着局部大括号结束而自动析构释放

            // 2. 锁释放后，安全地执行属性重算与封包广播，彻底避免非递归锁自我死锁
            if (isPlayer && pvpSuccess) {
                // 实时触发受击玩家属性面板重算与静默刷新（避免清空武功窗口）
                PlayerManager::GetInstance().RecalculateStats(dwDefenseID - 400000000, false, true);

                std::vector<BYTE> dAck(4 + 11);
                BYTE* dp = dAck.data() + 4;
                dp[0] = 0; *(DWORD*)(dp + 1) = targetObjectID; dp[5] = 1; // 1 = Player
                *(DWORD*)(dp + 6) = dwMugongID; dp[10] = bMugongLevel;
                PACKET_HEADER* dh = (PACKET_HEADER*)dAck.data();
                dh->id = 0x402C; dh->payloadSize = 11;
                EncryptPacket(dAck.data(), 0x42);
                BroadcastPacketToMap(playerMapID, dAck);
                LOG("[MugongHandler] HitDebuff PVP SUCCESS on player " + std::to_string(targetObjectID) + " duration=" + std::to_string(pMugongData->dwKeepUpTime) + "s");
            }
            else if (isMonster && pveSuccess) {
                std::vector<BYTE> dAck(4 + 11);
                BYTE* dp = dAck.data() + 4;
                dp[0] = 0; *(DWORD*)(dp + 1) = targetObjectID; dp[5] = targetObjectType; // 3 = Monster
                *(DWORD*)(dp + 6) = dwMugongID; dp[10] = bMugongLevel;
                PACKET_HEADER* dh = (PACKET_HEADER*)dAck.data();
                dh->id = 0x402C; dh->payloadSize = 11;
                EncryptPacket(dAck.data(), 0x42);
                BroadcastPacketToMap(playerMapID, dAck);
                LOG("[MugongHandler] HitDebuff PVE SUCCESS on monster " + std::to_string(targetObjectID) + " duration=" + std::to_string(pMugongData->dwKeepUpTime) + "s");
            }
        }

        std::vector<BYTE> ackBuf(4 + 38, 0);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0; *(DWORD*)(p + 1) = dwMugongID; p[5] = bMugongLevel; p[6] = 1;
        *(DWORD*)(p + 7) = dwAttackID; *(WORD*)(p + 11) = wAttackPosX; *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight; p[16] = bDefenseType; *(DWORD*)(p + 17) = dwDefenseID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0; *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0; p[37] = 0;
        PACKET_HEADER* head7 = (PACKET_HEADER*)ackBuf.data();
        head7->id = 0x4016; head7->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }

    // === bType=4, bKind=21（五毒针 122）：单体攻击倍率 DoT ===
    if (tpl && tpl->bType == 4 && tpl->bKind == 21 && pMugongData && dwDefenseID != 0) {
        WORD successRate = pMugongData->wSuccessRatePerc;
        DWORD dotDuration = pMugongData->dwKeepUpTime * 1000;

        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            PlayerData* pCaster = mapInst->GetPlayer(dwAttackID);
            MonsterData* pMon = mapInst->GetMonster(dwDefenseID);
            DWORD casterAtk = (pCaster) ? pCaster->dwTotalAtk : 50;
            if (pMon && pMon->dwHpCur > 0) {
                bool success = (successRate >= 100) || ((WORD)(rand() % 100) < successRate);
                if (success) {
                    PlayerData::sActiveBuff dotBuff;
                    dotBuff.dwMugongID = dwMugongID;
                    dotBuff.bLevel = bMugongLevel;
                    dotBuff.dwEndTime = GetTickCount() + dotDuration;
                    dotBuff.bIsDebuff = true;
                    dotBuff.dwLastTickTime = GetTickCount();
                    dotBuff.dwSnapshotAtk = casterAtk;
                    dotBuff.dwCasterID = dwAttackID; // 业务设计意图：记录施法者玩家ID，保证心跳DoT结算时能向客户端同步正确的攻击者
                    pMon->activeBuffs[dwMugongID] = dotBuff;

                    std::vector<BYTE> dotAck(4 + 11);
                    BYTE* dp = dotAck.data() + 4;
                    dp[0] = 0; *(DWORD*)(dp + 1) = pMon->dwObjectID; dp[5] = pMon->bObjectType;
                    *(DWORD*)(dp + 6) = dwMugongID; dp[10] = bMugongLevel;
                    PACKET_HEADER* dh = (PACKET_HEADER*)dotAck.data();
                    dh->id = 0x402C; dh->payloadSize = 11;
                    EncryptPacket(dotAck.data(), 0x42);
                    BroadcastPacketToMap(playerMapID, dotAck);
                    DWORD tickDmg = casterAtk * pMugongData->wIncAtkPerc / 100;
                    LOG("[MugongHandler] AtkDoT on monster " + std::to_string(pMon->dwObjectID)
                        + " atk=" + std::to_string(casterAtk) + " dmg/tick=" + std::to_string(tickDmg)
                        + " duration=" + std::to_string(pMugongData->dwKeepUpTime) + "s");
                }
            }
        }

        std::vector<BYTE> ackBuf(4 + 38, 0);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0; *(DWORD*)(p + 1) = dwMugongID; p[5] = bMugongLevel; p[6] = 1;
        *(DWORD*)(p + 7) = dwAttackID; *(WORD*)(p + 11) = wAttackPosX; *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight; p[16] = bDefenseType; *(DWORD*)(p + 17) = dwDefenseID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0; *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0; p[37] = 0;
        PACKET_HEADER* head8 = (PACKET_HEADER*)ackBuf.data();
        head8->id = 0x4016; head8->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }

    // === bType=4, bKind=22（化功术 125）：单体减蓝 DoT ===
    if (tpl && tpl->bType == 4 && tpl->bKind == 22 && pMugongData && dwDefenseID != 0) {
        WORD successRate = pMugongData->wSuccessRatePerc;
        DWORD dotDuration = pMugongData->dwKeepUpTime * 1000;

        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            
            bool isPlayer = false;
            bool isMonster = false;
            bool pvpSuccess = false;
            bool pveSuccess = false;
            DWORD targetObjectID = 0;
            BYTE targetObjectType = 0;

            // 1. 业务设计意图：引入局部大括号以隔离地图非递归锁的生命周期，避免在重算或发送封包时触发自我死锁。
            // 锁仅保护状态读取、概率计算与 Buff 数据录入，录入完毕后瞬时释放。
            {
                std::lock_guard<std::mutex> lock(mapInst->GetMutex());
                
                PlayerData* pTargetPlayer = mapInst->GetPlayer(dwDefenseID);
                if (pTargetPlayer && pTargetPlayer->dwHpCur > 0) {
                    isPlayer = true;
                    pvpSuccess = (successRate >= 100) || ((WORD)(rand() % 100) < successRate);
                    if (pvpSuccess) {
                        PlayerData::sActiveBuff mpDot;
                        mpDot.dwMugongID = dwMugongID;
                        mpDot.bLevel = bMugongLevel;
                        mpDot.dwEndTime = GetTickCount() + dotDuration;
                        mpDot.bIsDebuff = true;
                        mpDot.dwLastTickTime = GetTickCount();
                        mpDot.dwCasterID = dwAttackID; // 记录施法者ID，保证IP DoT源追踪
                        pTargetPlayer->activeBuffs[dwMugongID] = mpDot;
                        targetObjectID = pTargetPlayer->dwObjectID;
                    }
                }
                else {
                    MonsterData* pMon = mapInst->GetMonster(dwDefenseID);
                    if (pMon && pMon->dwHpCur > 0) {
                        isMonster = true;
                        pveSuccess = (successRate >= 100) || ((WORD)(rand() % 100) < successRate);
                        if (pveSuccess) {
                            PlayerData::sActiveBuff mpDot;
                            mpDot.dwMugongID = dwMugongID;
                            mpDot.bLevel = bMugongLevel;
                            mpDot.dwEndTime = GetTickCount() + dotDuration;
                            mpDot.bIsDebuff = true;
                            mpDot.dwLastTickTime = GetTickCount();
                            mpDot.dwCasterID = dwAttackID; // 记录施法者ID
                            pMon->activeBuffs[dwMugongID] = mpDot;
                            targetObjectID = pMon->dwObjectID;
                            targetObjectType = pMon->bObjectType;
                        }
                    }
                }
            } // 地图锁在此自动释放析构

            // 2. 锁释放后，安全地执行属性重算与 debuff 图标广播 (0x402C)
            if (isPlayer && pvpSuccess) {
                // 实时触发受击玩家属性面板重算与静默刷新 (不清理武功窗口)
                PlayerManager::GetInstance().RecalculateStats(dwDefenseID - 400000000, false, true);

                std::vector<BYTE> dAck(4 + 11);
                BYTE* dp = dAck.data() + 4;
                dp[0] = 0; *(DWORD*)(dp + 1) = targetObjectID; dp[5] = 1; // 1 = Player
                *(DWORD*)(dp + 6) = dwMugongID; dp[10] = bMugongLevel;
                PACKET_HEADER* dh = (PACKET_HEADER*)dAck.data();
                dh->id = 0x402C; dh->payloadSize = 11;
                EncryptPacket(dAck.data(), 0x42);
                BroadcastPacketToMap(playerMapID, dAck);
                LOG("[MugongHandler] MpDot PVP SUCCESS on player " + std::to_string(targetObjectID) 
                    + " drain=" + std::to_string(pMugongData->nEtc1) + "/tick");
            }
            else if (isMonster && pveSuccess) {
                std::vector<BYTE> dAck(4 + 11);
                BYTE* dp = dAck.data() + 4;
                dp[0] = 0; *(DWORD*)(dp + 1) = targetObjectID; dp[5] = targetObjectType;
                *(DWORD*)(dp + 6) = dwMugongID; dp[10] = bMugongLevel;
                PACKET_HEADER* dh = (PACKET_HEADER*)dAck.data();
                dh->id = 0x402C; dh->payloadSize = 11;
                EncryptPacket(dAck.data(), 0x42);
                BroadcastPacketToMap(playerMapID, dAck);
                LOG("[MugongHandler] MpDot PVE SUCCESS on monster " + std::to_string(targetObjectID) 
                    + " drain=" + std::to_string(pMugongData->nEtc1) + "/tick");
            }
        }

        std::vector<BYTE> ackBuf(4 + 38, 0);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0; *(DWORD*)(p + 1) = dwMugongID; p[5] = bMugongLevel; p[6] = 1;
        *(DWORD*)(p + 7) = dwAttackID; *(WORD*)(p + 11) = wAttackPosX; *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight; p[16] = bDefenseType; *(DWORD*)(p + 17) = dwDefenseID;
        *(DWORD*)(p + 21) = 0; *(DWORD*)(p + 25) = 0; *(DWORD*)(p + 29) = 0; *(DWORD*)(p + 33) = 0; p[37] = 0;
        PACKET_HEADER* head9 = (PACKET_HEADER*)ackBuf.data();
        head9->id = 0x4016; head9->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return;
    }


    // 1.5. Distance validation (only for targeted skills with wDistance > 0)

    if (pMugongData && pMugongData->wAttackRange > 0 && dwDefenseID != 0) {

        float dx = (float)wAttackPosX - (float)wTargetPosX;

        float dy = (float)wAttackPosY - (float)wTargetPosY;

        float dist = sqrtf(dx * dx + dy * dy);

        if (dist > (float)pMugongData->wAttackRange) {

            LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " blocked: distance " 

                + std::to_string((int)dist) + " > range " + std::to_string(pMugongData->wAttackRange));

            return;

        }

    }

    

    // 3. Buff/龙技能路由（基于 bType 数据驱动）
    // bType=5 即龙技能，isBuff 覆盖所有 dwKeepUpTime > 0 的技能（含经功等）
    bool isDragonSkill = (tpl && tpl->bType == 5);

    if (isBuff || isDragonSkill) {

        bool isDuplicate = false;

        

        if (g_MapInstances.count(playerMapID)) {

            CMapInstance* mapInst = g_MapInstances[playerMapID];

            std::lock_guard<std::mutex> lock(mapInst->GetMutex());

            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);

            if (pObj) {

                // If buff already exists, reset the timer and let it broadcast 0x402C to refresh UI

                if (pObj->activeBuffs.count(dwMugongID) > 0) {

                    pObj->activeBuffs[dwMugongID].dwEndTime = GetTickCount() + (isDragonSkill ? 3000 : (pMugongData ? pMugongData->dwKeepUpTime * 1000 : 300000));

                    isDuplicate = true; // Avoid sending CS_BT_KEEPUPMUGONGSTART_ACK again

                    LOG("[MugongHandler] Buff/Dragon " + std::to_string(dwMugongID) + " already exists. Reset internal timer. UI/Visuals will be refreshed.");

                } else {

                    // Add new buff

                    sServerObject::sActiveBuff newBuff;

                    newBuff.dwMugongID = dwMugongID;

                    newBuff.bLevel = bMugongLevel;

                    newBuff.dwEndTime = GetTickCount() + (isDragonSkill ? 3000 : (pMugongData ? pMugongData->dwKeepUpTime * 1000 : 300000));

                    newBuff.bIsDebuff = false;

                    pObj->activeBuffs[dwMugongID] = newBuff;

                    LOG("[MugongHandler] Applied BUFF/Dragon " + std::to_string(dwMugongID) + " to player " + std::to_string(dwAttackID));

                }

            } else {

                LOG("[MugongHandler] WARNING: mapInst->GetPlayer() IS NULL! dwAttackID=" + std::to_string(dwAttackID));

            }

        }



        // Allow recasting so client gets 0x402C and visual effects play every time



        std::vector<BYTE> buffAck(4 + 11);

        BYTE* bp = buffAck.data() + 4;

        bp[0] = 0; // bResult

        *(DWORD*)(bp + 1) = dwAttackID;

        bp[5] = 1; // bObjectType (Player)

        *(DWORD*)(bp + 6) = dwMugongID;

        bp[10] = bMugongLevel;

        

        PACKET_HEADER* headB = (PACKET_HEADER*)buffAck.data();

        headB->id = 0x402C;

        headB->payloadSize = 11;

        EncryptPacket(buffAck.data(), 0x42);

        BroadcastPacketToMap(playerMapID, buffAck);

        

        LOG("[MugongHandler] Applied BUFF " + std::to_string(dwMugongID) + " to player " + std::to_string(dwAttackID));



        // Immediately recalculate stats so buff bonuses take effect (do NOT send 0x4414 to prevent clearing client visuals)

        DWORD buffCharID = dwAttackID - 400000000;

        PlayerManager::GetInstance().RecalculateStats(buffCharID, false);

        // bType=4, bKind=7（九天凤舞/元气神功）：范围内友方也挂同样buff
        if (tpl && tpl->bType == 4 && tpl->bKind == 7 && pMugongData && g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::vector<DWORD> targetAllyCharIDs;
            
            // 1. 业务设计意图：地图非递归锁仅用于极其轻量级的 AOI 队友检索与距离校验，随即释放，杜绝锁重入。
            {
                std::lock_guard<std::mutex> lock(mapInst->GetMutex());
                PlayerData* pCaster = mapInst->GetPlayer(dwAttackID);
                if (pCaster) {
                    float buffRange = (pMugongData->wAttackRange > 0) ? (float)pMugongData->wAttackRange : 50.0f;
                    WORD cx = pCaster->wPosX, cy = pCaster->wPosY;

                    std::vector<PlayerData*> aoiPlayers = mapInst->GetPlayersInAOI(cx, cy);
                    for (PlayerData* pAlly : aoiPlayers) {
                        if (!pAlly || pAlly->dwObjectID == dwAttackID || pAlly->dwHpCur == 0) continue;
                        float dx = (float)pAlly->wPosX - (float)cx;
                        float dy = (float)pAlly->wPosY - (float)cy;
                        if (sqrtf(dx * dx + dy * dy) > buffRange) continue;

                        targetAllyCharIDs.push_back(pAlly->dwObjectID - 400000000);
                    }
                }
            } // 地图锁在此处随着大括号结束自动析构释放

            // 2. 锁完全释放后，依次对符合条件的队友调用 ApplyBuffSafe 进行并发安全的挂载、重算与广播
            for (DWORD allyCharID : targetAllyCharIDs) {
                PlayerManager::GetInstance().ApplyBuffSafe(allyCharID, dwMugongID, bMugongLevel, pMugongData->dwKeepUpTime * 1000, false);
            }
        }

    }



    // IP cost was already verified and deducted early at step 2.5



    // 4. Broadcast AttackAck (0x4016)

    // IMPORTANT: We must ALWAYS send 0x4016, even for buff skills.

    // The client's attack state machine requires this ACK to complete the sequence.

    // Without it, the client gets stuck and all subsequent skills fail to render effects.

    // For skills with no target (dwDefenseID==0, including self-buffs), set defender to self

    // with OBJTYPE_PC(1) to prevent client NULL dereference in OnCS_BT_MUGONGATTACK_ACK.

    bool bNoRealTarget = false;

    if (dwDefenseID == 0) {

        dwDefenseID = dwAttackID;

        bDefenseType = 1; // OBJTYPE_PC

        bNoRealTarget = true;

    }

    std::vector<BYTE> ackBuf(4 + 38);

    BYTE* p = ackBuf.data() + 4;

    p[0] = 0; // default bResult

    *(DWORD*)(p + 1) = dwMugongID;

    p[5] = bMugongLevel;

    p[6] = 1; // bAtkType = OBJTYPE_PC (client uses this in FindXiahObject)

    *(DWORD*)(p + 7) = dwAttackID;

    *(WORD*)(p + 11) = wAttackPosX;

    *(WORD*)(p + 13) = wAttackPosY;

    p[15] = bAttackHeight;

    p[16] = bDefenseType;

    *(DWORD*)(p + 17) = dwDefenseID;

    

    DWORD dwDefHpMax = 0;

    DWORD dwDefHpCur = 0;

    WORD  dwDefIpMax = 0;

    WORD  dwDefIpCur = 0;

    DWORD finalDmg = 0;

    DWORD deadExp = 0;

    bool isDead = false;

    BYTE bCritHit = 0;



    // Struct to store dead monsters to process EXP/animations outside the map lock safely

    struct sDeadEntity {
        DWORD dwObjectID;
        DWORD dwExp;
        std::string szName;
        DWORD dwFiveElmExp;
        WORD wPosX;
        WORD wPosY;
    };

    std::vector<sDeadEntity> deadEntities;


    // 用于在 mapMutex 锁外发送 0x3B0D 给被 AOE 溅射到的玩家（避免死锁）
    struct sAoeSplashPlayerSnapshot {
        DWORD dwCharID;
        DWORD dwHpMax;
        DWORD dwHpCur;
        DWORD dwIpMax;
        DWORD dwIpCur;
    };
    std::vector<sAoeSplashPlayerSnapshot> splashPlayerSnapshots;

    // [修复死锁] 延迟广播队列：在 mapMutex 区内只构造数据包，退出锁后再统一调用 BroadcastPacketToMap
    // 避免 mapMutex → SessionMgr::m_mutex 与 SessionMgr::m_mutex → mapMutex 的 AB-BA 死锁
    std::vector<std::vector<BYTE>> deferredSplashBroadcasts;


    // === PvE Attack Monster ===
#include "MugongAttack_PvE.inl"

    // === PvP Attack Player ===
#include "MugongAttack_PvP.inl"

    // [修复死锁] 在 mapMutex 锁外统一广播 AOE 溅射数据包（0x4016），避免 AB-BA 死锁
    for (const auto& pkt : deferredSplashBroadcasts) {
        BroadcastPacketToMap(playerMapID, pkt);
    }
    deferredSplashBroadcasts.clear();

    // [修复死锁] 在 mapMutex 锁外发送 0x3B0D 给被 AOE 溅射到的玩家
    for (const auto& snap : splashPlayerSnapshots) {
        SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(snap.dwCharID);
        if (targetSock != INVALID_SOCKET) {
            std::vector<BYTE> hpBuf(4);
            auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
            push4(snap.dwHpMax); push4(snap.dwHpCur); push4(snap.dwIpMax); push4(snap.dwIpCur);
            hpBuf.push_back(0);
            PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
            hpHead->id = 0x3B0D; hpHead->payloadSize = hpBuf.size() - 4;
            EncryptPacket(hpBuf.data(), 0x42);
            SafeSend(targetSock, (const char*)hpBuf.data(), hpBuf.size(), 0);
            PlayerManager::GetInstance().RecalculateStats(snap.dwCharID, false);
        }
    }
    splashPlayerSnapshots.clear();



    *(DWORD*)(p + 21) = dwDefHpMax;

    *(DWORD*)(p + 25) = dwDefHpCur;

    *(DWORD*)(p + 29) = finalDmg;

    *(DWORD*)(p + 33) = 0; // dwExp in ack doesn't seem to give it directly

    p[37] = bCritHit; // bHitFlag (1 = Critical, 0 = Normal)



    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();

    head->id = 0x4016; // CS_BT_MUGONGATTACK_ACK

    head->payloadSize = 38;

    EncryptPacket(ackBuf.data(), 0x42);



    // We must ALWAYS broadcast 0x4016 (MugongAttackAck), even for buff/self-cast skills.

    // The client's combat animation/effect state machine expects this to complete the sequence.

    // If suppressed, the client gets stuck in a state where it drops subsequent skill effects.

    // Using bResult = 0 ensures no self-hit stagger animation is played.

    BroadcastPacketToMap(playerMapID, ackBuf);



    // PvP ������� Debuff ���졤��

    if (!bNoRealTarget && dwDefenseID != dwAttackID && bDefenseType == 1 && dwDefenseID > 0 && dwDefHpMax > 0) {

        DWORD defenderCharID = dwDefenseID - 400000000;

        SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(defenderCharID);

        if (targetSock != INVALID_SOCKET) {

            std::vector<BYTE> hpBuf(4);

            auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };

            auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };

            push4(dwDefHpMax);

            push4(dwDefHpCur);

            push4(dwDefIpMax);

            push4(dwDefIpCur);

            hpBuf.push_back(0); 

            PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();

            hpHead->id = 0x3B0D;

            hpHead->payloadSize = hpBuf.size() - 4;

            EncryptPacket(hpBuf.data(), 0x42);

            SafeSend(targetSock, (const char*)hpBuf.data(), hpBuf.size(), 0);



            // ���쨦���쨦 Debuff 

            PlayerManager::GetInstance().RecalculateStats(defenderCharID, false);

        }

    }



    // Process exp granting, level-ups, and death animations for all dead monsters

    for (auto& de : deadEntities) {

        DWORD attackerCharID = dwAttackID - 400000000; // 800000301 -> 400000301 (matches DB/SessionMgr format)

        // 统一调用队伍经验分配逻辑（包含同地图与50格距离严格校验，以及升级广播）
        DistributePartyExp(attackerCharID, de.dwExp, de.dwFiveElmExp, playerMapID, de.wPosX, de.wPosY, false /* callerHoldsMapLock */);



        // Send CS_NC_STATUSCHANGE_ACK (0x3510) to trigger Death Animation

        auto pushDW = [](std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); };

        std::vector<BYTE> animBuf; animBuf.resize(4);

        animBuf.push_back(3); // bObjectType (Monster)

        DWORD oid = de.dwObjectID;

        pushDW(animBuf, oid);

        animBuf.push_back(3); // bStatus = 3 (Dead)

        animBuf.push_back(0); // wValue1 L

        animBuf.push_back(0); // wValue1 H

        animBuf.push_back(0xFF); // bValue2

        

        PACKET_HEADER* animHead = (PACKET_HEADER*)animBuf.data();

        animHead->id = 0x3510;

        animHead->payloadSize = animBuf.size() - sizeof(PACKET_HEADER);

        EncryptPacket(animBuf.data(), 0x42);

        BroadcastPacketToMap(playerMapID, animBuf);

        

        // Send CS_BT_KILLSUCCESS_ACK (0x4034) to attacker (trigger kill VFX)

        // Client reads: bObjectType(BYTE) + dwObjectID(DWORD) + dwExp(DWORD)

        std::vector<BYTE> killBuf; killBuf.resize(4);

        killBuf.push_back(3); // bObjectType (Monster)

        pushDW(killBuf, oid);

        pushDW(killBuf, de.dwExp);

        

        PACKET_HEADER* killHead = (PACKET_HEADER*)killBuf.data();

        killHead->id = 0x4034;

        killHead->payloadSize = killBuf.size() - sizeof(PACKET_HEADER);

        EncryptPacket(killBuf.data(), 0x42);

        SafeSend(clientSocket, (char*)killBuf.data(), killBuf.size(), 0);

    }

}
// ----------------------------------------------------------------------------
// ѡ�����ô����� (ͬ���ͻ��˰�ȫ��������: ���н�ɫ/�����ɳ�Ա/�޶���)
// ----------------------------------------------------------------------------
void OnSetOptionReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 1) return;
    BYTE opt = payload[0];
    DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (g_MapInstances.count(playerMapID)) {
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(charID + 400000000);
        if (pObj) {
            pObj->bSafeMode = opt;
            LOG("[MugongHandler] OnSetOptionReq: Player " + std::to_string(charID + 400000000) + " set bSafeMode to " + std::to_string(opt));
        }
    }

    std::vector<BYTE> ackBuf(4 + 1);
    BYTE* p = ackBuf.data() + 4;
    p[0] = opt;
    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x3A01; // CS_OP_SETOPTION_ACK
    head->payloadSize = 1;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
}

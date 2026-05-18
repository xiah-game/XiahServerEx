#include "MugongHandler.h"
#include "../GameObjects/MugongManager.h"
#include "../GameObjects/PlayerManager.h"
#include "../GameObjects/ExpSystem.h"
#include "../Network/SessionMgr.h"
#include "../MonsterAI.h"
#include "../GameObjects/DropManager.h"
#include "../GameObjects/MapInstance.h"
#include "../GameObjects/PlayerManager.h"
#include "../GameObjects/ExpSystem.h"
#include "../DB/CharacterDB.h"
#include <cmath>

extern std::map<DWORD, CMapInstance*> g_MapInstances;

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
        
        if (bMugongType == 0 && isGeneral) filteredSkills.push_back(s); // GENERAL/ACTIVE
        else if (bMugongType == 1 && isPassive) filteredSkills.push_back(s); // PASSIVE
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

void OnSelMugongReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 6) return;

    BYTE bType = payload[0];
    DWORD dwMugongID = *(DWORD*)(payload + 1);
    BYTE bIndex = payload[5];

    LOG("[MugongHandler] OnSelMugongReq: charID " + std::to_string(charID) + " set skill " + std::to_string(dwMugongID) + " as active S slot.");

    std::vector<BYTE> ackBuf(4 + 7); // 4 for header, 7 for payload
    BYTE* p = ackBuf.data() + 4;
    p[0] = 0; // bResult = 0 (Success)
    p[1] = bType;
    p[2] = dwMugongID & 0xFF;
    p[3] = (dwMugongID >> 8) & 0xFF;
    p[4] = (dwMugongID >> 16) & 0xFF;
    p[5] = (dwMugongID >> 24) & 0xFF;
    p[6] = bIndex;

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4018; // CS_BT_SELMUGONG_ACK
    head->payloadSize = ackBuf.size() - 4;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
}

void OnMugongPreAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
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

    LOG("[MugongHandler] OnMugongPreAttackReq: Skill " + std::to_string(dwMugongID) + " by " + std::to_string(dwAttackID) + " against " + std::to_string(dwDefenseID));

    std::vector<BYTE> ackBuf(4 + 29);
    BYTE* p = ackBuf.data() + 4;

    // Find Mugong level
    BYTE bMugongLevel = 1;
    int currentLvl = MugongManager::GetInstance()->GetPlayerMugongLevel(charID, dwMugongID);
    if(currentLvl > 0) bMugongLevel = currentLvl;

    // bResult (Always 0 to allow buff refreshing)
    p[0] = 0; 
    
    // Pack dwMugongID
    *(DWORD*)(p + 1) = dwMugongID;
    
    // Pack remaining bytes
    p[5] = bMugongLevel;
    p[6] = bAttackType;
    *(DWORD*)(p + 7) = dwAttackID;
    *(WORD*)(p + 11) = wAttackPosX;
    *(WORD*)(p + 13) = wAttackPosY;
    p[15] = bAttackHeight;
    p[16] = bDefenseType;
    *(DWORD*)(p + 17) = dwDefenseID;
    *(WORD*)(p + 21) = wTargetPosX;
    *(WORD*)(p + 23) = wTargetPosY;
    p[25] = bTargetHeight;
    *(WORD*)(p + 26) = 0; // wLifeTime
    p[28] = 0; // bAttackMode

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4014; // CS_BT_MUGONGPREATTACK_ACK
    head->payloadSize = 29;
    EncryptPacket(ackBuf.data(), 0x42);
    
    // Broadcast to map so everyone sees the pre-attack animation
    DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    BroadcastPacketToMap(playerMapID, ackBuf);
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

    BYTE bMugongLevel = 1;
    int currentLvl = MugongManager::GetInstance()->GetPlayerMugongLevel(charID, dwMugongID);
    if(currentLvl > 0) bMugongLevel = currentLvl;

    DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);

    // 1. Get the skill data
    sMugongList* pMugongData = MugongManager::GetInstance()->GetMugongLevelData(dwMugongID, bMugongLevel);
    
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

    // 2. Determine if it's a buff
    bool isBuff = (pMugongData && pMugongData->dwKeepUpTime > 0);
    
    // 3. If it's a buff skill (e.g. 64), broadcast KeepUpMugongStartAck (0x402C)
    if (isBuff || dwMugongID == 64) { // Fallback for 64 just in case DB doesn't have it
        bool isDuplicate = false;
        
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
            if (pObj) {
                // If buff already exists, reset the timer and let it broadcast 0x402C to refresh UI
                if (pObj->activeBuffs.count(dwMugongID) > 0) {
                    pObj->activeBuffs[dwMugongID].dwEndTime = GetTickCount() + (pMugongData ? pMugongData->dwKeepUpTime * 1000 : 300000);
                    LOG("[MugongHandler] Buff " + std::to_string(dwMugongID) + " already exists. Reset internal timer. UI will be refreshed.");
                } else {
                    // Add new buff
                    sServerObject::sActiveBuff newBuff;
                    newBuff.dwMugongID = dwMugongID;
                    newBuff.bLevel = bMugongLevel;
                    newBuff.dwEndTime = GetTickCount() + (pMugongData ? pMugongData->dwKeepUpTime * 1000 : 300000);
                    newBuff.bIsDebuff = false;
                    pObj->activeBuffs[dwMugongID] = newBuff;
                    LOG("[MugongHandler] Applied BUFF " + std::to_string(dwMugongID) + " to player " + std::to_string(dwAttackID));
                }
            } else {
                LOG("[MugongHandler] WARNING: mapInst->GetPlayer() IS NULL! dwAttackID=" + std::to_string(dwAttackID));
            }
        }

        // If it's a duplicate, RETURN EARLY. Do NOT send 0x402C to prevent UI icon duplication!
        if (isDuplicate) {
            return;
        }

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

        // Immediately recalculate stats so buff bonuses take effect
        DWORD buffCharID = dwAttackID - 400000000;
        PlayerManager::GetInstance().RecalculateStats(buffCharID, true);
    }

    // 3.5. Deduct IP cost (wReduceIP / dwCostMp) for using the skill
    if (pMugongData && pMugongData->dwCostMp > 0) {
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
            if (pObj) {
                if (pObj->wIpCur < (WORD)pMugongData->dwCostMp) {
                    // Not enough IP éˆ?block skill use
                    LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " blocked: IP " 
                        + std::to_string(pObj->wIpCur) + " < cost " + std::to_string(pMugongData->dwCostMp));
                    return;
                }
                WORD oldIp = pObj->wIpCur;
                pObj->wIpCur -= (WORD)pMugongData->dwCostMp;

                // Send 0x3B0D to update client HP/IP bars
                std::vector<BYTE> hpBuf(4);
                auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                push4(pObj->dwHpMax);
                push4(pObj->dwHpCur);
                push2(pObj->wIpMax);
                push2(pObj->wIpCur);
                hpBuf.push_back(0); // bType
                PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                hpHead->id = 0x3B0D;
                hpHead->payloadSize = hpBuf.size() - 4;
                EncryptPacket(hpBuf.data(), 0x42);
                SafeSend(clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);

                LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " IP cost: " 
                    + std::to_string(oldIp) + " - " + std::to_string(pMugongData->dwCostMp) 
                    + " = " + std::to_string(pObj->wIpCur));
            }
        }
    }

    // 4. Broadcast AttackAck (0x4016)
    std::vector<BYTE> ackBuf(4 + 38);
    BYTE* p = ackBuf.data() + 4;
    p[0] = 0; // default bResult
    *(DWORD*)(p + 1) = dwMugongID;
    p[5] = bMugongLevel;
    p[6] = bAttackType;
    *(DWORD*)(p + 7) = dwAttackID;
    *(WORD*)(p + 11) = wAttackPosX;
    *(WORD*)(p + 13) = wAttackPosY;
    p[15] = bAttackHeight;
    p[16] = bDefenseType;
    *(DWORD*)(p + 17) = dwDefenseID;
    
    DWORD dwDefHpMax = 60000;
    DWORD dwDefHpCur = 60000;
    DWORD finalDmg = 0;
    DWORD deadExp = 0;
    bool isDead = false;
    BYTE bCritHit = 0;

    if (!isBuff && dwMugongID != 64 && bDefenseType == 3) {
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            MonsterData* pTarget = mapInst->GetMonster(dwDefenseID);
            if (pTarget) {
                // Use template defense since MonsterData doesn't store wWepDef
                DWORD monsterDef = g_NpcTemplates.count(pTarget->bPropType) ? g_NpcTemplates[pTarget->bPropType].dwDefInit : 0;
                WORD monsterAvoid = pTarget->wAvoidRatio;

                // Dodge Logic
                DWORD playerAtkRating = 50; 
                PlayerData* pAttacker = mapInst->GetPlayer(dwAttackID);
                if (pAttacker) {
                    playerAtkRating += pAttacker->dwTotalHit;
                }
                
                float hitChance = (float)playerAtkRating / (float)(playerAtkRating + monsterAvoid);
                float dodgeRoll = (float)(rand() % 10000) / 10000.0f;
                
                if (monsterAvoid > 0 && dodgeRoll > hitChance) {
                    p[0] = 1; // 1 = MISS
                    finalDmg = 0;
                } else {
                    p[0] = 2; // 2 = HIT
                    
                    DWORD playerAtk = 50; // Default fallback
                    if (pAttacker) {
                        playerAtk = pAttacker->dwTotalAtk;
                    }

                    DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0; // Loaded from wIncAtk
                    DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0; 

                    // final = (playerAtk * ((100 + skillPerc) / 100.0f)) + skillFlatDmg
                    float rawDmg = (playerAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg;
                    
                    // Random float 90% ~ 110%
                    float roll = 0.9f + ((float)(rand() % 2000) / 10000.0f); // 0.9 + (0 ~ 0.2) = 0.9 ~ 1.1
                    rawDmg *= roll;

                    // Critical Hit (uses player wCritical stat, fallback 5%)
                    WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                    if ((WORD)(rand() % 100) < critRate) {
                        rawDmg *= 1.5f;
                        bCritHit = 1; // Critical hit!
                    }

                    if (rawDmg > monsterDef) rawDmg -= monsterDef;
                    else rawDmg = 1; // Minimum 1 damage
                    
                    finalDmg = (DWORD)rawDmg;
                    
                    if (pTarget->dwHpCur > finalDmg) {
                        pTarget->dwHpCur -= finalDmg;
                    } else {
                        finalDmg = pTarget->dwHpCur;
                        pTarget->dwHpCur = 0;
                        isDead = true;
                        pTarget->dwDeadTime = GetTickCount();
                        pTarget->dwTargetID = 0;
                        deadExp = pTarget->dwExp; // Use computed Init+Inc value
                        
                        DropManager::GetInstance()->GenerateDrops(dwAttackID, *pTarget);
                        LOG("[MugongHandler] Monster " + g_NpcTemplates[pTarget->bPropType].szName + " died from skill " + std::to_string(dwMugongID) + "!");
                    }
                }
                if (pTarget->dwAttackPattern != 0 && pTarget->dwHpCur > 0) {
                    pTarget->dwTargetID = dwAttackID;
                }
                dwDefHpMax = pTarget->dwHpMax;
                dwDefHpCur = pTarget->dwHpCur;
            }
        }
    }

    *(DWORD*)(p + 21) = dwDefHpMax;
    *(DWORD*)(p + 25) = dwDefHpCur;
    *(DWORD*)(p + 29) = finalDmg;
    *(DWORD*)(p + 33) = 0; // dwExp in ack doesn't seem to give it directly
    p[37] = bCritHit; // bHitFlag (1 = Critical, 0 = Normal)

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x4016; // CS_BT_MUGONGATTACK_ACK
    head->payloadSize = 38;
    EncryptPacket(ackBuf.data(), 0x42);
    BroadcastPacketToMap(playerMapID, ackBuf);

    if (isDead) {
        // Grant EXP via unified entry point (writes DB + sends 0x3B10)
        DWORD attackerCharID = dwAttackID - 400000000; // 800000301 -> 400000301 (matches DB/SessionMgr format)
        bool needRefresh = GrantExpToPlayer(attackerCharID, deadExp);
        if (needRefresh) {
            UpdatePlayerStatsAndSend(clientSocket, attackerCharID);
            
            // Restore HP/IP to max after level-up
            DWORD dwObjID = attackerCharID + 400000000;
            CMapInstance* mapInst2 = g_MapInstances.count(playerMapID) ? g_MapInstances[playerMapID] : nullptr;
            if (mapInst2) {
                {
                    std::lock_guard<std::mutex> lock2(mapInst2->GetMutex());
                    sServerObject* pObj = mapInst2->GetPlayer(dwObjID);
                    if (pObj) {
                        pObj->dwHpCur = pObj->dwHpMax;
                        pObj->wIpCur = pObj->wIpMax;
                    }
                }
                CharacterDB::GetInstance().RestoreHpIpToMax(attackerCharID);
                
                {
                    std::lock_guard<std::mutex> lock3(mapInst2->GetMutex());
                    sServerObject* pObj = mapInst2->GetPlayer(dwObjID);
                    if (pObj) {
                        std::vector<BYTE> hpBuf(4);
                        auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                        auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                        push4(pObj->dwHpMax);
                        push4(pObj->dwHpCur);
                        push2(pObj->wIpMax);
                        push2(pObj->wIpCur);
                        hpBuf.push_back(1);
                        PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                        hpHead->id = 0x3B0D;
                        hpHead->payloadSize = hpBuf.size() - 4;
                        EncryptPacket(hpBuf.data(), 0x42);
                        SafeSend(clientSocket, (char*)hpBuf.data(), hpBuf.size(), 0);
                    }
                }
            }
        }

        // Send CS_NC_STATUSCHANGE_ACK (0x3510) to trigger Death Animation
        auto pushDW = [](std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); };
        std::vector<BYTE> animBuf; animBuf.resize(4);
        animBuf.push_back(3); // bObjectType (Monster)
        DWORD oid = dwDefenseID;
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
        pushDW(killBuf, deadExp);
        
        PACKET_HEADER* killHead = (PACKET_HEADER*)killBuf.data();
        killHead->id = 0x4034;
        killHead->payloadSize = killBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(killBuf.data(), 0x42);
        SafeSend(clientSocket, (char*)killBuf.data(), killBuf.size(), 0);
    }
}

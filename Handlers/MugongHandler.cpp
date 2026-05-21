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
    // Determine if this is a buff skill - buff skills must use p[6]=0 to prevent
    // CharRender engine corruption. The engine's animation resource pipeline has an
    // internal bug where buff animation data pollutes the effect_count for subsequent
    // animations, causing SpawnEffect to fail (pool exhaustion / stale resource pointer).
    // Original game design: buff PreAttackAcks are silently dropped by client (p[6]=0).
    // Buff visual feedback comes from 0x402C (KeepUpMugongStartAck) instead.
    sMugongList* pPreMugong = MugongManager::GetInstance()->GetMugongLevelData(dwMugongID, bMugongLevel);
    bool isPreBuff = (pPreMugong && pPreMugong->dwKeepUpTime > 0);
    p[6] = isPreBuff ? 0 : 1;
    *(DWORD*)(p + 7) = dwAttackID;
    *(WORD*)(p + 11) = wAttackPosX;
    *(WORD*)(p + 13) = wAttackPosY;
    p[15] = bAttackHeight;
    // For self-buff/no-target skills, set defender to self with OBJTYPE_PC
    if (dwDefenseID == 0) {
        dwDefenseID = dwAttackID;
        bDefenseType = 1; // OBJTYPE_PC
    }
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

    // For buff skills (p[6]=0), the client will silently drop the PreAttackAck and
    // never send MugongAttackReq. So we must process the entire buff here.
    if (isPreBuff) {
        // 1. IP Cost deduction
        if (pPreMugong && pPreMugong->dwCostMp > 0) {
            if (g_MapInstances.count(playerMapID)) {
                CMapInstance* mapInst = g_MapInstances[playerMapID];
                std::lock_guard<std::mutex> lock(mapInst->GetMutex());
                sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
                if (pObj) {
                    if (pObj->wIpCur < (WORD)pPreMugong->dwCostMp) {
                        LOG("[MugongHandler] PreAttack Buff " + std::to_string(dwMugongID) + " blocked: IP insufficient");
                        return;
                    }
                    pObj->wIpCur -= (WORD)pPreMugong->dwCostMp;

                    // Send IP update (0x3B0D)
                    std::vector<BYTE> hpBuf(4);
                    auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                    auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                    push4(pObj->dwHpMax);
                    push4(pObj->dwHpCur);
                    push2(pObj->wIpMax);
                    push2(pObj->wIpCur);
                    hpBuf.push_back(0);
                    PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                    hpHead->id = 0x3B0D;
                    hpHead->payloadSize = hpBuf.size() - 4;
                    EncryptPacket(hpBuf.data(), 0x42);
                    SafeSend(clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);
                }
            }
        }

        // 2. Apply buff to player
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
            if (pObj) {
                if (pObj->activeBuffs.count(dwMugongID) > 0) {
                    pObj->activeBuffs[dwMugongID].dwEndTime = GetTickCount() + (pPreMugong->dwKeepUpTime * 1000);
                } else {
                    sServerObject::sActiveBuff newBuff;
                    newBuff.dwMugongID = dwMugongID;
                    newBuff.bLevel = bMugongLevel;
                    newBuff.dwEndTime = GetTickCount() + (pPreMugong->dwKeepUpTime * 1000);
                    newBuff.bIsDebuff = false;
                    pObj->activeBuffs[dwMugongID] = newBuff;
                }
            }
        }

        // 3. Broadcast KeepUpMugongStartAck (0x402C) - buff icon
        std::vector<BYTE> buffAck(4 + 11);
        BYTE* bp = buffAck.data() + 4;
        bp[0] = 0;
        *(DWORD*)(bp + 1) = dwAttackID;
        bp[5] = 1; // OBJTYPE_PC
        *(DWORD*)(bp + 6) = dwMugongID;
        bp[10] = bMugongLevel;
        PACKET_HEADER* headB = (PACKET_HEADER*)buffAck.data();
        headB->id = 0x402C;
        headB->payloadSize = 11;
        EncryptPacket(buffAck.data(), 0x42);
        BroadcastPacketToMap(playerMapID, buffAck);

        // 4. Recalculate stats
        DWORD buffCharID = dwAttackID - 400000000;
        PlayerManager::GetInstance().RecalculateStats(buffCharID, false);

        LOG("[MugongHandler] Buff " + std::to_string(dwMugongID) + " applied in PreAttackReq for player " + std::to_string(dwAttackID));
    }
}

bool IsAoeSkill(DWORD dwMugongID, sMugongTemplate* tpl) {
    if (!tpl) return false;
    // Five Elements skills (bKind 80-84) are AOE
    if (tpl->bKind >= 80 && tpl->bKind <= 84) return true;
    // Specific active weapon skills (IDs 80-120) are AOE
    if (dwMugongID >= 80 && dwMugongID <= 120) return true;
    return false;
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

    if (g_MapInstances.count(playerMapID)) {
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
        if (pObj && pObj->activeBuffs.count(130) > 0) {
            pObj->activeBuffs[130].dwEndTime = 0; // Mark for instant expiry in MonsterAI loop
            LOG("[MugongHandler] Player " + std::to_string(dwAttackID) + " used skill " + std::to_string(dwMugongID) + ". Expiring Turtle Breath.");
        }
    }

    // 1. Get the skill data
    sMugongList* pMugongData = MugongManager::GetInstance()->GetMugongLevelData(dwMugongID, bMugongLevel);
    
    // 2. Determine if it's a buff or heal
    bool isBuff = (pMugongData && pMugongData->dwKeepUpTime > 0);
    bool isHeal = (!isBuff && pMugongData && (pMugongData->wIncHpCur > 0 || pMugongData->wIncHpCurPerc > 0 || pMugongData->wIncIpCur > 0 || pMugongData->wIncIpCurPerc > 0) && dwMugongID != 37);

    // 2.5. Perform early IP cost validation and deduction for all non-healing skills
    if (!isHeal && dwMugongID != 63 && pMugongData && pMugongData->dwCostMp > 0) {
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
            if (pObj) {
                if (pObj->wIpCur < (WORD)pMugongData->dwCostMp) {
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

                LOG("[MugongHandler] Skill " + std::to_string(dwMugongID) + " IP cost deducted early: " 
                    + std::to_string(oldIp) + " - " + std::to_string(pMugongData->dwCostMp) 
                    + " = " + std::to_string(pObj->wIpCur));
            }
        }
    }

    // 3. Class 4: Healing / HP & IP Recovery Skills
    if (isHeal || dwMugongID == 63) {
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
            if (pObj) {
                // IP Cost deduction first
                DWORD cost = pMugongData ? pMugongData->dwCostMp : 0;
                if (pObj->wIpCur < (WORD)cost) {
                    LOG("[MugongHandler] Heal Skill " + std::to_string(dwMugongID) + " blocked: IP " 
                        + std::to_string(pObj->wIpCur) + " < cost " + std::to_string(cost));
                    return;
                }
                pObj->wIpCur -= (WORD)cost;

                // Calculate Heal Amounts
                DWORD hpHeal = 0;
                if (pMugongData) {
                    hpHeal = pMugongData->wIncHpCur;
                    if (pMugongData->wIncHpCurPerc > 0) {
                        hpHeal += (pObj->dwHpMax * pMugongData->wIncHpCurPerc / 100);
                    }
                }
                if (dwMugongID == 63 && hpHeal == 0) {
                    hpHeal = 200 + bMugongLevel * 100; // Fallback for level-based heal
                }

                DWORD ipHeal = 0;
                if (pMugongData) {
                    ipHeal = pMugongData->wIncIpCur;
                    if (pMugongData->wIncIpCurPerc > 0) {
                        ipHeal += (pObj->wIpMax * pMugongData->wIncIpCurPerc / 100);
                    }
                }

                // Apply Heals
                pObj->dwHpCur = (std::min)(pObj->dwHpMax, pObj->dwHpCur + hpHeal);
                pObj->wIpCur  = (std::min)(pObj->wIpMax, (WORD)(pObj->wIpCur + ipHeal));

                // Send 0x3B0D (HP/IP update) to the player
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

                LOG("[MugongHandler] Taoist Heal Skill " + std::to_string(dwMugongID) + " healed HP: " 
                    + std::to_string(hpHeal) + " (Cur: " + std::to_string(pObj->dwHpCur) + "/" + std::to_string(pObj->dwHpMax) + ")");
            }
        }

        // Broadcast AttackAck (0x4016) so the client plays the healing effect
        std::vector<BYTE> ackBuf(4 + 38);
        BYTE* p = ackBuf.data() + 4;
        p[0] = 0; // SUCCESS
        *(DWORD*)(p + 1) = dwMugongID;
        p[5] = bMugongLevel;
        p[6] = 1; // bAtkType = OBJTYPE_PC (client uses this in FindXiahObject)
        *(DWORD*)(p + 7) = dwAttackID;
        *(WORD*)(p + 11) = wAttackPosX;
        *(WORD*)(p + 13) = wAttackPosY;
        p[15] = bAttackHeight;
        p[16] = 1; // bDefenseType (Caster themselves as target)
        *(DWORD*)(p + 17) = dwAttackID;
        *(WORD*)(p + 21) = wAttackPosX;
        *(WORD*)(p + 23) = wAttackPosY;
        p[25] = bAttackHeight;
        
        // Caster status update in visual payload
        CMapInstance* mapInst = g_MapInstances.count(playerMapID) ? g_MapInstances[playerMapID] : nullptr;
        if (mapInst) {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwAttackID);
            if (pObj) {
                *(DWORD*)(p + 21) = pObj->dwHpMax;
                *(DWORD*)(p + 25) = pObj->dwHpCur;
            } else {
                *(DWORD*)(p + 21) = 0;
                *(DWORD*)(p + 25) = 0;
            }
        } else {
            *(DWORD*)(p + 21) = 0;
            *(DWORD*)(p + 25) = 0;
        }
        *(DWORD*)(p + 29) = 0; // finalDmg
        *(DWORD*)(p + 33) = 0; // deadExp
        p[37] = 0; // bCritHit

        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = 0x4016; // CS_BT_MUGONGATTACK_ACK
        head->payloadSize = 38;
        EncryptPacket(ackBuf.data(), 0x42);
        BroadcastPacketToMap(playerMapID, ackBuf);
        return; // Early return for healing skills
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
                    isDuplicate = true; // Avoid sending CS_BT_KEEPUPMUGONGSTART_ACK again
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
    }

    // IP cost was already verified and deducted early at step 2.5

    // 4. Broadcast AttackAck (0x4016)
    // IMPORTANT: We must ALWAYS send 0x4016, even for buff skills.
    // The client's attack state machine requires this ACK to complete the sequence.
    // Without it, the client gets stuck and all subsequent skills fail to render effects.
    // For skills with no target (dwDefenseID==0, including self-buffs), set defender to self
    // with OBJTYPE_PC(1) to prevent client NULL dereference in OnCS_BT_MUGONGATTACK_ACK.
    if (dwDefenseID == 0) {
        dwDefenseID = dwAttackID;
        bDefenseType = 1; // OBJTYPE_PC
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
    
    DWORD dwDefHpMax = 60000;
    DWORD dwDefHpCur = 60000;
    DWORD finalDmg = 0;
    DWORD deadExp = 0;
    bool isDead = false;
    BYTE bCritHit = 0;

    // Struct to store dead monsters to process EXP/animations outside the map lock safely
    struct sDeadEntity {
        DWORD dwObjectID;
        DWORD dwExp;
        std::string szName;
    };
    std::vector<sDeadEntity> deadEntities;

    if (!isBuff && dwMugongID != 64 && bDefenseType == 3) {
        if (g_MapInstances.count(playerMapID)) {
            CMapInstance* mapInst = g_MapInstances[playerMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            MonsterData* pTarget = mapInst->GetMonster(dwDefenseID);
            if (pTarget) {
                // Use template defense since MonsterData doesn't store wWepDef
                DWORD monsterDef = g_NpcTemplates.count(pTarget->bPropType) ? g_NpcTemplates[pTarget->bPropType].dwDefInit : 0;
                WORD monsterAvoid = pTarget->wAvoidRatio;

                PlayerData* pAttacker = mapInst->GetPlayer(dwAttackID);
                
                // Sacrifice Cost Logic (Class 5 skills)
                DWORD hpSacrificed = 0;
                bool isSacrifice = (dwMugongID == 37 || (pMugongData && pMugongData->wIncHpCurPerc > 0 && pMugongData->wIncHpCurPerc < 100));
                if (isSacrifice && pAttacker) {
                    BYTE pct = pMugongData ? pMugongData->wIncHpCurPerc : 20;
                    hpSacrificed = pAttacker->dwHpCur * pct / 100;
                    if (hpSacrificed > 0) {
                        pAttacker->dwHpCur = (std::max)(1UL, pAttacker->dwHpCur - hpSacrificed);
                        LOG("[MugongHandler] Sacrifice Skill " + std::to_string(dwMugongID) + " consumed " 
                            + std::to_string(hpSacrificed) + " HP (New HP: " + std::to_string(pAttacker->dwHpCur) + ")");
                        
                        // Send 0x3B0D to update client HP/IP bars
                        std::vector<BYTE> hpBuf(4);
                        auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                        auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                        push4(pAttacker->dwHpMax);
                        push4(pAttacker->dwHpCur);
                        push2(pAttacker->wIpMax);
                        push2(pAttacker->wIpCur);
                        hpBuf.push_back(0); // bType
                        PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                        hpHead->id = 0x3B0D;
                        hpHead->payloadSize = hpBuf.size() - 4;
                        EncryptPacket(hpBuf.data(), 0x42);
                        SafeSend(clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);
                    }
                }

                // Dodge Logic
                DWORD playerAtkRating = 50; 
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
                    
                    // Add sacrifice HP bonus
                    if (hpSacrificed > 0) {
                        rawDmg += hpSacrificed * 2.0f; // 2x HP sacrificed added as flat bonus damage!
                    }

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
                        
                        deadEntities.push_back({dwDefenseID, deadExp, g_NpcTemplates[pTarget->bPropType].szName});
                        DropManager::GetInstance()->GenerateDrops(dwAttackID, *pTarget);
                        LOG("[MugongHandler] Monster " + g_NpcTemplates[pTarget->bPropType].szName + " died from skill " + std::to_string(dwMugongID) + "!");
                    }
                }
                
                // AOE / Splash Damage Logic (Class 6 skills)
                sMugongTemplate* sTpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);
                if (IsAoeSkill(dwMugongID, sTpl)) {
                    std::vector<MonsterData*> aoiMonsters = mapInst->GetMonstersInAOI(wTargetPosX, wTargetPosY);
                    std::vector<MonsterData*> splashTargets;
                    
                    for (MonsterData* pMon : aoiMonsters) {
                        if (!pMon || pMon->dwObjectID == dwDefenseID || pMon->dwHpCur == 0) continue;
                        
                        // Check distance from target center (e.g. splash range 5.0 tiles)
                        float dx = (float)pMon->wPosX - (float)wTargetPosX;
                        float dy = (float)pMon->wPosY - (float)wTargetPosY;
                        float dist = sqrtf(dx * dx + dy * dy);
                        if (dist <= 5.0f) {
                            splashTargets.push_back(pMon);
                            if (splashTargets.size() >= 5) break; // Cap splash to 5 targets max
                        }
                    }

                    for (MonsterData* pSplashMon : splashTargets) {
                        DWORD sMonDef = g_NpcTemplates.count(pSplashMon->bPropType) ? g_NpcTemplates[pSplashMon->bPropType].dwDefInit : 0;
                        WORD sMonAvoid = pSplashMon->wAvoidRatio;
                        
                        // Dodge / Hit roll
                        DWORD sPlayerAtkRating = 50 + (pAttacker ? pAttacker->dwTotalHit : 0);
                        float sHitChance = (float)sPlayerAtkRating / (float)(sPlayerAtkRating + sMonAvoid);
                        float sRoll = (float)(rand() % 10000) / 10000.0f;
                        
                        BYTE sCritHit = 0;
                        DWORD sFinalDmg = 0;
                        BYTE sResult = 2; // HIT
                        
                        if (sMonAvoid > 0 && sRoll > sHitChance) {
                            sResult = 1; // MISS
                            sFinalDmg = 0;
                        } else {
                            DWORD pAtk = pAttacker ? pAttacker->dwTotalAtk : 50;
                            DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0;
                            DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0;
                            
                            // Splash damage factor: 70% of primary damage
                            float rawDmg = ((pAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg) * 0.7f;
                            
                            // Random float 90% ~ 110%
                            float var = 0.9f + ((float)(rand() % 2000) / 10000.0f);
                            rawDmg *= var;
                            
                            // Critical check
                            WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                            if ((WORD)(rand() % 100) < critRate) {
                                rawDmg *= 1.5f;
                                sCritHit = 1;
                            }
                            
                            if (rawDmg > sMonDef) rawDmg -= sMonDef;
                            else rawDmg = 1;
                            
                            sFinalDmg = (DWORD)rawDmg;
                            
                            if (pSplashMon->dwHpCur > sFinalDmg) {
                                pSplashMon->dwHpCur -= sFinalDmg;
                            } else {
                                sFinalDmg = pSplashMon->dwHpCur;
                                pSplashMon->dwHpCur = 0;
                                pSplashMon->dwDeadTime = GetTickCount();
                                pSplashMon->dwTargetID = 0;
                                
                                deadEntities.push_back({pSplashMon->dwObjectID, pSplashMon->dwExp, g_NpcTemplates[pSplashMon->bPropType].szName});
                                DropManager::GetInstance()->GenerateDrops(dwAttackID, *pSplashMon);
                                LOG("[MugongHandler] Splash target " + g_NpcTemplates[pSplashMon->bPropType].szName + " died from skill " + std::to_string(dwMugongID) + "!");
                            }
                        }
                        
                        if (pSplashMon->dwAttackPattern != 0 && pSplashMon->dwHpCur > 0) {
                            pSplashMon->dwTargetID = dwAttackID;
                        }
                        
                        // Broadcast 0x4016 (MugongAttackAck) for the splash target
                        std::vector<BYTE> splashAckBuf(4 + 38);
                        BYTE* sp = splashAckBuf.data() + 4;
                        sp[0] = sResult; 
                        *(DWORD*)(sp + 1) = dwMugongID;
                        sp[5] = bMugongLevel;
                        sp[6] = 1; // bAtkType = OBJTYPE_PC (client uses this in FindXiahObject)
                        *(DWORD*)(sp + 7) = dwAttackID;
                        *(WORD*)(sp + 11) = wAttackPosX;
                        *(WORD*)(sp + 13) = wAttackPosY;
                        sp[15] = bAttackHeight;
                        sp[16] = 3; // bDefenseType = 3 (Monster)
                        *(DWORD*)(sp + 17) = pSplashMon->dwObjectID;
                        *(DWORD*)(sp + 21) = pSplashMon->dwHpMax;
                        *(DWORD*)(sp + 25) = pSplashMon->dwHpCur;
                        *(DWORD*)(sp + 29) = sFinalDmg;
                        *(DWORD*)(sp + 33) = 0; 
                        sp[37] = sCritHit;
                        
                        PACKET_HEADER* sHead = (PACKET_HEADER*)splashAckBuf.data();
                        sHead->id = 0x4016;
                        sHead->payloadSize = 38;
                        EncryptPacket(splashAckBuf.data(), 0x42);
                        BroadcastPacketToMap(playerMapID, splashAckBuf);
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

    // For buff/self-cast skills, do NOT broadcast 0x4016.
    // The client's mugong attack flow sends MugongAttackReq directly (not via timer trigger),
    // so it doesn't need a 0x4016 to "complete" the state machine.
    // Sending 0x4016 with self-as-defender causes the client to process a "self-hit"
    // which corrupts the animation rendering state for subsequent skills.
    if (!isBuff) {
        BroadcastPacketToMap(playerMapID, ackBuf);
    }

    // Process exp granting, level-ups, and death animations for all dead monsters
    for (auto& de : deadEntities) {
        DWORD attackerCharID = dwAttackID - 400000000; // 800000301 -> 400000301 (matches DB/SessionMgr format)
        bool needRefresh = GrantExpToPlayer(attackerCharID, de.dwExp);
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

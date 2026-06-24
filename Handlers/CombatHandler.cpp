#include "CombatHandler.h"
#include "PartyHandler.h"
#include "../MonsterAI.h"
#include "../GameObjects/DropManager.h"
#include "../GameObjects/PlayerManager.h"
#include "../GameObjects/ExpSystem.h"
#include "../DB/CharacterDB.h"
#include "../GameObjects/MapInstance.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MugongManager.h"

extern std::map<DWORD, CMapInstance*> g_MapInstances;

void OnPreAttackReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    std::string hexDump = "";
    for (int i = 0; i < totalSize && i < 32; i++) {
        char buf[10]; sprintf(buf, "%02X ", payload[i]); hexDump += buf;
    }
    LOG("> RECEIVED 0x4003 PreAttackReq! Size: " + std::to_string(totalSize) + " Hex: " + hexDump);
    
    if (totalSize >= 20) {
        BYTE bAtkType = payload[0];
        DWORD dwAtkID = *(DWORD*)(payload + 1);
        WORD wAtkPosX = *(WORD*)(payload + 5);
        WORD wAtkPosY = *(WORD*)(payload + 7);
        BYTE bDefType = payload[10];
        DWORD dwDefID = *(DWORD*)(payload + 11);
        WORD wDefPosX = *(WORD*)(payload + 15);
        WORD wDefPosY = *(WORD*)(payload + 17);
        LOG("[OnPreAttackReq] Parse: CasterType=" + std::to_string(bAtkType) + " CasterID=" + std::to_string(dwAtkID) + " CasterPos=(" + std::to_string(wAtkPosX) + "," + std::to_string(wAtkPosY) + ") TargetType=" + std::to_string(bDefType) + " TargetID=" + std::to_string(dwDefID) + " TargetPos=(" + std::to_string(wDefPosX) + "," + std::to_string(wDefPosY) + ")");
    }
    
    // Build 0x4004 PreAttackAck ?echo payload back with id changed to 0x4004
    // Layout: [4-byte header] + [payload bytes] + optional [bAttackSpeed if PC attacker]
    std::vector<BYTE> ackBuf(totalSize + 4);
    memcpy(ackBuf.data() + 4, payload, totalSize);
    
    if (totalSize >= 1 && payload[0] == 1) { // OBJTYPE_PC attacker: append bAttackSpeed
        BYTE atkSpeed = 9; // default
        if (totalSize >= 9) {
            DWORD atkId = *(DWORD*)(payload + 1);
            WORD atkPosX = *(WORD*)(payload + 5);
            WORD atkPosY = *(WORD*)(payload + 7);
            DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
            if (g_MapInstances.count(pMapID)) {
                std::lock_guard<std::mutex> lock(g_MapInstances[pMapID]->GetMutex());
                PlayerData* pAtk = g_MapInstances[pMapID]->GetPlayer(atkId);
                if (pAtk) {
                    // 检查控制类 Debuff (定身/冰冻/眩晕) 拦截物理起手
                    bool isCC = false;
                    for (const auto& bf : pAtk->activeBuffs) {
                        if (bf.second.bIsDebuff) {
                            DWORD mugID = bf.second.dwMugongID;
                            if (mugID == 94 || mugID == 95 || mugID == 35 || mugID == 65 || mugID == 125) {
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
                        std::string msg = "[Control] You are frozen, stunned or immobilized and cannot attack!";
                        WORD len = (WORD)msg.size();
                        buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
                        buf.insert(buf.end(), msg.begin(), msg.end());
                        WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
                        WORD payloadSize = (WORD)(buf.size() - 4);
                        memcpy(&buf[0], &packetID, 2);
                        memcpy(&buf[2], &payloadSize, 2);
                        EncryptPacket(buf.data(), 0x42);
                        SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
                        return;
                    }

                    if (pAtk->activeBuffs.count(130) > 0) {
                        pAtk->activeBuffs[130].dwEndTime = 0; // Mark for instant expiry in MonsterAI loop
                        LOG("[CombatHandler] Player " + std::to_string(atkId) + " pre-attacked. Expiring Turtle Breath.");
                    }
                    if (pAtk->activeBuffs.count(178) > 0) {
                        pAtk->activeBuffs[178].dwEndTime = 0; // Mark for instant expiry in MonsterAI loop
                        LOG("[CombatHandler] Player " + std::to_string(atkId) + " pre-attacked. Expiring Stealth.");
                    }
                    // wAtkSpeed not in PlayerData; use default 9
                    // Sync position from attack packet (client may not send ENDMOVE when auto-walking to target)
                    if (atkPosX > 0 && atkPosX < 2048 && atkPosY > 0 && atkPosY < 2048) {
                        int oldX = pAtk->wPosX, oldY = pAtk->wPosY;
                        pAtk->wPosX = atkPosX;
                        pAtk->wPosY = atkPosY;
                        pAtk->fPosX = (float)atkPosX;
                        pAtk->fPosY = (float)atkPosY;
                        pAtk->bIsMoving = false;
                        g_MapInstances[pMapID]->UpdatePlayerGrid(atkId, oldX, oldY, atkPosX, atkPosY);
                    }
                }
            }
        }
        ackBuf.push_back(atkSpeed);
    }

    PACKET_HEADER* mh = (PACKET_HEADER*)ackBuf.data();
    mh->id = 0x4004; // CS_BT_PREATTACK_ACK
    mh->payloadSize = (WORD)(ackBuf.size() - 4);

    EncryptPacket(ackBuf.data(), 0x42);
    DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    BroadcastPacketToMap(playerMapID, ackBuf);
}

void OnAttackHitReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    LOG(">> RECEIVED 0x4005 AttackHitReq! Size: " + std::to_string(totalSize));
    std::string hexDump = "";
    for (int i = 0; i < totalSize && i < 32; i++) {
        char buf[10]; sprintf(buf, "%02X ", payload[i]); hexDump += buf;
    }
    LOG("[CombatHandler] ATTACK REQ 0x4005 Size: " + std::to_string(totalSize) + " Hex: " + hexDump);

    if (totalSize >= 15) {
        DWORD attackerId = *(DWORD*)(payload + 1);
        DWORD targetId = *(DWORD*)(payload + 11);
        DWORD playerMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        if (!g_MapInstances.count(playerMapID)) return;
        CMapInstance* mapInst = g_MapInstances[playerMapID];
        
        DWORD finalDmg = 50;
        DWORD dwHpMax = 60000, dwHpCur = 60000;
        BYTE bResult = 2; // Hit Success
        BYTE bHitFlag = 0; // 0 = Normal, 1 = Critical
        bool monsterDied = false;
        bool needStatusRefresh = false;
        DWORD attackerCharID = 0;
        BYTE deadObjType = 0; DWORD deadObjID = 0; BYTE deadPropType = 0; DWORD deadExp = 0;
        std::vector<DWORD> partyExpMembers;
        MonsterData deadMonsterCopy; // copy for GenerateDrops outside mutex
        bool hasDeadMonsterCopy = false;
        {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            PlayerData* pAttacker = mapInst->GetPlayer(attackerId);
            if (pAttacker) {
                // 检查控制类 Debuff (定身/冰冻/眩晕) 拦截物理命中伤害
                bool isCC = false;
                for (const auto& bf : pAttacker->activeBuffs) {
                    if (bf.second.bIsDebuff) {
                        DWORD mugID = bf.second.dwMugongID;
                        if (mugID == 94 || mugID == 95 || mugID == 35 || mugID == 65 || mugID == 125) {
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
                    std::string msg = "[Control] You are frozen, stunned or immobilized and cannot attack!";
                    WORD len = (WORD)msg.size();
                    buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
                    buf.insert(buf.end(), msg.begin(), msg.end());
                    WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
                    WORD payloadSize = (WORD)(buf.size() - 4);
                    memcpy(&buf[0], &packetID, 2);
                    memcpy(&buf[2], &payloadSize, 2);
                    EncryptPacket(buf.data(), 0x42);
                    SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
                    return;
                }

                if (pAttacker->activeBuffs.count(130) > 0) {
                    pAttacker->activeBuffs[130].dwEndTime = 0; // Mark for instant expiry in MonsterAI loop
                    LOG("[CombatHandler] Player " + std::to_string(attackerId) + " attacked. Expiring Turtle Breath.");
                }
                if (pAttacker->activeBuffs.count(178) > 0) {
                    pAttacker->activeBuffs[178].dwEndTime = 0; // Mark for instant expiry in MonsterAI loop
                    LOG("[CombatHandler] Player " + std::to_string(attackerId) + " attacked. Expiring Stealth.");
                }
                finalDmg = pAttacker->dwTotalAtk;
                if (finalDmg == 0) finalDmg = 50; // Fallback
                
                LOG("[CombatHandler] Player " + std::to_string(attackerId) + " attacks with Dmg: " + std::to_string(finalDmg) + " (TotalAtk: " + std::to_string(pAttacker->dwTotalAtk) + ")");
            }
            
            MonsterData* pTarget = mapInst->GetMonster(targetId);
            if (pTarget && pTarget->dwHpCur > 0) {
                if (pTarget->bIsReturning) {
                    bResult = 1; // Miss during leash return
                    finalDmg = 0;
                } else {
                    DWORD monsterDef = 0; // MonsterData has no wWepDef; use template
                    if (g_NpcTemplates.count(pTarget->bPropType))
                        monsterDef = g_NpcTemplates[pTarget->bPropType].dwDefInit;
                    
                    // 鍔ㄦ€佹墸鍑忥細妫€娴嬪苟搴旂敤鎬�墿褰撳墠鍙楀埌鐨勫噺闃茬瓑 Debuff 灞炴€ф晥鏋�
                    for (auto& bf : pTarget->activeBuffs) {
                        sMugongList* bd = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                        if (bd) {
                            if (bd->wIncDefPerc > 0) {
                                monsterDef = monsterDef * bd->wIncDefPerc / 100;
                            }
                            if (bd->wIncDef > 0) {
                                if (monsterDef > bd->wIncDef) monsterDef -= bd->wIncDef; else monsterDef = 0;
                            }
                        }
                    }

                    WORD monsterAvoid = pTarget->wAvoidRatio;
                    
                    // Xiah Dodge Logic
                    DWORD playerAtkRating = 50 + (pAttacker ? pAttacker->dwTotalHit : 0);
                    float hitChance = (float)playerAtkRating / (float)(playerAtkRating + monsterAvoid);
                    
                    float roll = (float)(rand() % 10000) / 10000.0f;
                    
                    if (monsterAvoid > 0 && roll > hitChance) {
                        bResult = 1; // 1 = Miss
                        finalDmg = 0;
                    } else {
                        // Damage variance +/-10%
                        float dmgFloat = (float)finalDmg;
                        float variance = 0.9f + ((float)(rand() % 2000) / 10000.0f);
                        dmgFloat *= variance;
                        
                        // Critical Hit (uses player wCritical stat, fallback 5%)
                        WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                        if ((WORD)(rand() % 100) < critRate) {
                            dmgFloat *= 2.0f;
                            bHitFlag = 1; // Critical hit!
                        }
                        
                        finalDmg = (DWORD)dmgFloat;
                        if (finalDmg > monsterDef) finalDmg -= monsterDef;
                        else finalDmg = 1; 
                    }
                    
                    pTarget->dwHpCur = (pTarget->dwHpCur > finalDmg) ? (pTarget->dwHpCur - finalDmg) : 0;

                    // 物理攻击命中蓄力：攻击者开启五行状态且成功命中时
                    if (pAttacker && pAttacker->bCurFiveElm > 0 && bResult == 2) {
                        pAttacker->dwFiveElmGauge += 5;
                        if (pAttacker->dwFiveElmGauge > 5000) pAttacker->dwFiveElmGauge = 5000;
                        SyncFiveElmStatus(clientSocket, pAttacker->dwObjectID - 400000000, pAttacker);
                    }
                }
                
                // Xiah AI Bitmask - NON-COMBAT (0) vs PASSIVE/ACTIVE
                // Only set aggro if the monster is not completely passive (0) and not returning
                if (pTarget->dwAttackPattern != 0 && !pTarget->bIsReturning) {
                    pTarget->dwTargetID = attackerId; // Set aggro (Even if it is Passive (2), it will fight back now)
                    LOG("[CombatHandler] SET AGGRO: Monster ObjID=" + std::to_string(targetId) + " -> targetID=" + std::to_string(attackerId) + " mapID=" + std::to_string(playerMapID));
                    
                    // Xiah AI Bitmask - ASSIST AGGRO / LINK AGGRO (Bit 3 / 8)
                    if ((pTarget->dwAttackPattern & 8) != 0) {
                        for (auto& pPair : mapInst->GetMonsters()) {
                            auto& friendObj = pPair.second;
                            if (friendObj.bObjectType == 3 && friendObj.dwHpCur > 0 && friendObj.dwTargetID == 0) {
                                if (friendObj.bPropType == pTarget->bPropType) {
                                    float dx = (float)friendObj.wPosX - (float)pTarget->wPosX;
                                    float dy = (float)friendObj.wPosY - (float)pTarget->wPosY;
                                    float dist = std::sqrt(dx*dx + dy*dy);
                                    if (dist <= 20.0f) {
                                        friendObj.dwTargetID = attackerId;
                                    }
                                }
                            }
                        }
                    }
                }
                if (pTarget->dwHpCur == 0) {
                    pTarget->dwDeadTime = GetTickCount();
                    pTarget->dwTargetID = 0;
                    monsterDied = true;
                    
                    // 击杀蓄气：攻击者开启五行状态且击杀怪物时，在内存中增加 50 蓄力点，随后在 GrantExpToPlayer 里会自动写库落盘并发送 0x3B10 包
                    if (pAttacker && pAttacker->bCurFiveElm > 0) {
                        pAttacker->dwFiveElmGauge += 50;
                        if (pAttacker->dwFiveElmGauge > 5000) pAttacker->dwFiveElmGauge = 5000;
                    }

                    deadObjType = pTarget->bObjectType;
                    deadObjID = pTarget->dwObjectID;
                    deadPropType = pTarget->bPropType;
                    deadExp = pTarget->dwExp;
                    LOG("[CombatHandler] Monster " + g_NpcTemplates[deadPropType].szName + " died!");
                    
                    attackerCharID = (attackerId >= 850000000 && pAttacker && pAttacker->dwOwnerID > 0) ? pAttacker->dwOwnerID : attackerId - 400000000;
                    
                    // 计算被击杀怪物提供的五行经验值
                    DWORD monsterFiveElmExp = 0;
                    if (g_NpcTemplates.count(deadPropType)) {
                        monsterFiveElmExp = g_NpcTemplates[deadPropType].wFiveElmExp;
                    }
                    monsterFiveElmExp += pTarget->wIncFiveElmExp;
                    
                    // Party EXP sharing
                    DWORD partyID = PartyManager::GetInstance().GetPartyID(attackerCharID);
                    BYTE expShareMode = (partyID != 0) ? PartyManager::GetInstance().GetExpShareMode(partyID) : 0;
                    
                    if (partyID != 0 && expShareMode == 1) {
                        // Shared mode: find nearby same-map party members
                        auto members = PartyManager::GetInstance().GetMembers(partyID);
                        std::vector<DWORD> nearbyMembers;
                        
                        // Get attacker position for distance check
                        WORD atkX = 0, atkY = 0;
                        PlayerData* pAtk2 = mapInst->GetPlayer(attackerId);
                        if (pAtk2) { atkX = pAtk2->wPosX; atkY = pAtk2->wPosY; }
                        
                        for (auto& m : members) {
                            SOCKET mSock = SessionMgr::GetInstance().GetSocketByCharID(m.dwCharID);
                            if (mSock != INVALID_SOCKET) {
                                DWORD mMap = SessionMgr::GetInstance().GetMapID(mSock);
                                if (mMap == playerMapID) {
                                    // Distance check (within AOI range ~50 tiles)
                                    DWORD mObjID = m.dwCharID + 400000000;
                                    PlayerData* mObj = mapInst->GetPlayer(mObjID);
                                    if (mObj) {
                                        float dx = (float)mObj->wPosX - (float)atkX;
                                        float dy = (float)mObj->wPosY - (float)atkY;
                                        float dist = std::sqrt(dx*dx + dy*dy);
                                        if (dist <= 50.0f) {
                                            nearbyMembers.push_back(m.dwCharID);
                                        }
                                    }
                                }
                            }
                        }
                        if (nearbyMembers.empty()) nearbyMembers.push_back(attackerCharID);
                        
                        DWORD sharedExp = deadExp / (DWORD)nearbyMembers.size();
                        if (sharedExp == 0) sharedExp = 1;
                        
                        DWORD sharedFiveElmExp = monsterFiveElmExp / (DWORD)nearbyMembers.size();
                        
                        LOG("[PARTY-EXP] Party " + std::to_string(partyID) + 
                            " sharing " + std::to_string(deadExp) + " EXP & " + std::to_string(monsterFiveElmExp) + " FiveElmEXP among " + 
                            std::to_string(nearbyMembers.size()) + " nearby members (" + 
                            std::to_string(sharedExp) + " each)");
                        
                        for (DWORD memberCharID : nearbyMembers) {
                            bool lvlUp = GrantExpToPlayer(memberCharID, sharedExp, sharedFiveElmExp);
                            if (memberCharID == attackerCharID) {
                                needStatusRefresh = lvlUp;
                            } else if (lvlUp) {
                                SOCKET mSock = SessionMgr::GetInstance().GetSocketByCharID(memberCharID);
                                if (mSock != INVALID_SOCKET) {
                                    UpdatePlayerStatsAndSend(mSock, memberCharID);
                                    DWORD mObjID = memberCharID + 400000000;
                                    PlayerData* mObj = mapInst->GetPlayer(mObjID);
                                    if (mObj) {
                                        mObj->dwHpCur = mObj->dwHpMax;
                                        mObj->wIpCur = mObj->wIpMax;
                                    }
                                }
                            }
                        }
                        partyExpMembers = nearbyMembers;
                    } else {
                        // Individual mode or not in party
                        needStatusRefresh = GrantExpToPlayer(attackerCharID, deadExp, monsterFiveElmExp);
                    }
                    
                    // Save monster copy for GenerateDrops (called after mutex release)
                    deadMonsterCopy = *pTarget;
                    hasDeadMonsterCopy = true;
                }
                
                
                // Hit Stagger: push attack timer forward by wStaggerTime, but don't fully reset
                // This gives a brief hit reaction without permanently preventing attacks
                WORD staggerMs = g_NpcTemplates[pTarget->bPropType].wStaggerTime;
                if (staggerMs > 0) {
                    WORD atkInterval = g_NpcTemplates[pTarget->bPropType].wAtkInterval;
                    if (atkInterval > staggerMs) {
                        DWORD staggerTime = GetTickCount() - (atkInterval - staggerMs);
                        if (staggerTime > pTarget->dwLastAttackTime) {
                            pTarget->dwLastAttackTime = staggerTime;
                        }
                    }
                }
                dwHpMax = pTarget->dwHpMax;
                dwHpCur = pTarget->dwHpCur;
            }

            PlayerData* pTargetPlayer = mapInst->GetPlayer(targetId);
            if (pTargetPlayer) {
                // 物理 PvP 伤害与命中判定分支
                if (pAttacker) {
                    // A. 组队（队友）免伤保护 (最高级硬核保护)
                    DWORD pID1 = PartyManager::GetInstance().GetPartyID(attackerId - 400000000);
                    DWORD pID2 = PartyManager::GetInstance().GetPartyID(pTargetPlayer->dwObjectID - 400000000);
                    if (pID1 != 0 && pID1 == pID2) {
                        bResult = 1; // MISS
                        finalDmg = 0;
                        LOG("[CombatHandler] Friendly fire physical attack blocked: " + pAttacker->szName + " -> " + pTargetPlayer->szName);
                        goto APPLY_PHYSICAL_DAMAGE;
                    }
                    
                    // B. 安全非PK模式保护校验
                    if (pAttacker->bSafeMode != 2) {
                        bResult = 1; // MISS
                        finalDmg = 0;
                        LOG("[CombatHandler] Safe Mode physical attack blocked: " + pAttacker->szName + " -> " + pTargetPlayer->szName);
                        goto APPLY_PHYSICAL_DAMAGE;
                    }
                }
                
                // 命中计算（物理平A PK 命中率计算）
                DWORD playerAtkRating = 50 + (pAttacker ? pAttacker->dwTotalHit : 0);
                DWORD targetDodge = pTargetPlayer->dwTotalDodge;
                float hitChance = (float)playerAtkRating / (float)(playerAtkRating + targetDodge);
                float roll = (float)(rand() % 10000) / 10000.0f;
                
                if (targetDodge > 0 && roll > hitChance) {
                    bResult = 1; // MISS
                    finalDmg = 0;
                } else if (pTargetPlayer->activeBuffs.count(130) > 0) {
                    // 130 龟息大法 PVP 免疫
                    bResult = 1; // MISS
                    finalDmg = 0;
                } else {
                    if (pTargetPlayer->activeBuffs.count(178) > 0) {
                        pTargetPlayer->activeBuffs[178].dwEndTime = 0; // 破隐
                    }
                    bResult = 2; // HIT
                    
                    float dmgFloat = (float)finalDmg;
                    float variance = 0.9f + ((float)(rand() % 2000) / 10000.0f);
                    dmgFloat *= variance;
                    
                    WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                    if ((WORD)(rand() % 100) < critRate) {
                        dmgFloat *= 1.5f;
                        bHitFlag = 1; // Critical
                    }
                    
                    finalDmg = (DWORD)dmgFloat;
                    DWORD targetDef = pTargetPlayer->dwTotalDef;
                    if (finalDmg > targetDef) finalDmg -= targetDef;
                    else finalDmg = 1;
                    
                    pTargetPlayer->dwHpCur = (pTargetPlayer->dwHpCur > finalDmg) ? (pTargetPlayer->dwHpCur - finalDmg) : 0;

                    // 物理受创蓄力：被击中玩家开启五行状态且成功命中时
                    if (pTargetPlayer->bCurFiveElm > 0 && bResult == 2) {
                        pTargetPlayer->dwFiveElmGauge += 10;
                        if (pTargetPlayer->dwFiveElmGauge > 5000) pTargetPlayer->dwFiveElmGauge = 5000;
                        SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(pTargetPlayer->dwObjectID - 400000000);
                        if (targetSock != INVALID_SOCKET) {
                            SyncFiveElmStatus(targetSock, pTargetPlayer->dwObjectID - 400000000, pTargetPlayer);
                        }
                    }
                    if (pTargetPlayer->dwHpCur == 0) {
                        pTargetPlayer->dwDeadTime = GetTickCount();
                        monsterDied = true; // 复用死亡动画触发机制
                        deadObjType = 1; // OBJTYPE_PC
                        deadObjID = pTargetPlayer->dwObjectID;

                        // [业务设计意图]
                        // 玩家物理受击死亡时，必须立刻在服务端清理名下的分身，防止怪物转火分身引发客户端镜像生成和崩溃。
                        // [潜在风险]
                        extern void CleanupAllBunsins(DWORD ownerCharID, DWORD mapID);
                        CleanupAllBunsins(pTargetPlayer->dwObjectID - 400000000, playerMapID);

                        extern void ClearPlayerBuffsOnDeath(PlayerData& player, DWORD mapID);
                        ClearPlayerBuffsOnDeath(*pTargetPlayer, playerMapID);

                        LOG("[CombatHandler] Player " + pTargetPlayer->szName + " died from physical attack by " + (pAttacker ? pAttacker->szName : "Unknown"));
                    }
                }
                
APPLY_PHYSICAL_DAMAGE:
                dwHpMax = pTargetPlayer->dwHpMax;
                dwHpCur = pTargetPlayer->dwHpCur;
            }
        } // unlock

        // Send status refresh AFTER mutex release (avoids deadlock)
        if (needStatusRefresh && attackerCharID != 0) {
            // Recalculate all stats (level affects HP/IP max) and send 0x4414
            UpdatePlayerStatsAndSend(clientSocket, attackerCharID);
            
            // Restore HP/IP to max after level-up
            DWORD dwObjectID = attackerCharID + 400000000;
            {
                std::lock_guard<std::mutex> lock2(mapInst->GetMutex());
                PlayerData* pObj = mapInst->GetPlayer(dwObjectID);
                if (pObj) {
                    pObj->dwHpCur = pObj->dwHpMax;
                    pObj->wIpCur = pObj->wIpMax;
                }
            }
            // Update DB
            CharacterDB::GetInstance().RestoreHpIpToMax(attackerCharID);
            
            // Send HP/IP bar update to client
            {
                std::lock_guard<std::mutex> lock3(mapInst->GetMutex());
                PlayerData* pObj = mapInst->GetPlayer(dwObjectID);
                if (pObj) {
                    std::vector<BYTE> hpBuf(4);
                    auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                    auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                    push4(pObj->dwHpMax);
                    push4(pObj->dwHpCur);
                    push4(pObj->wIpMax);
                    push4(pObj->wIpCur);
                    hpBuf.push_back(1); // bType = 1 (with restore effect)
                    PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                    hpHead->id = 0x3B0D;
                    hpHead->payloadSize = hpBuf.size() - 4;
                    EncryptPacket(hpBuf.data(), 0x42);
                    SafeSend(clientSocket, (char*)hpBuf.data(), hpBuf.size(), 0);
                }
            }
        }

        // 1. Send 0x4006 (Damage ACK to attacker)
        std::vector<BYTE> ackBuf; ackBuf.reserve(64);
        ackBuf.push_back(bResult);
        for (int i=0; i<10; i++) ackBuf.push_back(payload[i]); // bAtkType to bAtkHeight
        ackBuf.push_back(payload[10]); // bDefType
        for (int i=11; i<15; i++) ackBuf.push_back(payload[i]); // dwDefID
        
        auto pushDWord = [&](std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); };
        
        pushDWord(ackBuf, dwHpMax); // dwDefHpMax
        pushDWord(ackBuf, dwHpCur); // dwDefHpCur
        pushDWord(ackBuf, finalDmg); // wDamage
        pushDWord(ackBuf, 10);      // dwExp
        ackBuf.push_back(payload[15]); // bAttackMode
        ackBuf.push_back(bHitFlag); // bHitFlag (1 = Critical)
        
        std::vector<BYTE> fullAck; fullAck.resize(4); fullAck.insert(fullAck.end(), ackBuf.begin(), ackBuf.end());
        PACKET_HEADER* ackHead = (PACKET_HEADER*)fullAck.data(); ackHead->id = 0x4006; ackHead->payloadSize = ackBuf.size();
        EncryptPacket(fullAck.data(), 0x42);
        BroadcastPacketToMap(playerMapID, fullAck);

        // 3. Send 0x3510 (CS_NC_STATUSCHANGE_ACK) to trigger Death Animation
        if (monsterDied) {
            std::vector<BYTE> animBuf; animBuf.resize(4);
            animBuf.push_back(deadObjType); // bObjType
            pushDWord(animBuf, deadObjID);
            animBuf.push_back(3); // bStatus = 3 (Dead)
            animBuf.push_back(0); // wValue1 L
            animBuf.push_back(0); // wValue1 H
            animBuf.push_back(0xFF); // bValue2
            
            PACKET_HEADER* animHead = (PACKET_HEADER*)animBuf.data(); animHead->id = 0x3510; animHead->payloadSize = animBuf.size() - sizeof(PACKET_HEADER);
            EncryptPacket(animBuf.data(), 0x42);
            BroadcastPacketToMap(playerMapID, animBuf);
            
            // Send CS_BT_KILLSUCCESS_ACK (0x4034) to trigger EXP acquire VFX
            DWORD displayExp = deadExp;
            if (!partyExpMembers.empty()) {
                displayExp = deadExp / (DWORD)partyExpMembers.size();
                if (displayExp == 0) displayExp = 1;
                // Send to all party members on same map
                for (DWORD memberCharID : partyExpMembers) {
                    SOCKET mSock = SessionMgr::GetInstance().GetSocketByCharID(memberCharID);
                    if (mSock != INVALID_SOCKET) {
                        std::vector<BYTE> killBuf; killBuf.resize(4);
                        killBuf.push_back(deadObjType);
                        pushDWord(killBuf, deadObjID);
                        pushDWord(killBuf, displayExp);
                        PACKET_HEADER* killHead = (PACKET_HEADER*)killBuf.data(); killHead->id = 0x4034; killHead->payloadSize = killBuf.size() - sizeof(PACKET_HEADER);
                        EncryptPacket(killBuf.data(), 0x42);
                        SafeSend(mSock, (char*)killBuf.data(), killBuf.size(), 0);
                    }
                }
            } else {
                std::vector<BYTE> killBuf; killBuf.resize(4);
                killBuf.push_back(deadObjType);
                pushDWord(killBuf, deadObjID);
                pushDWord(killBuf, displayExp);
                PACKET_HEADER* killHead = (PACKET_HEADER*)killBuf.data(); killHead->id = 0x4034; killHead->payloadSize = killBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(killBuf.data(), 0x42);
                SafeSend(clientSocket, (char*)killBuf.data(), killBuf.size(), 0);
            }
        }

        // Generate drops AFTER all combat packets (0x4006, 0x3510, 0x4034)
        if (monsterDied && hasDeadMonsterCopy) {
            DropManager::GetInstance()->GenerateDrops(attackerId, deadMonsterCopy);
        }
    }
}

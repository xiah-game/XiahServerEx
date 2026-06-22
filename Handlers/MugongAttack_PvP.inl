    else if (!isBuff && dwMugongID != 64 && bDefenseType == 1 && !bNoRealTarget) {

        // PvP (Player vs Player) ���

        if (g_MapInstances.count(playerMapID)) {

            CMapInstance* mapInst = g_MapInstances[playerMapID];

            std::lock_guard<std::mutex> lock(mapInst->GetMutex());

            sServerObject* pTarget = mapInst->GetPlayer(dwDefenseID);

            if (pTarget) {

                PlayerData* pAttacker = mapInst->GetPlayer(dwAttackID);



                // ��ֹȺ����ɱ����Ŀ�����Լ���������Ŀ���˺�����ִ�� AOE ����

                if (dwDefenseID == dwAttackID) goto pvp_aoe_only;

                

                // �������

                DWORD hpSacrificed = 0;

                bool isSacrifice = (tpl && tpl->bType == 3 && tpl->bKind == 18);

                if (isSacrifice && pAttacker) {

                    BYTE pct = pMugongData ? pMugongData->wIncHpCurPerc : 20;

                    hpSacrificed = pAttacker->dwHpCur * pct / 100;

                    if (hpSacrificed > 0) {

                        pAttacker->dwHpCur = (std::max)(1UL, pAttacker->dwHpCur - hpSacrificed);

                        LOG("[MugongHandler] PvP Sacrifice Skill " + std::to_string(dwMugongID) + " consumed " 

                            + std::to_string(hpSacrificed) + " HP");

                        

                        // ���㨨����

                        std::vector<BYTE> hpBuf(4);

                        auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };

                        auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };

                        push4(pAttacker->dwHpMax);

                        push4(pAttacker->dwHpCur);

                        push4(pAttacker->wIpMax);

                        push4(pAttacker->wIpCur);

                        hpBuf.push_back(0); 

                        PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();

                        hpHead->id = 0x3B0D;

                        hpHead->payloadSize = hpBuf.size() - 4;

                        EncryptPacket(hpBuf.data(), 0x42);

                        SafeSend(clientSocket, (const char*)hpBuf.data(), hpBuf.size(), 0);

                    }

                }



                // PvP �訦������

                DWORD playerAtkRating = 50; 

                if (pAttacker) {

                    playerAtkRating += pAttacker->dwTotalHit;

                }

                DWORD targetDodge = pTarget->dwTotalDodge;

                

                float hitChance = (float)playerAtkRating / (float)(playerAtkRating + targetDodge);

                float dodgeRoll = (float)(rand() % 10000) / 10000.0f;

                

                if (targetDodge > 0 && dodgeRoll > hitChance) {

                    p[0] = 1; // MISS

                    finalDmg = 0;

                } else if (pTarget->activeBuffs.count(130) > 0) {

                    // 130 ����� PVP ����

                    p[0] = 1; // MISS (��)

                    finalDmg = 0;

                    LOG("[MugongPvp] Player " + pTarget->szName + " protected by Turtle Breath. Zero Damage.");

                } else {

                    // ������ 178 ��

                    if (pTarget->activeBuffs.count(178) > 0) {

                        pTarget->activeBuffs[178].dwEndTime = 0; // �訦

                        LOG("[MugongPvp] Player " + pTarget->szName + " hit while stealth. Expiring Stealth.");

                    }

                    p[0] = 2; // HIT

                    

                    DWORD playerAtk = 50;

                    if (pAttacker) {

                        playerAtk = pAttacker->dwTotalAtk;

                    }



                    DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0;

                    DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0; 


                    float rawDmg;
                    // bType=3, bKind=16/21/24（固定加伤）：总攻击力 + 技能固定攻击力
                    if (tpl && tpl->bType == 3 && (tpl->bKind == 16 || tpl->bKind == 21 || tpl->bKind == 24)) {
                        rawDmg = (float)playerAtk + (float)skillFlatDmg;
                    }
                    // bType=3, bKind=17（远程投射攻击）：倍率只乘武器攻击力
                    else if (tpl && tpl->bType == 3 && tpl->bKind == 17 && pAttacker) {
                        DWORD weaponAtk = pAttacker->wWepAtk;
                        DWORD otherAtk = (playerAtk > weaponAtk) ? (playerAtk - weaponAtk) : 0;
                        rawDmg = (float)(weaponAtk * ((100.0f + skillPerc) / 100.0f)) + (float)otherAtk + (float)skillFlatDmg;
                    }
                    // bType=3, bKind=18（以血换血，如玉石俱焚）：
                    // 公式 = 牺牲血量 × nEtc1%，不叠加攻击力
                    else if (tpl && tpl->bType == 3 && tpl->bKind == 18 && hpSacrificed > 0) {
                        int etc1 = pMugongData ? pMugongData->nEtc1 : 200;
                        rawDmg = (float)hpSacrificed * (float)etc1 / 100.0f;
                    }
                    // bType=3, bKind=19（御剑术）：武器攻击力 × wIncAtkPerc%
                    else if (tpl && tpl->bType == 3 && tpl->bKind == 19 && pAttacker) {
                        rawDmg = (float)pAttacker->wWepAtk * (float)skillPerc / 100.0f;
                    }
                    // bType=3, bKind=0/23（群体攻击/一击残）：总攻击力 × 倍率
                    else if (tpl && tpl->bType == 3 && (tpl->bKind == 0 || tpl->bKind == 23)) {
                        rawDmg = (float)playerAtk * (float)skillPerc / 100.0f;
                    } else {
                        rawDmg = (playerAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg;
                    }

                    if (hpSacrificed > 0 && !(tpl && tpl->bType == 3 && tpl->bKind == 18)) {

                        rawDmg += hpSacrificed * 2.0f; 

                    }



                    float roll = 0.9f + ((float)(rand() % 2000) / 10000.0f);

                    rawDmg *= roll;



                    WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;

                    if ((WORD)(rand() % 100) < critRate) {

                        rawDmg *= 1.5f;

                        bCritHit = 1; // 

                    }



                    // ���訦��

                    DWORD targetDef = pTarget->dwTotalDef;

                    if (rawDmg > targetDef) rawDmg -= targetDef;

                    else rawDmg = 1;

                    // bType=4, bKind=5（狂魔毒功/神功）：防御方受伤放大 nEtc1%
                    if (pTarget) {
                        for (auto& bf : pTarget->activeBuffs) {
                            sMugongTemplate* bfTpl = MugongManager::GetInstance()->GetTemplate(bf.second.dwMugongID);
                            if (bfTpl && bfTpl->bType == 4 && bfTpl->bKind == 5) {
                                sMugongList* bfData = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                                if (bfData && bfData->nEtc1 > 0) {
                                    rawDmg = rawDmg * bfData->nEtc1 / 100;
                                }
                                break;
                            }
                        }
                    }

                    

                    finalDmg = (DWORD)rawDmg;

                    // 武功PvP命中蓄力与受创蓄力
                    if (pAttacker && pAttacker->bCurFiveElm > 0 && (dwMugongID < 150 || dwMugongID > 154) && p[0] == 2) {
                        pAttacker->dwFiveElmGauge += 5;
                        if (pAttacker->dwFiveElmGauge > 5000) pAttacker->dwFiveElmGauge = 5000;
                        SyncFiveElmStatus(clientSocket, pAttacker->dwObjectID - 400000000, pAttacker);
                    }
                    if (pTarget && pTarget->bCurFiveElm > 0 && p[0] == 2) {
                        pTarget->dwFiveElmGauge += 10;
                        if (pTarget->dwFiveElmGauge > 5000) pTarget->dwFiveElmGauge = 5000;
                        SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(pTarget->dwObjectID - 400000000);
                        if (targetSock != INVALID_SOCKET) {
                            SyncFiveElmStatus(targetSock, pTarget->dwObjectID - 400000000, pTarget);
                        }
                    }

                    

                    if (pTarget->dwHpCur > finalDmg) {

                        pTarget->dwHpCur -= finalDmg;

                    } else {

                        finalDmg = pTarget->dwHpCur;

                        pTarget->dwHpCur = 0;

                        isDead = true;

                        pTarget->dwDeadTime = GetTickCount();

                        LOG("[MugongHandler] PvP Player " + pTarget->szName + " died from skill " + std::to_string(dwMugongID));

                    }

                }

                

                // Debuff  (���訨����)

                if (pMugongData && pMugongData->dwKeepUpTime > 0 && p[0] == 2) {

                    sServerObject::sActiveBuff debuff;

                    debuff.dwMugongID = dwMugongID;

                    debuff.bLevel = bMugongLevel;

                    debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;

                    debuff.bIsDebuff = true;

                    pTarget->activeBuffs[dwMugongID] = debuff;

                    LOG("[MugongHandler] PvP Debuff Applied skill " + std::to_string(dwMugongID) + " to target player " + std::to_string(dwDefenseID));

                }



                pvp_aoe_only:

                // AOE / Splash Damage Logic (Class 6 skills) - PVP Target AOE (�� Debuff )

                sMugongTemplate* sTpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);

                if (IsAoeSkill(dwMugongID, sTpl)) {

                    // ������������������PC/����

                    WORD wAoeCenterX = pAttacker ? pAttacker->wPosX : wAttackPosX;

                    WORD wAoeCenterY = pAttacker ? pAttacker->wPosY : wAttackPosY;

                    float splashRange = (pMugongData && pMugongData->wAttackRange > 0) ? (float)pMugongData->wAttackRange : 20.0f;

                    

                    LOG("[MugongHandler] [AOE-PvP] Skill ID: " + std::to_string(dwMugongID) + " Attacker OID: " + std::to_string(dwAttackID) + " Pos: (" + std::to_string(wAoeCenterX) + "," + std::to_string(wAoeCenterY) + ")");



                    // 1. ��

                    std::vector<MonsterData*> aoiMonsters = mapInst->GetMonstersInAOI(wAoeCenterX, wAoeCenterY);

                    std::vector<MonsterData*> splashMonsters;

                    

                    LOG("[MugongHandler] [AOE-PvP] Scanned " + std::to_string(aoiMonsters.size()) + " potential monsters in AOI");

                    for (MonsterData* pMon : aoiMonsters) {

                        if (!pMon || pMon->dwObjectID == dwDefenseID || pMon->dwHpCur == 0 || pMon->bIsReturning) continue;

                        float dx = (float)pMon->wPosX - (float)wAoeCenterX;

                        float dy = (float)pMon->wPosY - (float)wAoeCenterY;

                        float dist = sqrtf(dx * dx + dy * dy);

                        

                        LOG("[MugongHandler] [AOE-PvP] -> Mon OID: " + std::to_string(pMon->dwObjectID) + " Pos: (" + std::to_string(pMon->wPosX) + "," + std::to_string(pMon->wPosY) + ") Dist: " + std::to_string(dist));

                        if (dist <= splashRange) {

                            splashMonsters.push_back(pMon);

                            if (splashMonsters.size() >= 5) break;

                        }

                    }

                    LOG("[MugongHandler] [AOE-PvP] -> Splash monsters count selected: " + std::to_string(splashMonsters.size()));



                    for (MonsterData* pSplashMon : splashMonsters) {

                        DWORD sMonDef = g_NpcTemplates.count(pSplashMon->bPropType) ? g_NpcTemplates[pSplashMon->bPropType].dwDefInit : 0;

                        

                        // ���� Debuff ��

                        for (auto& bf : pSplashMon->activeBuffs) {

                            sMugongList* bd = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);

                            if (bd) {

                                if (bd->wIncDefPerc > 0) sMonDef = sMonDef * bd->wIncDefPerc / 100;

                                if (bd->wIncDef > 0) {

                                    if (sMonDef > bd->wIncDef) sMonDef -= bd->wIncDef; else sMonDef = 0;

                                }

                            }

                        }



                        WORD sMonAvoid = pSplashMon->wAvoidRatio;

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

                            // 溅射伤害公式：与主目标一致，按 bKind 路由
                            float rawDmg;
                            if (tpl && tpl->bType == 3 && (tpl->bKind == 16 || tpl->bKind == 21 || tpl->bKind == 24)) {
                                rawDmg = (float)pAtk + (float)skillFlatDmg;
                            } else if (tpl && tpl->bType == 3 && tpl->bKind == 17 && pAttacker) {
                                DWORD wAtk = pAttacker->wWepAtk;
                                DWORD oAtk = (pAtk > wAtk) ? (pAtk - wAtk) : 0;
                                rawDmg = (float)(wAtk * ((100.0f + skillPerc) / 100.0f)) + (float)oAtk + (float)skillFlatDmg;
                            } else if (tpl && tpl->bType == 3 && tpl->bKind == 19 && pAttacker) {
                                rawDmg = (float)pAttacker->wWepAtk * (float)skillPerc / 100.0f;
                            } else if (tpl && tpl->bType == 3 && (tpl->bKind == 0 || tpl->bKind == 23)) {
                                rawDmg = (float)pAtk * (float)skillPerc / 100.0f;
                            } else {
                                rawDmg = (pAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg;
                            }

                            float var = 0.9f + ((float)(rand() % 2000) / 10000.0f);

                            rawDmg *= var;

                            

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

                                

                                DWORD splashFiveElmExp2 = g_NpcTemplates.count(pSplashMon->bPropType) ? g_NpcTemplates[pSplashMon->bPropType].wFiveElmExp : 0;
                                splashFiveElmExp2 += pSplashMon->wIncFiveElmExp;

                                deadEntities.push_back({pSplashMon->dwObjectID, pSplashMon->dwExp, g_NpcTemplates[pSplashMon->bPropType].szName, splashFiveElmExp2});

                                DropManager::GetInstance()->GenerateDrops(dwAttackID, *pSplashMon);

                                LOG("[MugongHandler] Splash target " + g_NpcTemplates[pSplashMon->bPropType].szName + " died from skill " + std::to_string(dwMugongID) + "!");

                            }

                        }



                        // Debuff ������������

                        if (pMugongData && pMugongData->dwKeepUpTime > 0 && sResult == 2) {

                            PlayerData::sActiveBuff debuff;

                            debuff.dwMugongID = dwMugongID;

                            debuff.bLevel = bMugongLevel;

                            debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;

                            debuff.bIsDebuff = true;

                            pSplashMon->activeBuffs[dwMugongID] = debuff;

                            LOG("[MugongHandler] Applied splash debuff skill " + std::to_string(dwMugongID) + " to monster " + std::to_string(pSplashMon->dwObjectID));

                        }

                        

                        if (pSplashMon->dwAttackPattern != 0 && pSplashMon->dwHpCur > 0) {

                            pSplashMon->dwTargetID = dwAttackID;

                        }

                        

                        //  0x4016 �衧

                        std::vector<BYTE> splashAckBuf(4 + 38);

                        BYTE* sp = splashAckBuf.data() + 4;

                        sp[0] = sResult; 

                        *(DWORD*)(sp + 1) = dwMugongID;

                        sp[5] = bMugongLevel;

                        sp[6] = 1; 

                        *(DWORD*)(sp + 7) = dwAttackID;

                        *(WORD*)(sp + 11) = wAttackPosX;

                        *(WORD*)(sp + 13) = wAttackPosY;

                        sp[15] = bAttackHeight;

                        sp[16] = 3; 

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

                        deferredSplashBroadcasts.push_back(splashAckBuf); // [修复死锁] 延迟广播，避免在 mapMutex 内获取 SessionMgr::m_mutex

                    }



                    // 2. �� PC  (PVP)
                    // 2. AOE PC 溅射 (PVP) — 仅在攻击者处于自由PK模式(bSafeMode==2)时才扫描玩家
                    // bSafeMode: 0=保护所有角色, 1=保护本门派, 2=自由PK
                    if (pAttacker && pAttacker->bSafeMode == 2) {
                        std::vector<PlayerData*> aoiPlayers = mapInst->GetPlayersInAOI(wAoeCenterX, wAoeCenterY);
                        std::vector<PlayerData*> splashPlayers;
                        LOG("[MugongHandler] [AOE-PvP] Scanned " + std::to_string(aoiPlayers.size()) + " potential players in AOI");
                        for (PlayerData* pPl : aoiPlayers) {
                            if (!pPl || pPl->dwObjectID == dwAttackID || pPl->dwObjectID == dwDefenseID || pPl->dwHpCur == 0 || pPl->dwInvulnerableUntil > GetTickCount()) continue;
                            // 组队保护：同队玩家不受AOE伤害
                            DWORD pID1 = PartyManager::GetInstance().GetPartyID(dwAttackID - 400000000);
                            DWORD pID2 = PartyManager::GetInstance().GetPartyID(pPl->dwObjectID - 400000000);
                            if (pID1 != 0 && pID1 == pID2) continue;
                            float dx = (float)pPl->wPosX - (float)wAoeCenterX;
                            float dy = (float)pPl->wPosY - (float)wAoeCenterY;
                            float dist = sqrtf(dx * dx + dy * dy);
                            if (dist <= splashRange) {
                                splashPlayers.push_back(pPl);
                                if (splashPlayers.size() >= 5) break;
                            }
                        }
                        LOG("[MugongHandler] [AOE-PvP] -> Splash players: " + std::to_string(splashPlayers.size()));
                        for (PlayerData* pSplashPlayer : splashPlayers) {
                            DWORD sPlDef = pSplashPlayer->dwTotalDef;
                            WORD sPlDodge = pSplashPlayer->dwTotalDodge;
                            DWORD sPlayerAtkRating = 50 + (pAttacker ? pAttacker->dwTotalHit : 0);
                            float sHitChance = (float)sPlayerAtkRating / (float)(sPlayerAtkRating + sPlDodge);
                            float sRoll = (float)(rand() % 10000) / 10000.0f;
                            BYTE sCritHit = 0; DWORD sFinalDmg = 0; BYTE sResult = 2;
                            if (sPlDodge > 0 && sRoll > sHitChance) { sResult = 1; sFinalDmg = 0; }
                            else {
                                DWORD pAtk = pAttacker ? pAttacker->dwTotalAtk : 50;
                                DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0;
                                DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0;
                                float rawDmg = ((pAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg) * 0.7f;
                                float var = 0.9f + ((float)(rand() % 2000) / 10000.0f); rawDmg *= var;
                                WORD critRate = (pAttacker && pAttacker->wCritical > 0) ? pAttacker->wCritical : 5;
                                if ((WORD)(rand() % 100) < critRate) { rawDmg *= 1.5f; sCritHit = 1; }
                                if (rawDmg > sPlDef) rawDmg -= sPlDef; else rawDmg = 1;
                                sFinalDmg = (DWORD)rawDmg;
                                if (pSplashPlayer->dwHpCur > sFinalDmg) { pSplashPlayer->dwHpCur -= sFinalDmg; }
                                else { sFinalDmg = pSplashPlayer->dwHpCur; pSplashPlayer->dwHpCur = 0; pSplashPlayer->dwDeadTime = GetTickCount(); }
                            }
                            if (pMugongData && pMugongData->dwKeepUpTime > 0 && sResult == 2) {
                                sServerObject::sActiveBuff debuff;
                                debuff.dwMugongID = dwMugongID; debuff.bLevel = bMugongLevel;
                                debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000; debuff.bIsDebuff = true;
                                pSplashPlayer->activeBuffs[dwMugongID] = debuff;
                            }
                            std::vector<BYTE> splashAckBuf(4 + 38);
                            BYTE* sp = splashAckBuf.data() + 4;
                            sp[0] = sResult; *(DWORD*)(sp + 1) = dwMugongID; sp[5] = bMugongLevel; sp[6] = 1;
                            *(DWORD*)(sp + 7) = dwAttackID; *(WORD*)(sp + 11) = wAttackPosX; *(WORD*)(sp + 13) = wAttackPosY; sp[15] = bAttackHeight;
                            sp[16] = 1; *(DWORD*)(sp + 17) = pSplashPlayer->dwObjectID;
                            *(DWORD*)(sp + 21) = pSplashPlayer->dwHpMax; *(DWORD*)(sp + 25) = pSplashPlayer->dwHpCur;
                            *(DWORD*)(sp + 29) = sFinalDmg; *(DWORD*)(sp + 33) = 0; sp[37] = sCritHit;
                            PACKET_HEADER* sHead = (PACKET_HEADER*)splashAckBuf.data();
                            sHead->id = 0x4016; sHead->payloadSize = 38;
                            EncryptPacket(splashAckBuf.data(), 0x42);
                            deferredSplashBroadcasts.push_back(splashAckBuf); // [修复死锁] 延迟广播
                            splashPlayerSnapshots.push_back({pSplashPlayer->dwObjectID - 400000000, pSplashPlayer->dwHpMax, pSplashPlayer->dwHpCur, pSplashPlayer->wIpMax, pSplashPlayer->wIpCur});
                        }
                    }

                }



                dwDefHpMax = pTarget->dwHpMax;

                dwDefHpCur = pTarget->dwHpCur;

                dwDefIpMax = pTarget->wIpMax;

                dwDefIpCur = pTarget->wIpCur;

            }

        }

    }
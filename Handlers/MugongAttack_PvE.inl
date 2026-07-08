    if (!isBuff && dwMugongID != 64 && bDefenseType == 3) {

        if (g_MapInstances.count(playerMapID)) {

            CMapInstance* mapInst = g_MapInstances[playerMapID];

            std::lock_guard<std::mutex> lock(mapInst->GetMutex());

            MonsterData* pTarget = mapInst->GetMonster(dwDefenseID);

            if (pTarget && pTarget->dwHpCur > 0) {

                bool sIsReturning = pTarget->bIsReturning;

                // Use template defense since MonsterData doesn't store wWepDef

                DWORD monsterDef = g_NpcTemplates.count(pTarget->bPropType) ? g_NpcTemplates[pTarget->bPropType].dwDefInit : 0;

                

                // �����㨦 Debuff ����

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



                PlayerData* pAttacker = mapInst->GetPlayer(dwAttackID);

                

                // 以血换血技能（bType=3, bKind=18，如玉石俱焚）

                DWORD hpSacrificed = 0;

                bool isSacrifice = (tpl && tpl->bType == 3 && tpl->bKind == 18);

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

                        push4(pAttacker->wIpMax);

                        push4(pAttacker->wIpCur);

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
                
                // 降命中 Debuff 折损攻击方命中率
                if (pAttacker) {
                    for (const auto& bf : pAttacker->activeBuffs) {
                        sMugongTemplate* debuffTpl = MugongManager::GetInstance()->GetTemplate(bf.second.dwMugongID);
                        if (debuffTpl && debuffTpl->bType == 4 && debuffTpl->bKind == 20) {
                            sMugongList* debuffData = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                            if (debuffData && debuffData->wIncRatePerc > 0 && debuffData->wIncRatePerc < 100) {
                                hitChance *= (float)debuffData->wIncRatePerc / 100.0f;
                            }
                            break;
                        }
                    }
                }
                float dodgeRoll = (float)(rand() % 10000) / 10000.0f;

                

                if (sIsReturning || (monsterAvoid > 0 && dodgeRoll > hitChance)) {

                    p[0] = 1; // 1 = MISS

                    finalDmg = 0;

                } else {

                    p[0] = 2; // 2 = HIT

                    // 致盲受击清醒：命中怪物时，移除 bKind=16 致盲 debuff
                    for (auto bIt = pTarget->activeBuffs.begin(); bIt != pTarget->activeBuffs.end(); ) {
                        sMugongTemplate* bTpl = MugongManager::GetInstance()->GetTemplate(bIt->second.dwMugongID);
                        if (bTpl && bTpl->bType == 4 && bTpl->bKind == 16 && bIt->second.bIsDebuff) {
                            LOG("[PvE] Blind removed from monster " + std::to_string(pTarget->dwObjectID) + " by attack hit");
                            bIt = pTarget->activeBuffs.erase(bIt);
                        } else {
                            ++bIt;
                        }
                    }


                    DWORD playerAtk = 50; // Default fallback

                    if (pAttacker) {

                        playerAtk = pAttacker->dwTotalAtk;

                    }



                    DWORD skillFlatDmg = pMugongData ? pMugongData->dwDamageMul : 0; // Loaded from wIncAtk

                    DWORD skillPerc = pMugongData ? pMugongData->wIncAtkPerc : 0; 


                    float rawDmg;
                    // bType=3, bKind=16/21/24（固定加伤）：
                    // 公式 = 总攻击力 + 技能固定攻击力（不乘倍率）
                    if (tpl && tpl->bType == 3 && (tpl->bKind == 16 || tpl->bKind == 21 || tpl->bKind == 24)) {
                        rawDmg = (float)playerAtk + (float)skillFlatDmg;
                    }
                    // bType=3, bKind=17（远程投射攻击，如真武剑诀）：
                    // 公式 = (武器攻击力 × 倍率) + (其余攻击力总和) + 技能固定伤害
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
                    // bType=3, bKind=19（御剑术）：消耗武器耐久增幅攻击
                    // 公式 = 武器攻击力 × wIncAtkPerc%
                    else if (tpl && tpl->bType == 3 && tpl->bKind == 19 && pAttacker) {
                        rawDmg = (float)pAttacker->wWepAtk * (float)skillPerc / 100.0f;
                    }
                    // bType=3, bKind=0/23（群体攻击/一击残）：总攻击力 × 倍率
                    // wIncAtkPerc 即为实际百分比（120=120%=1.2倍）
                    else if (tpl && tpl->bType == 3 && (tpl->bKind == 0 || tpl->bKind == 23)) {
                        rawDmg = (float)playerAtk * (float)skillPerc / 100.0f;
                    } else {
                        // 通用公式：总攻击力 × 倍率 + 固定伤害
                        rawDmg = (playerAtk * ((100.0f + skillPerc) / 100.0f)) + skillFlatDmg;
                    }

                    

                    // 非 bKind=18 的牺牲技能（如有其他扩展）保留旧的加伤逻辑

                    if (hpSacrificed > 0 && !(tpl && tpl->bType == 3 && tpl->bKind == 18)) {

                        rawDmg += hpSacrificed * 2.0f;

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

                    

                    DWORD elemDmg = 0;
                    if (pAttacker && pAttacker->bCurFiveElm > 0) {
                        DWORD baseFEValue = 0;
                        switch (pAttacker->bCurFiveElm) {
                            case 1: baseFEValue = pAttacker->wFireExp; break;
                            case 2: baseFEValue = pAttacker->wWaterExp; break;
                            case 3: baseFEValue = pAttacker->wWoodExp; break;
                            case 4: baseFEValue = pAttacker->wMetalExp; break;
                            case 5: baseFEValue = pAttacker->wEarthExp; break;
                        }
                        BYTE attLvl = pAttacker->GetFiveElmPassiveLevel();
                        BYTE defLvl = 0; // 怪物无被动等级
                        float fCounter = CalculateFiveElmCounter(pAttacker->bCurFiveElm, pTarget->bFiveElm, attLvl, defLvl, pAttacker->bIgnoreFiveElm);
                        
                        WORD monsterFEExp = pTarget->wIncFiveElmExp;
                        if (monsterFEExp == 0 && g_NpcTemplates.count(pTarget->bPropType)) {
                            monsterFEExp = g_NpcTemplates[pTarget->bPropType].wFiveElmExp;
                        }
                        elemDmg = CalculateFiveElmDamage(baseFEValue, attLvl, fCounter, pTarget->wLevel, monsterFEExp, pAttacker->bIgnoreDefFiveElm);
                    }
                    finalDmg = (DWORD)rawDmg + elemDmg;

                    // 武功命中蓄力（普通武功技能命中怪物）：施法者处于五行激活状态
                    if (pAttacker && pAttacker->bCurFiveElm > 0 && (dwMugongID < 150 || dwMugongID > 154)) {
                        pAttacker->dwFiveElmGauge += 5;
                        if (pAttacker->dwFiveElmGauge > 5000) pAttacker->dwFiveElmGauge = 5000;
                        SyncFiveElmStatus(clientSocket, pAttacker->dwObjectID - 400000000, pAttacker);
                    }

                    

                    if (pTarget->dwHpCur > finalDmg) {

                        pTarget->dwHpCur -= finalDmg;

                    } else {

                        finalDmg = pTarget->dwHpCur;

                        pTarget->dwHpCur = 0;

                        isDead = true;

                        pTarget->dwDeadTime = GetTickCount();

                        pTarget->dwTargetID = 0;

                        deadExp = pTarget->dwExp; // Use computed Init+Inc value

                        DWORD targetFiveElmExp = g_NpcTemplates.count(pTarget->bPropType) ? g_NpcTemplates[pTarget->bPropType].wFiveElmExp : 0;
                        targetFiveElmExp += pTarget->wIncFiveElmExp;

                        deadEntities.push_back({dwDefenseID, deadExp, g_NpcTemplates[pTarget->bPropType].szName, targetFiveElmExp});

                        // 妙手空空(bKind=23)：击杀时暴率翻倍
                        WORD origRootItem = pTarget->wRootItem;
                        if (tpl && tpl->bType == 4 && tpl->bKind == 23 && pMugongData) {
                            WORD successRate = pMugongData->wSuccessRatePerc;
                            bool luckyKill = (successRate >= 100) || ((WORD)(rand() % 100) < successRate);
                            if (luckyKill && pTarget->wRootItem > 1) {
                                pTarget->wRootItem = pTarget->wRootItem / 2;
                                if (pTarget->wRootItem < 1) pTarget->wRootItem = 1;
                                LOG("[MugongHandler] MiaoShouKongKong: drop rate doubled! wRootItem " + std::to_string(origRootItem) + " -> " + std::to_string(pTarget->wRootItem));
                            }
                        }
                        DropManager::GetInstance()->GenerateDrops(dwAttackID, *pTarget);
                        pTarget->wRootItem = origRootItem; // 恢复原值（虽然怪物已死但保持数据一致性）

                        LOG("[MugongHandler] Monster " + g_NpcTemplates[pTarget->bPropType].szName + " died from skill " + std::to_string(dwMugongID) + "!");

                    }

                }

                

                // Debuff  (���訨����)

                if (pMugongData && pMugongData->dwKeepUpTime > 0 && p[0] == 2) {

                    PlayerData::sActiveBuff debuff;

                    debuff.dwMugongID = dwMugongID;

                    debuff.bLevel = bMugongLevel;

                    debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;

                    debuff.bIsDebuff = true;

                    pTarget->activeBuffs[dwMugongID] = debuff;

                    LOG("[MugongHandler] Applied debuff skill " + std::to_string(dwMugongID) + " to monster " + std::to_string(dwDefenseID));

                    // 广播 0x402C 状态同步包给客户端以挂载 Debuff 图标
                    std::vector<BYTE> buffAck(4 + 11);
                    BYTE* bp = buffAck.data() + 4;
                    bp[0] = 0; // bResult
                    *(DWORD*)(bp + 1) = dwDefenseID;
                    bp[5] = pTarget->bObjectType; // 3 = Monster/NPC
                    *(DWORD*)(bp + 6) = dwMugongID;
                    bp[10] = bMugongLevel;
                    PACKET_HEADER* headB = (PACKET_HEADER*)buffAck.data();
                    headB->id = 0x402C; // CS_BT_KEEPUPMUGONGSTART_ACK
                    headB->payloadSize = 11;
                    EncryptPacket(buffAck.data(), 0x42);
                    mapInst->BroadcastPacket(buffAck);
                }

                

                // AOE / Splash Damage Logic (Class 6 skills) - �� Debuff 

                sMugongTemplate* sTpl = MugongManager::GetInstance()->GetTemplate(dwMugongID);

                if (IsAoeSkill(dwMugongID, sTpl)) {

                    // �����������������訨��

                    WORD wAoeCenterX = pAttacker ? pAttacker->wPosX : wAttackPosX;

                    WORD wAoeCenterY = pAttacker ? pAttacker->wPosY : wAttackPosY;

                    float splashRange = (pMugongData && pMugongData->wAttackRange > 0) ? (float)pMugongData->wAttackRange : 20.0f;

                    

                    LOG("[MugongHandler] [AOE-Monster] Skill ID: " + std::to_string(dwMugongID) + " Attacker OID: " + std::to_string(dwAttackID) + " Pos: (" + std::to_string(wAoeCenterX) + "," + std::to_string(wAoeCenterY) + ")");



                    // 1. ��

                    std::vector<MonsterData*> aoiMonsters = mapInst->GetMonstersInAOI(wAoeCenterX, wAoeCenterY);

                    std::vector<MonsterData*> splashMonsters;

                    

                    LOG("[MugongHandler] [AOE-Monster] Scanned " + std::to_string(aoiMonsters.size()) + " potential monsters in AOI");

                    for (MonsterData* pMon : aoiMonsters) {

                        if (!pMon || pMon->dwObjectID == dwDefenseID || pMon->dwHpCur == 0 || pMon->bIsReturning) continue;

                        float dx = (float)pMon->wPosX - (float)wAoeCenterX;

                        float dy = (float)pMon->wPosY - (float)wAoeCenterY;

                        float dist = sqrtf(dx * dx + dy * dy);

                        

                        LOG("[MugongHandler] [AOE-Monster] -> Mon OID: " + std::to_string(pMon->dwObjectID) + " Pos: (" + std::to_string(pMon->wPosX) + "," + std::to_string(pMon->wPosY) + ") Dist: " + std::to_string(dist));

                        if (dist <= splashRange) {

                            splashMonsters.push_back(pMon);

                            if (splashMonsters.size() >= 5) break; // ���5

                        }

                    }

                    LOG("[MugongHandler] [AOE-Monster] -> Splash monsters count selected: " + std::to_string(splashMonsters.size()));



                    for (MonsterData* pSplashMon : splashMonsters) {

                        DWORD sMonDef = g_NpcTemplates.count(pSplashMon->bPropType) ? g_NpcTemplates[pSplashMon->bPropType].dwDefInit : 0;

                        

                        // ���������� Debuff 

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
                        
                        // 降命中 Debuff 折损攻击方命中率
                        if (pAttacker) {
                            for (const auto& bf : pAttacker->activeBuffs) {
                                sMugongTemplate* debuffTpl = MugongManager::GetInstance()->GetTemplate(bf.second.dwMugongID);
                                if (debuffTpl && debuffTpl->bType == 4 && debuffTpl->bKind == 20) {
                                    sMugongList* debuffData = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                                    if (debuffData && debuffData->wIncRatePerc > 0 && debuffData->wIncRatePerc < 100) {
                                        sHitChance *= (float)debuffData->wIncRatePerc / 100.0f;
                                    }
                                    break;
                                }
                            }
                        }
                        float sRoll = (float)(rand() % 10000) / 10000.0f;

                        

                        BYTE sCritHit = 0;

                        DWORD sFinalDmg = 0;

                        BYTE sResult = 2; // HIT

                        

                        if (sMonAvoid > 0 && sRoll > sHitChance) {

                            sResult = 1; // MISS

                            sFinalDmg = 0;

                        } else {

                            // AOE致盲受击清醒
                            for (auto bIt = pSplashMon->activeBuffs.begin(); bIt != pSplashMon->activeBuffs.end(); ) {
                                sMugongTemplate* bTpl = MugongManager::GetInstance()->GetTemplate(bIt->second.dwMugongID);
                                if (bTpl && bTpl->bType == 4 && bTpl->bKind == 16 && bIt->second.bIsDebuff) {
                                    bIt = pSplashMon->activeBuffs.erase(bIt);
                                } else { ++bIt; }
                            }

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

                            

                            DWORD sElemDmg = 0;
                            if (pAttacker && pAttacker->bCurFiveElm > 0) {
                                DWORD baseFEValue = 0;
                                switch (pAttacker->bCurFiveElm) {
                                    case 1: baseFEValue = pAttacker->wFireExp; break;
                                    case 2: baseFEValue = pAttacker->wWaterExp; break;
                                    case 3: baseFEValue = pAttacker->wWoodExp; break;
                                    case 4: baseFEValue = pAttacker->wMetalExp; break;
                                    case 5: baseFEValue = pAttacker->wEarthExp; break;
                                }
                                BYTE attLvl = pAttacker->GetFiveElmPassiveLevel();
                                BYTE defLvl = 0; // 怪物无被动等级
                                float fCounter = CalculateFiveElmCounter(pAttacker->bCurFiveElm, pSplashMon->bFiveElm, attLvl, defLvl, pAttacker->bIgnoreFiveElm);
                                
                                WORD monsterFEExp = pSplashMon->wIncFiveElmExp;
                                if (monsterFEExp == 0 && g_NpcTemplates.count(pSplashMon->bPropType)) {
                                    monsterFEExp = g_NpcTemplates[pSplashMon->bPropType].wFiveElmExp;
                                }
                                sElemDmg = CalculateFiveElmDamage(baseFEValue, attLvl, fCounter, pSplashMon->wLevel, monsterFEExp, pAttacker->bIgnoreDefFiveElm);
                            }
                            sFinalDmg = (DWORD)rawDmg + sElemDmg;

                            

                            if (pSplashMon->dwHpCur > sFinalDmg) {

                                pSplashMon->dwHpCur -= sFinalDmg;

                            } else {

                                sFinalDmg = pSplashMon->dwHpCur;

                                pSplashMon->dwHpCur = 0;

                                pSplashMon->dwDeadTime = GetTickCount();

                                pSplashMon->dwTargetID = 0;

                                

                                DWORD splashFiveElmExp = g_NpcTemplates.count(pSplashMon->bPropType) ? g_NpcTemplates[pSplashMon->bPropType].wFiveElmExp : 0;
                                splashFiveElmExp += pSplashMon->wIncFiveElmExp;

                                deadEntities.push_back({pSplashMon->dwObjectID, pSplashMon->dwExp, g_NpcTemplates[pSplashMon->bPropType].szName, splashFiveElmExp});

                                DropManager::GetInstance()->GenerateDrops(dwAttackID, *pSplashMon);

                                LOG("[MugongHandler] Splash target " + g_NpcTemplates[pSplashMon->bPropType].szName + " died from skill " + std::to_string(dwMugongID) + "!");

                            }

                        }



                        // Debuff �����������

                        if (pMugongData && pMugongData->dwKeepUpTime > 0 && sResult == 2) {

                            PlayerData::sActiveBuff debuff;

                            debuff.dwMugongID = dwMugongID;

                            debuff.bLevel = bMugongLevel;

                            debuff.dwEndTime = GetTickCount() + pMugongData->dwKeepUpTime * 1000;

                            debuff.bIsDebuff = true;

                            pSplashMon->activeBuffs[dwMugongID] = debuff;

                            LOG("[MugongHandler] Applied splash debuff skill " + std::to_string(dwMugongID) + " to monster " + std::to_string(pSplashMon->dwObjectID));

                            // 广播 0x402C 状态同步包给客户端以挂载溅射 Debuff 图标
                            std::vector<BYTE> buffAck(4 + 11);
                            BYTE* bp = buffAck.data() + 4;
                            bp[0] = 0; // bResult
                            *(DWORD*)(bp + 1) = pSplashMon->dwObjectID;
                            bp[5] = pSplashMon->bObjectType; // 3 = Monster/NPC
                            *(DWORD*)(bp + 6) = dwMugongID;
                            bp[10] = bMugongLevel;
                            PACKET_HEADER* headB = (PACKET_HEADER*)buffAck.data();
                            headB->id = 0x402C; // CS_BT_KEEPUPMUGONGSTART_ACK
                            headB->payloadSize = 11;
                            EncryptPacket(buffAck.data(), 0x42);
                            mapInst->BroadcastPacket(buffAck);
                        }

                        

                        if (pSplashMon->dwAttackPattern != 0 && pSplashMon->dwHpCur > 0) {

                            pSplashMon->dwTargetID = dwAttackID;

                        }

                        

                        // ���� 0x4016 �衧

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
                        LOG("[MugongHandler] [AOE-Monster] Scanned " + std::to_string(aoiPlayers.size()) + " potential players in AOI");
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
                        LOG("[MugongHandler] [AOE-Monster] -> Splash players: " + std::to_string(splashPlayers.size()));
                        for (PlayerData* pSplashPlayer : splashPlayers) {
                            DWORD sPlDef = pSplashPlayer->dwTotalDef;
                            WORD sPlDodge = pSplashPlayer->dwTotalDodge;
                            DWORD sPlayerAtkRating = 50 + (pAttacker ? pAttacker->dwTotalHit : 0);
                            float sHitChance = (float)sPlayerAtkRating / (float)(sPlayerAtkRating + sPlDodge);
                            
                            // 降命中 Debuff 折损攻击方命中率
                            if (pAttacker) {
                                for (const auto& bf : pAttacker->activeBuffs) {
                                    sMugongTemplate* debuffTpl = MugongManager::GetInstance()->GetTemplate(bf.second.dwMugongID);
                                    if (debuffTpl && debuffTpl->bType == 4 && debuffTpl->bKind == 20) {
                                        sMugongList* debuffData = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                                        if (debuffData && debuffData->wIncRatePerc > 0 && debuffData->wIncRatePerc < 100) {
                                            sHitChance *= (float)debuffData->wIncRatePerc / 100.0f;
                                        }
                                        break;
                                    }
                                }
                            }
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
                                DWORD sElemDmg = 0;
                                if (pAttacker && pAttacker->bCurFiveElm > 0) {
                                    DWORD baseFEValue = 0;
                                    switch (pAttacker->bCurFiveElm) {
                                        case 1: baseFEValue = pAttacker->wFireExp; break;
                                        case 2: baseFEValue = pAttacker->wWaterExp; break;
                                        case 3: baseFEValue = pAttacker->wWoodExp; break;
                                        case 4: baseFEValue = pAttacker->wMetalExp; break;
                                        case 5: baseFEValue = pAttacker->wEarthExp; break;
                                    }
                                    BYTE attLvl = pAttacker->GetFiveElmPassiveLevel();
                                    BYTE defLvl = pSplashPlayer->GetFiveElmPassiveLevel();
                                    float fCounter = CalculateFiveElmCounter(pAttacker->bCurFiveElm, pSplashPlayer->bCurFiveElm, attLvl, defLvl, pAttacker->bIgnoreFiveElm);
                                    WORD playerDefExp = 0;
                                    switch (pAttacker->bCurFiveElm) {
                                        case 1: playerDefExp = pSplashPlayer->wFireExp; break;
                                        case 2: playerDefExp = pSplashPlayer->wWaterExp; break;
                                        case 3: playerDefExp = pSplashPlayer->wWoodExp; break;
                                        case 4: playerDefExp = pSplashPlayer->wMetalExp; break;
                                        case 5: playerDefExp = pSplashPlayer->wEarthExp; break;
                                    }
                                    sElemDmg = CalculateFiveElmDamage(baseFEValue, attLvl, fCounter, 0, playerDefExp, pAttacker->bIgnoreDefFiveElm);
                                }
                                sFinalDmg = (DWORD)rawDmg + sElemDmg;
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

                

                if (pTarget->dwAttackPattern != 0 && pTarget->dwHpCur > 0 && !pTarget->bIsReturning) {

                    pTarget->dwTargetID = dwAttackID;

                }

                dwDefHpMax = pTarget->dwHpMax;

                dwDefHpCur = pTarget->dwHpCur;

            }

        }

    }
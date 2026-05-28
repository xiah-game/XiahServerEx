#include "PlayerManager.h"
#include "MugongManager.h"
#include "../DBHelper.h"
#include "../DB/CharacterDB.h"
#include "../DB/ItemDB.h"
#include "../../XiahClient/csprotocol.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../Handlers/MugongHandler.h"

extern std::map<DWORD, CMapInstance*> g_MapInstances;

void PlayerManager::RecalculateStats(DWORD dwCharID, bool sendPacket) {
    DWORD dwObjectID = (dwCharID < 800000000) ? (dwCharID + 400000000) : dwCharID;

    // 1. Fetch base attributes from DB
    int baseStr = 10, baseDex = 10, baseVit = 10, baseInt = 10, baseLevel = 1;
    CharacterDB::CharPower cpPM;
    if (CharacterDB::GetInstance().GetCharData(dwCharID, cpPM)) {
        baseStr = cpPM.wStr; baseDex = cpPM.wDex; baseVit = cpPM.wVit;
        baseInt = cpPM.wSus; baseLevel = cpPM.wLevel;
    }


    // 2. Fetch equipment stats from DB
    int equipAtk=0, equipDef=0, equipMag=0, equipSpd=0, equipAtkSpd=0, equipCrit=0, equipHp=0, equipIp=0, equipRestoreHp=0, equipRestoreIp=0;
    std::vector<ItemDB::EquipStatRow> equipRows;
    ItemDB::GetInstance().GetEquippedItemStats(dwCharID, equipRows);
    for (auto& row : equipRows) {
        int d4=row.d4, d5=row.d5, d6=row.d6, d7=row.d7, d13=row.d13, d9=row.d9, d10=row.d10, d11=row.d11, d12=row.d12;
        WORD ref = row.wRefID;
        if (d4 == -9999) d4 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData4 : 0);
        if (d5 == -9999) d5 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData5 : 0);
        if (d6 == -9999) d6 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData6 : 0);
        if (d7 == -9999) d7 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData7 : 0);
        if (d13 == -9999) d13 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData13 : 0);
        if (d9 == -9999) d9 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData9 : 0);
        if (d10 == -9999) d10 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData10 : 0);
        if (d11 == -9999) d11 = 0;
        if (d12 == -9999) d12 = 0;
        equipAtk += d4; equipDef += d5; equipMag += d6; equipCrit += d13;
        if (g_ItemTemplates.count(ref) && g_ItemTemplates[ref].bType == 4) {
            equipSpd += d7;
        } else {
            equipAtkSpd += d7;
        }
        equipHp += d9; equipIp += d10; equipRestoreHp += d11; equipRestoreIp += d12;
        LOG("[RecalcStats] EquipRow ref=" + std::to_string(ref) + " d7(spd)=" + std::to_string(d7) + " d9(hp)=" + std::to_string(d9) + " d10(ip)=" + std::to_string(d10) + " d11(restHp)=" + std::to_string(d11) + " d12(restIp)=" + std::to_string(d12) + " d13(crit)=" + std::to_string(d13));
    }

    // 动态拉取夺命符在数据库 nData22 配置的暴击值 (若为 0 且检测到装备则兼容性降级为默认的 +20 暴击加成)
    int critBonus = ItemDB::GetInstance().GetEquippedItemDataValue(dwCharID, 9, 22); // 9 = 夺命符
    if (critBonus <= 0) {
        // 如果数据库中未配置或配置为 0，我们通过检查其使用时间 (nData2) 间接证明玩家确实装备了夺命符，从而启动降级保护
        if (ItemDB::GetInstance().GetEquippedItemDataValue(dwCharID, 9, 2) > 0) { 
            critBonus = 20;
        }
    }
    if (critBonus > 0) {
        equipCrit += critBonus;
    }

    LOG("[RecalcStats] charID=" + std::to_string(dwCharID) + " TOTALS: equipAtk=" + std::to_string(equipAtk) + " equipDef=" + std::to_string(equipDef) + " equipSpd=" + std::to_string(equipSpd) + " equipCrit=" + std::to_string(equipCrit) + " equipHp=" + std::to_string(equipHp) + " equipIp=" + std::to_string(equipIp) + " restoreHp=" + std::to_string(equipRestoreHp) + " restoreIp=" + std::to_string(equipRestoreIp));

    // 3. Combine them in memory
    SOCKET s = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
    DWORD pMapID = SessionMgr::GetInstance().GetMapID(s);
    if (g_MapInstances.count(pMapID)) {
        CMapInstance* mapInst = g_MapInstances[pMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwObjectID);
        if (pObj) {
            pObj->wStr = baseStr;
            pObj->wDex = baseDex;
            pObj->wVit = baseVit;
            pObj->wInt = baseInt; // Actually wSus in this context

            // Load visual equipment and fame from DB into memory
            CharacterDB::GetInstance().LoadVisualEquipAndFame(dwCharID, pObj);

            // Broadcast equipment changes to other players in the same map
            for (BYTE pos = 0; pos < 9; pos++) {
                std::vector<BYTE> ackBuf; ackBuf.resize(4);
                
                auto pushDWord = [&](DWORD d) { ackBuf.push_back(d & 0xFF); ackBuf.push_back((d>>8)&0xFF); ackBuf.push_back((d>>16)&0xFF); ackBuf.push_back((d>>24)&0xFF); };
                auto pushWord = [&](WORD w) { ackBuf.push_back(w & 0xFF); ackBuf.push_back((w>>8)&0xFF); };
                auto pushByte = [&](BYTE b) { ackBuf.push_back(b); };
                
                pushDWord(dwCharID);   // dwCharID
                pushDWord(0);          // dwItemID
                pushByte(pos);         // bPos
                pushWord(pObj->wVisualID[pos]);  // wVisualID
                pushByte(pObj->bRarity[pos]);    // bRarity
                pushByte(pObj->bStxType[pos]);   // bStxType
                
                PACKET_HEADER* ah = (PACKET_HEADER*)ackBuf.data();
                ah->id = 0x3F0C; // CS_CD_CHGEQUIPMENT_ACK
                ah->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(ackBuf.data(), 0x42);
                
                // Broadcast to all sockets on the map
                SessionMgr::GetInstance().ForEachSocketInMap(pMapID, [&](SOCKET sSocket, DWORD sCharID) {
                    SafeSend(sSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
                });
            }
        
            pObj->wWepAtk = equipAtk;
            pObj->wWepDef = equipDef;
            pObj->wWepMag = equipMag;
            pObj->wPlusSpeed = equipSpd;
            pObj->wCritical = equipCrit;
            pObj->wEquipHp = equipHp;
            pObj->wEquipIp = equipIp;
            pObj->wEquipRestoreHp = equipRestoreHp;
            pObj->wEquipRestoreIp = equipRestoreIp;
            
            int maxBuffSpd = 0;
            if (pObj->activeBuffs.count(124) > 0) { // 草上飞
                BYTE lvl = pObj->activeBuffs[124].bLevel;
                int currentSpd = 4 + (lvl > 0 ? lvl : 1);
                if (currentSpd > maxBuffSpd) maxBuffSpd = currentSpd;
            }
            if (pObj->activeBuffs.count(94) > 0) { // 疾风步
                BYTE lvl = pObj->activeBuffs[94].bLevel;
                int currentSpd = 5 + (lvl > 0 ? lvl : 1);
                if (currentSpd > maxBuffSpd) maxBuffSpd = currentSpd;
            }
            if (pObj->activeBuffs.count(64) > 0) { // 凌波微步
                BYTE lvl = pObj->activeBuffs[64].bLevel;
                int currentSpd = 4 + (lvl > 0 ? lvl : 1);
                if (currentSpd > maxBuffSpd) maxBuffSpd = currentSpd;
            }
            if (pObj->activeBuffs.count(34) > 0) { // 斗转星移
                BYTE lvl = pObj->activeBuffs[34].bLevel;
                int currentSpd = 3 + (lvl > 0 ? lvl : 1);
                if (currentSpd > maxBuffSpd) maxBuffSpd = currentSpd;
            }

            // 初始基础行走速度调低至 4 格，防游戏后期速度发飘瞬移拉扯，为装备成长留出平衡空间
            pObj->wWalkSpeed = 4 + equipSpd + maxBuffSpd;
            // Attack speed: computed on-the-fly in PreAttackReq, not stored on PlayerData

            // Calculate and update HP/IP max on the object (same formula as SendCharStatusInfoAck)
            pObj->dwHpMax = (baseInt * 8) + ((baseLevel - 1) * 8) + equipHp; // baseInt = wSus
            DWORD computedIpMax = (baseVit * 0) + ((baseLevel - 1) * 4) + (DWORD)equipIp;
            pObj->wIpMax = computedIpMax;
            // Don't let current exceed max
            if (pObj->dwHpCur > pObj->dwHpMax) pObj->dwHpCur = pObj->dwHpMax;
            if (pObj->wIpCur > pObj->wIpMax) pObj->wIpCur = pObj->wIpMax;

            LOG("[RecalcStats] Applied to pObj: restoreHp=" + std::to_string(pObj->wEquipRestoreHp) + " restoreIp=" + std::to_string(pObj->wEquipRestoreIp) + " dwHpMax=" + std::to_string(pObj->dwHpMax) + " dwHpCur=" + std::to_string(pObj->dwHpCur) + " wIpMax=" + std::to_string(pObj->wIpMax));
        
            // ========== Additive Percentage Stat Calculation ==========
            // Phase 1: Accumulate ALL flat bonuses
            DWORD flatAtk  = baseStr + equipAtk;
            DWORD flatDef  = (baseInt * 2) + equipDef;  // baseInt = wSus
            DWORD flatHit  = baseDex + equipMag;
            DWORD flatDodge = baseDex + equipSpd;
            DWORD flatHpMax = 0;
            DWORD flatIpMax = 0;
            DWORD flatCrit  = equipCrit;

            // Phase 2: Accumulate ALL percentage bonuses (using signed integers to support debuff reductions)
            int percAtk  = 0;
            int percDef  = 0;
            int percHit  = 0;
            int percCrit = 0;

            // 4.5. Passive Inner Skill (崘懎濮) flat + perc (dwMugongID 1-29)
            for (auto& mg : pObj->learnedMugongs) {
                DWORD mugID = mg.first;
                BYTE mugLvl = mg.second;
                if (mugID >= 1 && mugID <= 29) {
                    sMugongList* pd = MugongManager::GetInstance()->GetMugongLevelData(mugID, mugLvl);
                    if (pd) {
                        flatAtk  += pd->wIncAtk;
                        flatDef  += pd->wIncDef;
                        flatHit  += pd->wIncRate;
                        flatHpMax += pd->wIncHpMax;
                        flatIpMax += pd->wIncIpMax;
                        flatCrit  += pd->wIncCritical;

                        if (pd->wIncAtkPerc > 0) percAtk += ((int)pd->wIncAtkPerc - 100);
                        if (pd->wIncDefPerc > 0) percDef += ((int)pd->wIncDefPerc - 100);
                        if (pd->wIncRatePerc > 0) percHit += ((int)pd->wIncRatePerc - 100);
                        if (pd->wIncCriticalPerc > 0) percCrit += ((int)pd->wIncCriticalPerc - 100);

                        LOG("[RecalcStats] Passive ID=" + std::to_string(mugID) + " Lv=" + std::to_string(mugLvl) 
                            + " flatAtk+" + std::to_string(pd->wIncAtk) + " percAtk+" + std::to_string(pd->wIncAtkPerc)
                            + " flatDef+" + std::to_string(pd->wIncDef) + " percDef+" + std::to_string(pd->wIncDefPerc));
                    }
                }
            }

            // 5. Active Buffs flat + perc (dwKeepUpTime > 0)
            DWORD currentTick = GetTickCount();
            for (auto it = pObj->activeBuffs.begin(); it != pObj->activeBuffs.end(); ) {
                if (currentTick > it->second.dwEndTime) {
                    it = pObj->activeBuffs.erase(it);
                } else {
                    sMugongList* bd = MugongManager::GetInstance()->GetMugongLevelData(it->second.dwMugongID, it->second.bLevel);
                    if (bd) {
                        flatAtk  += bd->wIncAtk;
                        flatDef  += bd->wIncDef;
                        flatHit  += bd->wIncRate;
                        flatHpMax += bd->wIncHpMax;
                        flatIpMax += bd->wIncIpMax;
                        flatCrit  += bd->wIncCritical;

                        if (bd->wIncAtkPerc > 0) percAtk += ((int)bd->wIncAtkPerc - 100);
                        if (bd->wIncDefPerc > 0) percDef += ((int)bd->wIncDefPerc - 100);
                        if (bd->wIncRatePerc > 0) percHit += ((int)bd->wIncRatePerc - 100);
                        if (bd->wIncCriticalPerc > 0) percCrit += ((int)bd->wIncCriticalPerc - 100);
                    }
                    ++it;
                }
            }

            // Phase 3: Apply percentages once 垾?totalFlat * (100 + totalPerc) / 100
            int finalAtkPerc = 100 + percAtk; if (finalAtkPerc < 1) finalAtkPerc = 1;
            int finalDefPerc = 100 + percDef; if (finalDefPerc < 1) finalDefPerc = 1;
            int finalHitPerc = 100 + percHit; if (finalHitPerc < 1) finalHitPerc = 1;
            int finalCritPerc = 100 + percCrit; if (finalCritPerc < 1) finalCritPerc = 1;

            DWORD totalAtk   = flatAtk * finalAtkPerc / 100;
            DWORD totalDef   = flatDef * finalDefPerc / 100;
            DWORD totalHit   = flatHit * finalHitPerc / 100;
            DWORD totalDodge = flatDodge;
            DWORD totalCrit  = flatCrit * finalCritPerc / 100;

            // Update HP/IP max with all bonuses
            pObj->dwHpMax += flatHpMax;
            pObj->wIpMax  += (DWORD)flatIpMax;

            // 6. Save final stats to the object
            pObj->dwTotalAtk = totalAtk;
            pObj->dwTotalDef = totalDef;
            pObj->dwTotalHit = totalHit;
            pObj->dwTotalDodge = totalDodge;
            pObj->wCritical = (WORD)totalCrit;
            LOG("[RecalcStats] Final: flatAtk=" + std::to_string(flatAtk) + " percAtk=" + std::to_string(percAtk) + "% -> totalAtk=" + std::to_string(totalAtk)
                + " | flatDef=" + std::to_string(flatDef) + " percDef=" + std::to_string(percDef) + "% -> totalDef=" + std::to_string(totalDef)
                + " | crit=" + std::to_string(totalCrit) + " HpMax=" + std::to_string(pObj->dwHpMax) + " IpMax=" + std::to_string(pObj->wIpMax));
        }
    }

    // 7. Send packet to client
    if (sendPacket) {
        SOCKET s = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
        if (s) {
            SendCharStatusInfoAck(s, dwCharID, 0x4414); // CS_IT_CHARSTATUSINFO_ACK
            // Client's Init_WindowOutSide/InSide clears mugong data upon receiving 0x4414,
            // so we must immediately re-send the mugong lists to repopulate the UI.
            BYTE typeGeneral = 0; // General/Outgong (灂垐杞)
            OnMugongListReq(s, dwCharID, &typeGeneral, 1);
            BYTE typePassive = 1; // Passive/Ingong (洰悡杞)  also triggers Active list
            OnMugongListReq(s, dwCharID, &typePassive, 1);

            // Send 0x3B0D (HP/IP bar update) to ensure bars are refreshed with in-memory values
            if (g_MapInstances.count(pMapID)) {
                CMapInstance* mapInst2 = g_MapInstances[pMapID];
                std::lock_guard<std::mutex> lock2(mapInst2->GetMutex());
                sServerObject* pObj2 = mapInst2->GetPlayer(dwObjectID);
                if (pObj2) {
                    std::vector<BYTE> hpBuf(4);
                    auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                    auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                    push4(pObj2->dwHpMax);
                    push4(pObj2->dwHpCur);
                    push4(pObj2->wIpMax);
                    push4(pObj2->wIpCur);
                    hpBuf.push_back(0); // bType
                    PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                    hpHead->id = 0x3B0D; // CS_IF_CHARHP_ACK
                    hpHead->payloadSize = hpBuf.size() - 4;
                    EncryptPacket(hpBuf.data(), 0x42);
                    SafeSend(s, (const char*)hpBuf.data(), hpBuf.size(), 0);
                    LOG("[RecalcStats] Sent 0x3B0D bar update: HpCur=" + std::to_string(pObj2->dwHpCur) + "/" + std::to_string(pObj2->dwHpMax)
                        + " IpCur=" + std::to_string(pObj2->wIpCur) + "/" + std::to_string(pObj2->wIpMax));
                }
            }
        }
    }
}

void PlayerManager::SavePlayer(DWORD dwCharID) {
    if (dwCharID == 0) return;
    DWORD dwObjectID = (dwCharID < 800000000) ? (dwCharID + 400000000) : dwCharID;

    SOCKET s = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
    if (!s) return;
    DWORD mapID = SessionMgr::GetInstance().GetMapID(s);

    if (g_MapInstances.count(mapID)) {
        CMapInstance* mapInst = g_MapInstances[mapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwObjectID);
        if (pObj) {
            CharacterDB::GetInstance().SavePosition(dwCharID, pObj->wPosX, pObj->wPosY, mapID);
            CharacterDB::GetInstance().UpdateHpIp(dwCharID, pObj->dwHpCur, pObj->wIpCur);

            LOG("[PlayerManager] Saved player " + std::to_string(dwCharID) + " data. POS: " + std::to_string(pObj->wPosX) + "," + std::to_string(pObj->wPosY) + " MAP: " + std::to_string(mapID));
        }
    }
}

// ========================================================================
// Free functions migrated from UnitServer.cpp / MonsterAI.cpp (Phase 3)
// ========================================================================

void UpdatePlayerStatsAndSend(SOCKET clientSocket, DWORD dwCharID) {
    PlayerManager::GetInstance().RecalculateStats(dwCharID, true);
}

void BroadcastPacketToMap(DWORD mapID, const std::vector<BYTE>& packet) {
    SessionMgr::GetInstance().ForEachSocketInMap(mapID, [&](SOCKET s, DWORD /*charID*/) {
        SafeSend(s, (const char*)packet.data(), (int)packet.size(), 0);
    });
}

void SendCharStatusInfoAck(SOCKET clientSocket, DWORD dwCharID, WORD opCode) {
    if (!clientSocket || dwCharID == 0) return;

    DWORD dwObjectID = (dwCharID < 800000000) ? (dwCharID + 400000000) : dwCharID;
    
    PlayerData playerObj;
    playerObj.wWepAtk = 0; playerObj.wWepDef = 0; playerObj.wWepMag = 0; playerObj.wWalkSpeed = 24;
    bool bHasMapObj = false;
    
    {
        DWORD mapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        if (g_MapInstances.count(mapID)) {
            CMapInstance* pMap = g_MapInstances[mapID];
            std::lock_guard<std::mutex> lock(pMap->GetMutex());
            PlayerData* pObj = pMap->GetPlayer(dwObjectID);
            if (pObj) {
                playerObj = *pObj;
                bHasMapObj = true;
            }
        }
    }

    std::vector<BYTE> ackBuf; ackBuf.resize(4);
    
    std::string q_unused = ""; // kept for reference
    
    LOG("[PlayerManager] Querying DB for dwCharID: " + std::to_string(dwCharID) + " opCode: " + std::to_string(opCode));
    CharacterDB::CharFullStatus fs;
    bool dbRet = CharacterDB::GetInstance().GetCharFullStatus(dwCharID, fs);
    LOG("[PlayerManager] GetCharFullStatus returned " + std::to_string(dbRet));
    
    if (dbRet) {
        LOG("[PlayerManager] DB Query returned a row for dwCharID: " + std::to_string(dwCharID));
        WORD wLevel = fs.wLevel, wStr = fs.wStr, wSus = fs.wSus, wDex = fs.wDex, wVit = fs.wVit;
        DWORD wIpMax = fs.wIpMax, wIpCur = fs.wIpCur;
        WORD wRemainSp = fs.wRemainSp, wRemainTp = fs.wRemainTp;
        DWORD dwHpMax = fs.dwHpMax, dwHpCur = fs.dwHpCur, dwTotalSp = fs.dwTotalSp, dwTotalTp = fs.dwTotalTp, dwMoney = fs.dwMoney, dwFame = fs.dwFame;
        long long int dwExp = fs.dwExp, levelExp = 0, nextLevelExp = 0;
        
        // Calculate Max HP and Max IP dynamically
        dwHpMax = (wSus * 8) + ((wLevel - 1) * 8) + playerObj.wEquipHp;
        DWORD computedIpMax = (wVit * 0) + ((wLevel - 1) * 4) + (DWORD)playerObj.wEquipIp;
        wIpMax = computedIpMax;
        
        LOG("[SendStatusAck] BEFORE fix: dwHpCur=" + std::to_string(dwHpCur) + " dwHpMax=" + std::to_string(dwHpMax)
            + " wIpCur=" + std::to_string(wIpCur) + " wIpMax=" + std::to_string(wIpMax)
            + " equipHp=" + std::to_string(playerObj.wEquipHp) + " equipIp=" + std::to_string(playerObj.wEquipIp)
            + " wSus=" + std::to_string(wSus) + " wVit=" + std::to_string(wVit) + " wLevel=" + std::to_string(wLevel));
        
        if (bHasMapObj) {
            dwHpCur = playerObj.dwHpCur;
            wIpCur = playerObj.wIpCur;
            dwHpMax = playerObj.dwHpMax;
            wIpMax = playerObj.wIpMax;
        }
        
        if (dwHpCur > dwHpMax || dwHpCur <= 8) dwHpCur = dwHpMax;
        if (wIpCur > wIpMax || wIpCur == 0) wIpCur = wIpMax;
        
        LOG("[SendStatusAck] AFTER fix: dwHpCur=" + std::to_string(dwHpCur) + " dwHpMax=" + std::to_string(dwHpMax)
            + " wIpCur=" + std::to_string(wIpCur) + " wIpMax=" + std::to_string(wIpMax));
        
        if (g_LevelTemplates.count(wLevel)) {
            levelExp = g_LevelTemplates[wLevel].begin()->second.dwNeedExp;
        }
        if (g_LevelTemplates.count(wLevel + 1)) {
            nextLevelExp = g_LevelTemplates[wLevel + 1].begin()->second.dwNeedExp;
        }
        
        if (nextLevelExp <= levelExp) nextLevelExp = levelExp + 1000;

        long long int tpExp = 0, nextTpExp = 1000;
        long long int diff = nextLevelExp - levelExp;
        long long int segSize = diff / 6;
        if (segSize <= 0) segSize = 1;
        long long int currentOffset = dwExp - levelExp;
        if (currentOffset < 0) currentOffset = 0;
        long long int currentSegIndex = currentOffset / segSize;
        if (currentSegIndex > 5) currentSegIndex = 5;
        long long int segBaseExp = levelExp + (currentSegIndex * segSize);
        long long int segNextExp = segBaseExp + segSize;
        if (currentSegIndex == 5) segNextExp = nextLevelExp;
        tpExp = segBaseExp;
        nextTpExp = segNextExp;

        auto pushWord = [&](WORD w) { ackBuf.push_back((BYTE)(w & 0xFF)); ackBuf.push_back((BYTE)(w >> 8)); };
        auto pushDword = [&](DWORD d) { pushWord((WORD)(d & 0xFFFF)); pushWord((WORD)(d >> 16)); };
        auto pushInt64 = [&](long long int d) { pushDword((DWORD)(d & 0xFFFFFFFF)); pushDword((DWORD)(d >> 32)); };
        
        if (opCode == CS_IF_CHARINFO_ACK) {
            pushWord(wLevel); pushWord(wStr); pushWord(wSus); pushWord(wDex); pushWord(wVit);
            pushWord(wRemainSp); pushDword(dwTotalSp);
            pushDword(playerObj.dwTotalAtk);
            pushDword(playerObj.dwTotalDef);
            pushDword(playerObj.dwTotalHit);
            ackBuf.push_back(0);
            ackBuf.push_back(playerObj.wWalkSpeed & 0xFF);
            pushDword(dwHpCur); pushDword(dwHpMax); 
            pushDword(wIpCur); pushDword(wIpMax);
            pushWord(playerObj.wCritical);
            pushWord(wStr); pushWord(wSus * 2); pushWord(wDex);
            ackBuf.push_back(10);
            pushWord(20);
            ackBuf.push_back(playerObj.wPlusSpeed & 0xFF);
            pushWord(wRemainTp); pushDword(dwTotalTp);
            pushDword(dwFame);
            ackBuf.push_back(0); ackBuf.push_back(0);
            pushDword(0); pushDword(0); pushDword(0);
        } else if (opCode == CS_IT_CHARSTATUSINFO_ACK) {
            pushWord(wLevel); pushWord(wStr); pushWord(wSus); pushWord(wDex); pushWord(wVit);
            ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0);
            pushDword(wIpMax); pushDword(wIpCur); pushDword(dwHpMax); pushDword(dwHpCur);
            pushInt64(dwExp); pushInt64(levelExp); pushInt64(nextLevelExp); pushInt64(tpExp); pushInt64(nextTpExp);
            pushDword(dwTotalSp); pushWord(wRemainSp); pushDword(dwTotalTp); pushWord(wRemainTp);
            pushWord(wStr); pushDword(playerObj.dwTotalAtk);
            pushWord(wSus * 2); pushDword(playerObj.dwTotalDef);
            pushWord(wDex); pushDword(playerObj.dwTotalHit);
            pushWord(20);
            ackBuf.push_back(playerObj.wWalkSpeed & 0xFF); ackBuf.push_back(playerObj.wWalkSpeed & 0xFF); ackBuf.push_back(playerObj.wPlusSpeed & 0xFF);
            ackBuf.push_back(0);
            pushDword(0);
            pushDword(dwMoney);
            pushWord(playerObj.wCritical);
            ackBuf.push_back(10); ackBuf.push_back(0);
            pushDword(dwFame);
            pushWord(0);
            pushDword(0); pushDword(0); pushDword(0);
            pushWord(0); pushWord(0); pushWord(0); pushWord(0); pushWord(0);
            ackBuf.push_back(0);
        }
    }
    
    LOG("[PlayerManager] ackBuf size: " + std::to_string(ackBuf.size()));
        
    
    if (ackBuf.size() > 4) {
        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
        ackHead->id = opCode;
        ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
        
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    }
}



#include "PlayerManager.h"
#include "MugongManager.h"
#include "../DBHelper.h"
#include "../UnitServer.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../Handlers/MugongHandler.h"

extern std::map<DWORD, CMapInstance*> g_MapInstances;

void PlayerManager::RecalculateStats(DWORD dwCharID, bool sendPacket) {
    DWORD dwObjectID = (dwCharID < 800000000) ? (dwCharID + 400000000) : dwCharID;

    // 1. Fetch base attributes from DB
    int baseStr = 10, baseDex = 10, baseVit = 10, baseInt = 10, baseLevel = 1;
    std::string qStats = "SELECT wStr, wDex, wVit, wSus, wLevel FROM CHAR_DATA WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteQuery(qStats, [&](SQLHSTMT hStmt) {
        SQLLEN c1, c2, c3, c4, c5;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &baseStr, 0, &c1);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &baseDex, 0, &c2);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &baseVit, 0, &c3);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &baseInt, 0, &c4);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &baseLevel, 0, &c5);
    });

    // 2. Fetch equipment stats from DB
    int equipAtk=0, equipDef=0, equipMag=0, equipSpd=0, equipAtkSpd=0, equipCrit=0, equipHp=0, equipIp=0, equipRestoreHp=0, equipRestoreIp=0;
    std::string qEq = "SELECT I.wRefID, ISNULL(D.nData4, -9999), ISNULL(D.nData5, -9999), ISNULL(D.nData6, -9999), ISNULL(D.nData7, -9999), ISNULL(D.nData13, -9999), ISNULL(D.nData9, -9999), ISNULL(D.nData10, -9999), ISNULL(D.nData11, -9999), ISNULL(D.nData12, -9999) FROM SACKITEM S JOIN ITEM I ON S.dwItemID = I.dwItemID LEFT JOIN ITEMDATA D ON S.dwItemID = D.dwItemID WHERE S.dwCharID = " + std::to_string(dwCharID) + " AND S.bSackPos < 20";
    
    DBHelper::GetInstance().ExecuteQuery(qEq, [&](SQLHSTMT hStmt) {
        WORD ref=0; int d4=0, d5=0, d6=0, d7=0, d13=0, d9=0, d10=0, d11=0, d12=0; SQLLEN c[10];
        SQLGetData(hStmt, 1, SQL_C_USHORT, &ref, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &d4, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &d5, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &d6, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &d7, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_SLONG, &d13, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &d9, 0, &c[6]);
        SQLGetData(hStmt, 8, SQL_C_SLONG, &d10, 0, &c[7]);
        SQLGetData(hStmt, 9, SQL_C_SLONG, &d11, 0, &c[8]);
        SQLGetData(hStmt, 10, SQL_C_SLONG, &d12, 0, &c[9]);
        
        if (d4 == -9999) d4 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData4 : 0);
        if (d5 == -9999) d5 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData5 : 0);
        if (d6 == -9999) d6 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData6 : 0);
        if (d7 == -9999) d7 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData7 : 0);
        if (d13 == -9999) d13 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData13 : 0);
        if (d9 == -9999) d9 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData9 : 0);
        if (d10 == -9999) d10 = (g_ItemTemplates.count(ref) ? g_ItemTemplates[ref].nData10 : 0);
        if (d11 == -9999) d11 = 0; // nData11 not in template, default 0
        if (d12 == -9999) d12 = 0; // nData12 not in template, default 0
        
        equipAtk += d4; equipDef += d5; equipMag += d6; equipCrit += d13;
        // nData7 (StkSpeed): Shoes → Move Speed, Weapons/Other → Attack Speed
        if (g_ItemTemplates.count(ref) && g_ItemTemplates[ref].bType == 4) {
            equipSpd += d7; // Shoe: move speed bonus
        } else {
            equipAtkSpd += d7; // Weapon/Other: attack speed bonus
        }
        equipHp += d9; equipIp += d10; equipRestoreHp += d11; equipRestoreIp += d12;
        LOG("[RecalcStats] EquipRow ref=" + std::to_string(ref) + " d7(spd)=" + std::to_string(d7) + " d9(hp)=" + std::to_string(d9) + " d10(ip)=" + std::to_string(d10) + " d11(restHp)=" + std::to_string(d11) + " d12(restIp)=" + std::to_string(d12) + " d13(crit)=" + std::to_string(d13));
    });

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
        
            pObj->wWepAtk = equipAtk;
            pObj->wWepDef = equipDef;
            pObj->wWepMag = equipMag;
            pObj->wPlusSpeed = equipSpd;
            pObj->wCritical = equipCrit;
            pObj->wEquipHp = equipHp;
            pObj->wEquipIp = equipIp;
            pObj->wEquipRestoreHp = equipRestoreHp;
            pObj->wEquipRestoreIp = equipRestoreIp;
            pObj->wWalkSpeed = 6 + equipSpd;
            // Attack speed: base 9 + weapon StkSpeed bonus. Higher = faster animation.
            int atkSpd = 9 + equipAtkSpd;
            if (atkSpd < 1) atkSpd = 1;
            if (atkSpd > 27) atkSpd = 27; // Client caps at 3.0x (27/9=3.0)
            pObj->wAtkSpeed = (WORD)atkSpd;

            // Calculate and update HP/IP max on the object (same formula as SendCharStatusInfoAck)
            pObj->dwHpMax = (baseInt * 8) + ((baseLevel - 1) * 8) + equipHp; // baseInt = wSus
            pObj->wIpMax = (baseVit * 0) + ((baseLevel - 1) * 4) + equipIp;
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

            // Phase 2: Accumulate ALL percentage bonuses
            DWORD percAtk  = 0;
            DWORD percDef  = 0;
            DWORD percHit  = 0;
            DWORD percCrit = 0;

            // 4.5. Passive Inner Skill (鍐呭姛) flat + perc (dwMugongID 1-29)
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

                        if (pd->wIncAtkPerc > 100) percAtk += (pd->wIncAtkPerc - 100);
                        if (pd->wIncDefPerc > 100) percDef += (pd->wIncDefPerc - 100);
                        if (pd->wIncRatePerc > 100) percHit += (pd->wIncRatePerc - 100);
                        if (pd->wIncCriticalPerc > 100) percCrit += (pd->wIncCriticalPerc - 100);

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

                        if (bd->wIncAtkPerc > 100) percAtk += (bd->wIncAtkPerc - 100);
                        if (bd->wIncDefPerc > 100) percDef += (bd->wIncDefPerc - 100);
                        if (bd->wIncRatePerc > 100) percHit += (bd->wIncRatePerc - 100);
                        if (bd->wIncCriticalPerc > 100) percCrit += (bd->wIncCriticalPerc - 100);
                    }
                    ++it;
                }
            }

            // Phase 3: Apply percentages once 鈥?totalFlat * (100 + totalPerc) / 100
            DWORD totalAtk   = flatAtk * (100 + percAtk) / 100;
            DWORD totalDef   = flatDef * (100 + percDef) / 100;
            DWORD totalHit   = flatHit * (100 + percHit) / 100;
            DWORD totalDodge = flatDodge;
            DWORD totalCrit  = flatCrit * (100 + percCrit) / 100;

            // Update HP/IP max with all bonuses
            pObj->dwHpMax += flatHpMax;
            pObj->wIpMax  += (WORD)flatIpMax;

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
            BYTE typeGeneral = 0; // General/Outgong (鞕戈车)
            OnMugongListReq(s, dwCharID, &typeGeneral, 1);
            BYTE typePassive = 1; // Passive/Ingong (雮搓车) — also triggers Active list
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
                    push2(pObj2->wIpMax);
                    push2(pObj2->wIpCur);
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
            std::string qStat = "UPDATE CHAR_STATUS SET dwMapID = " + std::to_string(mapID) +
                                ", wPosX = " + std::to_string(pObj->wPosX) +
                                ", wPosY = " + std::to_string(pObj->wPosY) +
                                " WHERE dwCharID = " + std::to_string(dwCharID);
            DBHelper::GetInstance().ExecuteUpdate(qStat);

            std::string qPower = "UPDATE CHAR_POWER SET dwHpCur = " + std::to_string(pObj->dwHpCur) +
                                 ", wIpCur = " + std::to_string(pObj->wIpCur) +
                                 " WHERE dwCharID = " + std::to_string(dwCharID);
            DBHelper::GetInstance().ExecuteUpdate(qPower);

            LOG("[PlayerManager] Saved player " + std::to_string(dwCharID) + " data. POS: " + std::to_string(pObj->wPosX) + "," + std::to_string(pObj->wPosY) + " MAP: " + std::to_string(mapID));
        }
    }
}


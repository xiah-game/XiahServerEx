#include "MapInstance.h"
#include "../Network/SessionMgr.h"
#include "../DBHelper.h"
#include <cmath>
#include <unordered_set>
#include "MugongManager.h"
#include "../Handlers/MugongHandler.h"
#include "DropManager.h"
#include "ExpSystem.h"
#include "PlayerManager.h"
#include "../DB/CharacterDB.h"
#include "../Handlers/FiveElmHandler.h"



CMapInstance::CMapInstance(DWORD mapID, int width, int height, const std::vector<BYTE>& collisionGrid)
    : m_dwMapID(mapID), m_width(width), m_height(height), m_collisionGrid(collisionGrid) {
    m_gridCols = (m_width + GRID_SIZE - 1) / GRID_SIZE;
    if (m_gridCols <= 0) m_gridCols = 1;
    m_gridRows = (m_height + GRID_SIZE - 1) / GRID_SIZE;
    if (m_gridRows <= 0) m_gridRows = 1;
    
    m_playerGrid.resize(m_gridCols * m_gridRows);
    m_monsterGrid.resize(m_gridCols * m_gridRows);
}

CMapInstance::~CMapInstance() {
    m_players.clear();
    m_monsters.clear();
    m_playerGrid.clear();
    m_monsterGrid.clear();
}

void CMapInstance::AddPlayer(const PlayerData& player) {
    m_players[player.dwObjectID] = player;
    int idx = GetGridIndex(player.wPosX, player.wPosY);
    if (idx >= 0) AddToGrid(m_playerGrid, idx, player.dwObjectID);
}

void CMapInstance::RemovePlayer(DWORD dwObjectID) {
    auto it = m_players.find(dwObjectID);
    if (it != m_players.end()) {
        // [业务设计意图]
        // 玩家因下线、切图等原因移出地图时，必须同步清理其召唤的所有分身，杜绝分身作为孤儿残留地图导致怪物转火。
        // [潜在风险]
        // CleanupAllBunsins 会调用 RemovePlayer。对于分身 ID（>= 850000000），此处过滤了防重入，因此不会无限递归。
        if (dwObjectID >= 400000000 && dwObjectID < 850000000) {
            extern void CleanupAllBunsins(DWORD ownerCharID, DWORD mapID);
            CleanupAllBunsins(dwObjectID - 400000000, m_dwMapID);
        }

        // 清理玩家召唤的真实宠物，防止宠物作为虚体在地图上永久驻留
        if (dwObjectID >= 400000000 && dwObjectID < 800000000) {
            std::vector<DWORD> petsToRemove;
            for (auto& pair : m_players) {
                if (pair.second.dwObjectID >= 800000000 && pair.second.dwObjectID < 850000000) {
                    if (pair.second.dwOwnerID == dwObjectID) {
                        petsToRemove.push_back(pair.second.dwObjectID);
                    }
                }
            }
            for (DWORD petID : petsToRemove) {
                // 广播 MAPLEAVE_ACK (0x3506) 告知周围客户端删除宠物虚体
                std::vector<BYTE> leaveBuf;
                leaveBuf.resize(4, 0);
                auto pushByte = [&](BYTE b) { leaveBuf.push_back(b); };
                auto pushDWord = [&](DWORD d) { leaveBuf.push_back(d & 0xFF); leaveBuf.push_back((d >> 8) & 0xFF); leaveBuf.push_back((d >> 16) & 0xFF); leaveBuf.push_back(d >> 24); };
                pushByte(0);
                pushDWord(petID);
                pushByte(4); // bObjectType = 4 (PET)
                pushDWord(m_dwMapID);
                PACKET_HEADER* leaveHead = (PACKET_HEADER*)leaveBuf.data();
                leaveHead->id = 0x3506;
                leaveHead->payloadSize = (WORD)(leaveBuf.size() - sizeof(PACKET_HEADER));
                EncryptPacket(leaveBuf.data(), 0x42);
                BroadcastPacketAOI_NoLock(m_players[petID].wPosX, m_players[petID].wPosY, leaveBuf);

                // 保存战宠当前生命值到数据库
                DWORD rawPetID = petID - 800000000;
                DBHelper::GetInstance().ExecuteUpdate(
                    "UPDATE CHAR_PET SET dwHpCur = " + std::to_string(m_players[petID].dwHpCur) + " WHERE dwID = " + std::to_string(rawPetID));

                // 从九宫格网格及角色列表中彻底移除战宠实体
                int pIdx = GetGridIndex(m_players[petID].wPosX, m_players[petID].wPosY);
                if (pIdx >= 0) RemoveFromGrid(m_playerGrid, pIdx, petID);
                m_players.erase(petID);
                LOG("[MapInstance] Cleaned up active pet dwObjectID=" + std::to_string(petID) + " for disconnecting owner dwCharID=" + std::to_string(dwObjectID));
            }
        }

        int idx = GetGridIndex(it->second.wPosX, it->second.wPosY);
        if (idx >= 0) RemoveFromGrid(m_playerGrid, idx, dwObjectID);
        m_players.erase(it);
    }
}

PlayerData* CMapInstance::GetPlayer(DWORD dwObjectID) {
    auto it = m_players.find(dwObjectID);
    if (it != m_players.end()) {
        return &(it->second);
    }
    return nullptr;
}

void CMapInstance::AddMonster(const MonsterData& monster) {
    m_monsters[monster.dwObjectID] = monster;
    int idx = GetGridIndex(monster.wPosX, monster.wPosY);
    if (idx >= 0) AddToGrid(m_monsterGrid, idx, monster.dwObjectID);
}

void CMapInstance::RemoveMonster(DWORD dwObjectID) {
    auto it = m_monsters.find(dwObjectID);
    if (it != m_monsters.end()) {
        int idx = GetGridIndex(it->second.wPosX, it->second.wPosY);
        if (idx >= 0) RemoveFromGrid(m_monsterGrid, idx, dwObjectID);
        m_monsters.erase(it);
    }
}

MonsterData* CMapInstance::GetMonster(DWORD dwObjectID) {
    auto it = m_monsters.find(dwObjectID);
    if (it != m_monsters.end()) {
        return &(it->second);
    }
    return nullptr;
}

void CMapInstance::BroadcastPacket(const std::vector<BYTE>& packet) {
    SessionMgr::GetInstance().BroadcastToMap(m_dwMapID, packet);
}

void CMapInstance::Update(DWORD tick) {
    // -----------------------------------------------------
    // 战宠与分身跟随及协同战斗 AI
    // -----------------------------------------------------
    for (auto& pair : m_players) {
        PlayerData& pl = pair.second;
        if (pl.dwObjectID >= 800000000 && pl.dwHpCur > 0) {
            DWORD ownerObjID = pl.dwOwnerID + 400000000;
            if (m_players.count(ownerObjID)) {
                PlayerData& owner = m_players[ownerObjID];
                bool isInCombat = false;
                if (pl.dwPetTargetObjectID != 0) {
                    MonsterData* pTargetMon = GetMonster(pl.dwPetTargetObjectID);
                    if (pTargetMon && pTargetMon->dwHpCur > 0) {
                        isInCombat = true;
                        int distMon = std::abs((int)pl.wPosX - (int)pTargetMon->wPosX) + std::abs((int)pl.wPosY - (int)pTargetMon->wPosY);
                        if (distMon <= 2) {
                            if (tick - pl.dwLastPetAttackTime >= 1200) {
                                pl.dwLastPetAttackTime = tick;
                                DWORD petDmg = 25 + pl.wLevel * 3;
                                pTargetMon->dwHpCur = (pTargetMon->dwHpCur > petDmg) ? (pTargetMon->dwHpCur - petDmg) : 0;
                                std::vector<BYTE> atkBuf; atkBuf.resize(4);
                                atkBuf.push_back(0);
                                atkBuf.push_back(pl.dwObjectID & 0xFF); atkBuf.push_back((pl.dwObjectID >> 8) & 0xFF);
                                atkBuf.push_back((pl.dwObjectID >> 16) & 0xFF); atkBuf.push_back(pl.dwObjectID >> 24);
                                atkBuf.push_back(pl.wPosX & 0xFF); atkBuf.push_back(pl.wPosX >> 8);
                                atkBuf.push_back(pl.wPosY & 0xFF); atkBuf.push_back(pl.wPosY >> 8);
                                atkBuf.push_back(pl.bHeight);
                                atkBuf.push_back(1);
                                atkBuf.push_back(pTargetMon->dwObjectID & 0xFF); atkBuf.push_back((pTargetMon->dwObjectID >> 8) & 0xFF);
                                atkBuf.push_back((pTargetMon->dwObjectID >> 16) & 0xFF); atkBuf.push_back(pTargetMon->dwObjectID >> 24);
                                atkBuf.push_back(petDmg & 0xFF); atkBuf.push_back(petDmg >> 8);
                                atkBuf.push_back(0); atkBuf.push_back(0);
                                atkBuf.push_back(0); atkBuf.push_back(0);
                                DWORD maxHp = pTargetMon->dwHpMax;
                                DWORD curHp = pTargetMon->dwHpCur;
                                atkBuf.push_back(maxHp & 0xFF); atkBuf.push_back((maxHp >> 8) & 0xFF);
                                atkBuf.push_back((maxHp >> 16) & 0xFF); atkBuf.push_back(maxHp >> 24);
                                atkBuf.push_back(curHp & 0xFF); atkBuf.push_back((curHp >> 8) & 0xFF);
                                atkBuf.push_back((curHp >> 16) & 0xFF); atkBuf.push_back(curHp >> 24);
                                atkBuf.push_back(0); atkBuf.push_back(0);
                                atkBuf.push_back(0); atkBuf.push_back(0);
                                PACKET_HEADER* atkHead = (PACKET_HEADER*)atkBuf.data();
                                atkHead->id = 0x4006;
                                atkHead->payloadSize = atkBuf.size() - 4;
                                EncryptPacket(atkBuf.data(), 0x42);
                                BroadcastPacketAOI_NoLock(pl.wPosX, pl.wPosY, atkBuf);
                                if (pTargetMon->dwHpCur == 0) {
                                    HandleMonsterDoTDeath(tick, *pTargetMon, pl.dwObjectID, 0, 1);
                                    pl.dwPetTargetObjectID = 0;
                                }
                            }
                        } else {
                            if (tick - pl.dwLastPetMoveTime >= 800) {
                                pl.dwLastPetMoveTime = tick;
                                int oldX = pl.wPosX; int oldY = pl.wPosY;
                                int gridIdxOld = GetGridIndex(oldX, oldY);
                                if (gridIdxOld >= 0) RemoveFromGrid(m_playerGrid, gridIdxOld, pl.dwObjectID);
                                int stepX = (pTargetMon->wPosX > pl.wPosX) ? 1 : ((pTargetMon->wPosX < pl.wPosX) ? -1 : 0);
                                int stepY = (pTargetMon->wPosY > pl.wPosY) ? 1 : ((pTargetMon->wPosY < pl.wPosY) ? -1 : 0);
                                pl.wPosX += stepX; pl.wPosY += stepY;
                                pl.fPosX = (float)pl.wPosX; pl.fPosY = (float)pl.wPosY;
                                int gridIdxNew = GetGridIndex(pl.wPosX, pl.wPosY);
                                if (gridIdxNew >= 0) AddToGrid(m_playerGrid, gridIdxNew, pl.dwObjectID);
                                float angle = std::atan2((float)stepY, (float)stepX) * 180.0f / 3.14159265f;
                                if (angle < 0) angle += 360.0f;
                                WORD wDirection = (WORD)angle;
                                std::vector<BYTE> moveBuf; moveBuf.resize(4);
                                moveBuf.push_back(0);
                                moveBuf.push_back(pl.dwObjectID & 0xFF); moveBuf.push_back((pl.dwObjectID >> 8) & 0xFF);
                                moveBuf.push_back((pl.dwObjectID >> 16) & 0xFF); moveBuf.push_back((pl.dwObjectID >> 24) & 0xFF);
                                moveBuf.push_back(pl.wPosX & 0xFF); moveBuf.push_back(pl.wPosX >> 8);
                                moveBuf.push_back(pl.wPosY & 0xFF); moveBuf.push_back(pl.wPosY >> 8);
                                moveBuf.push_back(pl.bHeight);
                                moveBuf.push_back(pTargetMon->wPosX & 0xFF); moveBuf.push_back(pTargetMon->wPosX >> 8);
                                moveBuf.push_back(pTargetMon->wPosY & 0xFF); moveBuf.push_back(pTargetMon->wPosY >> 8);
                                moveBuf.push_back(pTargetMon->bHeight);
                                moveBuf.push_back(wDirection & 0xFF); moveBuf.push_back(wDirection >> 8);
                                moveBuf.push_back(1);
                                moveBuf.push_back(pl.wWalkSpeed & 0xFF);
                                moveBuf.push_back(0);
                                PACKET_HEADER* moveHead = (PACKET_HEADER*)moveBuf.data();
                                moveHead->id = 0x430C; moveHead->payloadSize = moveBuf.size() - 4;
                                EncryptPacket(moveBuf.data(), 0x42);
                                BroadcastPacketAOI_NoLock(pl.wPosX, pl.wPosY, moveBuf);
                                std::vector<BYTE> stopBuf; stopBuf.resize(4);
                                stopBuf.push_back(0);
                                stopBuf.push_back(pl.dwObjectID & 0xFF); stopBuf.push_back((pl.dwObjectID >> 8) & 0xFF);
                                stopBuf.push_back((pl.dwObjectID >> 16) & 0xFF); stopBuf.push_back((pl.dwObjectID >> 24) & 0xFF);
                                stopBuf.push_back(pl.wPosX & 0xFF); stopBuf.push_back(pl.wPosX >> 8);
                                stopBuf.push_back(pl.wPosY & 0xFF); stopBuf.push_back(pl.wPosY >> 8);
                                stopBuf.push_back(pl.bHeight);
                                stopBuf.push_back(wDirection & 0xFF); stopBuf.push_back(wDirection >> 8);
                                PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data();
                                stopHead->id = 0x4310; stopHead->payloadSize = stopBuf.size() - 4;
                                EncryptPacket(stopBuf.data(), 0x42);
                                BroadcastPacketAOI_NoLock(pl.wPosX, pl.wPosY, stopBuf);
                            }
                        }
                    } else {
                        pl.dwPetTargetObjectID = 0;
                    }
                }
                if (!isInCombat) {
                    int dist = std::abs((int)pl.wPosX - (int)owner.wPosX) + std::abs((int)pl.wPosY - (int)owner.wPosY);
                    // 业务设计意图：非战斗状态下，宠物的正常跟随移动完全由主人客户端 PetAI 自主平滑导航并向服务端上报（0x3507/0x3509/0x350B）。
                    // 服务端不再执行每秒强行+1格并广播0x430C/0x4310急刹车的冲突位移，彻底消除闪跳与抽搐！
                    // 仅在极端脱节（如主人传送卷轴/换地图/距离超过50格）时执行兜底拉回同步。
                    if (dist >= 50) {
                        int oldX = pl.wPosX; int oldY = pl.wPosY;
                        int gridIdxOld = GetGridIndex(oldX, oldY);
                        if (gridIdxOld >= 0) RemoveFromGrid(m_playerGrid, gridIdxOld, pl.dwObjectID);
                        pl.wPosX = owner.wPosX; pl.wPosY = owner.wPosY;
                        pl.fPosX = (float)pl.wPosX; pl.fPosY = (float)pl.wPosY;
                        int gridIdxNew = GetGridIndex(pl.wPosX, pl.wPosY);
                        if (gridIdxNew >= 0) AddToGrid(m_playerGrid, gridIdxNew, pl.dwObjectID);

                        std::vector<BYTE> stopBuf; stopBuf.resize(4, 0);
                        stopBuf.push_back(0); // bResult
                        stopBuf.push_back(pl.dwObjectID & 0xFF); stopBuf.push_back((pl.dwObjectID >> 8) & 0xFF);
                        stopBuf.push_back((pl.dwObjectID >> 16) & 0xFF); stopBuf.push_back((pl.dwObjectID >> 24) & 0xFF);
                        stopBuf.push_back(4); // OBJTYPE_PET
                        stopBuf.push_back(pl.wPosX & 0xFF); stopBuf.push_back(pl.wPosX >> 8);
                        stopBuf.push_back(pl.wPosY & 0xFF); stopBuf.push_back(pl.wPosY >> 8);
                        stopBuf.push_back(pl.bHeight);
                        stopBuf.push_back(0); // bStatus
                        PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data();
                        stopHead->id = 0x350C; stopHead->payloadSize = (WORD)(stopBuf.size() - 4);
                        EncryptPacket(stopBuf.data(), 0x42);
                        BroadcastPacketAOI_NoLock(pl.wPosX, pl.wPosY, stopBuf);
                        LOG("[PetSync] Master far away (dist=" + std::to_string(dist) + "), warped pet " + std::to_string(pl.dwObjectID) + " to master at (" + std::to_string(pl.wPosX) + "," + std::to_string(pl.wPosY) + ")");
                    }
                }
            }
        }
    }
    // -----------------------------------------------------
    // -0. Process Expired or Dead Bunsins (Clones)
    // -----------------------------------------------------
    for (auto it = m_players.begin(); it != m_players.end(); ) {
        PlayerData& pl = it->second;
        if (pl.dwObjectID >= 850000000 && (tick >= pl.dwBunsinEndTime || pl.dwHpCur == 0)) {
            // Broadcast despawn PKT_MAPLEAVE_ACK (0x3506)
            std::vector<BYTE> leaveBuf; leaveBuf.resize(4);
            leaveBuf.push_back(0); // bResult = 0
            DWORD oid = pl.dwObjectID;
            leaveBuf.push_back(oid & 0xFF); leaveBuf.push_back((oid >> 8) & 0xFF); leaveBuf.push_back((oid >> 16) & 0xFF); leaveBuf.push_back(oid >> 24);
            leaveBuf.push_back(4); // bObjectType = 4 (PET)
            DWORD mid = m_dwMapID;
            leaveBuf.push_back(mid & 0xFF); leaveBuf.push_back((mid >> 8) & 0xFF); leaveBuf.push_back((mid >> 16) & 0xFF); leaveBuf.push_back(mid >> 24);
            leaveBuf.push_back(0); // bType = 0 (Normal leave)
            PACKET_HEADER* leaveHead = (PACKET_HEADER*)leaveBuf.data(); leaveHead->id = 0x3506; leaveHead->payloadSize = leaveBuf.size() - sizeof(PACKET_HEADER);
            EncryptPacket(leaveBuf.data(), 0x42);
            BroadcastPacketAOI_NoLock(pl.wPosX, pl.wPosY, leaveBuf);
            
            LOG("[BunsinExpiry] Bunsin ObjID=" + std::to_string(pl.dwObjectID) + " expired or dead. HP=" + std::to_string(pl.dwHpCur) + ". Despawning.");
            
            // Remove from grid
            int idx = GetGridIndex(pl.wPosX, pl.wPosY);
            if (idx >= 0) RemoveFromGrid(m_playerGrid, idx, pl.dwObjectID);
            
            it = m_players.erase(it);
        } else {
            ++it;
        }
    }

    // -----------------------------------------------------
    // 0. Process Deferred Player Death Broadcasts
    // -----------------------------------------------------
    for (auto& pair : m_players) {
        PlayerData& pl = pair.second;
        if (pl.dwDeadTime != 0) {
            LOG("[PlayerDeathDebug] ObjID=" + std::to_string(pl.dwObjectID) 
                + " HpCur=" + std::to_string(pl.dwHpCur)
                + " DeadTime=" + std::to_string(pl.dwDeadTime)
                + " TimeDiff=" + std::to_string(tick - pl.dwDeadTime));
        }
        if (pl.dwHpCur == 0 && pl.dwDeadTime != 0 && (tick - pl.dwDeadTime) >= 500) {
            // Send death broadcast after 500ms delay
            auto pushDWord = [&](std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); };
            auto pushWord = [&](std::vector<BYTE>& b, WORD w) { b.push_back(w&0xFF); b.push_back(w>>8); };
            
            std::vector<BYTE> animBuf; animBuf.resize(4);
            pushDWord(animBuf, pl.dwObjectID);
            animBuf.push_back(5); // CHARSTATE_DIE
            pushWord(animBuf, 0); // wDirection
            pushWord(animBuf, pl.wPosX);
            pushWord(animBuf, pl.wPosY);
            
            PACKET_HEADER* animHead = (PACKET_HEADER*)animBuf.data();
            animHead->id = 0x4002; // CS_BT_CHANGEMODE_ACK
            animHead->payloadSize = animBuf.size() - sizeof(PACKET_HEADER);
            EncryptPacket(animBuf.data(), 0x42);
            BroadcastPacketAOI_NoLock(pl.wPosX, pl.wPosY, animBuf);
            
            LOG("[PlayerDeath] Sent deferred death broadcast for ObjID=" + std::to_string(pl.dwObjectID));
            for (auto& mPair : m_monsters) {
                if (mPair.second.dwTargetID == pl.dwObjectID) {
                    mPair.second.dwTargetID = 0;
                    mPair.second.bInAttackRange = false;
                    float dist = std::sqrt(std::pow((float)mPair.second.wPosX - mPair.second.wSpawnX, 2) + std::pow((float)mPair.second.wPosY - mPair.second.wSpawnY, 2));
                    if (dist > 1.0f) {
                        mPair.second.bIsReturning = true;
                        mPair.second.dwReturnStartTime = tick;
                        mPair.second.fPosX = (float)mPair.second.wPosX;
                        mPair.second.fPosY = (float)mPair.second.wPosY;
                        mPair.second.wDestX = mPair.second.wSpawnX;
                        mPair.second.wDestY = mPair.second.wSpawnY;
                        mPair.second.wLastSentDestX = 0;
                        mPair.second.wLastSentDestY = 0;
                        mPair.second.wLastSentPosX = 0;
                        mPair.second.wLastSentPosY = 0;
                        mPair.second.dwHpCur = mPair.second.dwHpMax;
                    }
                }
            }
            pl.dwDeadTime = 0; // Clear so we don't send again
        }
    }

    // -----------------------------------------------------
    // 1. Process Player Buffs
    // -----------------------------------------------------
    ProcessBuffs(tick);

    // -----------------------------------------------------
    // 2. Interpolate Player Positions (server-side prediction)
    // -----------------------------------------------------
    InterpolatePlayerPositions(tick);

    // -----------------------------------------------------
    // -----------------------------------------------------
    // 3. Process Monster AI
    // -----------------------------------------------------
    for (auto& pair : m_monsters) {
        MonsterData& obj = pair.second;
        // 定期清理已过期的怪物状态 (Debuff)
        for (auto it = obj.activeBuffs.begin(); it != obj.activeBuffs.end(); ) {
            if (tick >= it->second.dwEndTime) {
                // 业务设计意图：补全怪物过期 Buff 的 0x402E 销毁包广播，彻底解决客户端血条下方的 Buff 图标残留 Bug。
                std::vector<BYTE> endAck(4 + 11);
                BYTE* ep = endAck.data() + 4;
                ep[0] = 0; // bResult
                *(DWORD*)(ep + 1) = obj.dwObjectID;
                ep[5] = obj.bObjectType; // 3 = Monster/NPC
                *(DWORD*)(ep + 6) = it->second.dwMugongID;
                ep[10] = it->second.bLevel;
                PACKET_HEADER* headE = (PACKET_HEADER*)endAck.data();
                headE->id = 0x402E; // CS_BT_KEEPUPMUGONGEND_ACK
                headE->payloadSize = 11;
                EncryptPacket(endAck.data(), 0x42);
                BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, endAck);

                LOG("[MonsterBuffExpiry] Debuff " + std::to_string(it->second.dwMugongID) + " expired on monster " + std::to_string(obj.dwObjectID));
                it = obj.activeBuffs.erase(it);
            } else {
                ++it;
            }
        }
        ProcessMonsterAI(tick, obj);
    }

    // -----------------------------------------------------
    // 4. 地面持续 AoE 特效 tick（如寸草不生 bKind=24）
    // -----------------------------------------------------
    for (auto gIt = m_groundEffects.begin(); gIt != m_groundEffects.end(); ) {
        if (tick >= gIt->dwEndTime) {
            LOG("[GroundEffect] Effect " + std::to_string(gIt->dwMugongID) + " expired at (" + std::to_string(gIt->wPosX) + "," + std::to_string(gIt->wPosY) + ")");
            
            // 业务设计意图：向周围广播 0x402E 协议包通知客户端该地面特效已到期。
            // 客户端收到后会彻底注销 Buff 图标并淡出销毁地面毒雾粒子，保障视听反馈与服务端状态的一致性。
            std::vector<BYTE> endAck(4 + 11);
            BYTE* ep = endAck.data() + 4;
            ep[0] = 0; // bResult
            *(DWORD*)(ep + 1) = gIt->dwCasterID;
            ep[5] = 1; // bObjectType = PC (Player)
            *(DWORD*)(ep + 6) = gIt->dwMugongID;
            ep[10] = gIt->bLevel;
            PACKET_HEADER* headE = (PACKET_HEADER*)endAck.data();
            headE->id = 0x402E; // CS_BT_KEEPUPMUGONGEND_ACK
            headE->payloadSize = 11;
            EncryptPacket(endAck.data(), 0x42);
            BroadcastPacketAOI_NoLock(gIt->wPosX, gIt->wPosY, endAck);

            gIt = m_groundEffects.erase(gIt);
            continue;
        }
        if (tick - gIt->dwLastTickTime >= gIt->dwTickInterval) {
            gIt->dwLastTickTime = tick;
            ProcessSingleGroundEffect(tick, *gIt);
        }
        ++gIt;
    }
}

void CMapInstance::ProcessSingleGroundEffect(DWORD tick, sGroundEffect& ge) {
    DWORD dotDmg = ge.dwSnapshotAtk * ge.wAtkPerc / 100;
    if (dotDmg < 1) dotDmg = 1;

    // 1. 搜索范围内所有怪物，结算毒雾伤害，并挂载中毒 Debuff
    std::vector<MonsterData*> nearbyMonsters = GetMonstersInAOI(ge.wPosX, ge.wPosY);
    for (MonsterData* pMon : nearbyMonsters) {
        if (!pMon || pMon->dwHpCur == 0 || IsGatherResource(pMon->bPropType)) continue;
        float dx = (float)pMon->wPosX - (float)ge.wPosX;
        float dy = (float)pMon->wPosY - (float)ge.wPosY;
        if (sqrtf(dx * dx + dy * dy) > ge.fRadius) continue;

        DWORD finalDmg = dotDmg;
        if (pMon->dwHpCur > finalDmg) {
            pMon->dwHpCur -= finalDmg;

            // 业务设计意图：给踩中地表毒雾的怪物挂上中毒 Buff（刷新到 4 秒），并广播 0x402C 同步包以在客户端渲染 Debuff 图标
            if (pMon->activeBuffs.count(ge.dwMugongID) == 0 || pMon->activeBuffs[ge.dwMugongID].dwEndTime < tick + 2000) {
                PlayerData::sActiveBuff debuff;
                debuff.dwMugongID = ge.dwMugongID;
                debuff.bLevel = ge.bLevel;
                debuff.dwEndTime = tick + 4000; // 只要处于毒中，每一跳伤害都将其刷新至 4 秒
                debuff.bIsDebuff = true;
                pMon->activeBuffs[ge.dwMugongID] = debuff;

                std::vector<BYTE> buffAck(4 + 11);
                BYTE* bp = buffAck.data() + 4;
                bp[0] = 0; // bResult
                *(DWORD*)(bp + 1) = pMon->dwObjectID;
                bp[5] = pMon->bObjectType; // 3 = Monster
                *(DWORD*)(bp + 6) = ge.dwMugongID;
                bp[10] = ge.bLevel;
                PACKET_HEADER* headB = (PACKET_HEADER*)buffAck.data();
                headB->id = 0x402C; // CS_BT_KEEPUPMUGONGSTART_ACK
                headB->payloadSize = 11;
                EncryptPacket(buffAck.data(), 0x42);
                BroadcastPacketAOI_NoLock(pMon->wPosX, pMon->wPosY, buffAck);
            }
        } else {
            finalDmg = pMon->dwHpCur;
            HandleMonsterDoTDeath(tick, *pMon, ge.dwCasterID, ge.dwMugongID, ge.bLevel);
        }

        // 广播 0x4016 伤害飘字与血条同步包给周围玩家
        std::vector<BYTE> dmgPkt(4 + 38, 0);
        BYTE* p = dmgPkt.data() + 4;
        p[0] = 0; // bResult
        *(DWORD*)(p + 1) = ge.dwMugongID;
        p[5] = ge.bLevel;
        p[6] = 1; // bAtkType (PC)
        *(DWORD*)(p + 7) = ge.dwCasterID;
        *(WORD*)(p + 11) = ge.wPosX;
        *(WORD*)(p + 13) = ge.wPosY;
        p[15] = 0; // bAtkHeight
        p[16] = pMon->bObjectType; // bDefType
        *(DWORD*)(p + 17) = pMon->dwObjectID;
        *(DWORD*)(p + 21) = pMon->dwHpMax;
        *(DWORD*)(p + 25) = pMon->dwHpCur;
        *(DWORD*)(p + 29) = finalDmg;
        *(DWORD*)(p + 33) = 0; // dwExp
        p[37] = 1; // bHitFlag
        PACKET_HEADER* dh = (PACKET_HEADER*)dmgPkt.data();
        dh->id = 0x4016; dh->payloadSize = 38;
        EncryptPacket(dmgPkt.data(), 0x42);
        BroadcastPacketAOI_NoLock(pMon->wPosX, pMon->wPosY, dmgPkt);
    }

    // 2. 搜索范围内所有玩家并执行碰撞、伤害及 PvP 中毒 Debuff 结算
    std::vector<PlayerData*> nearbyPlayers = GetPlayersInAOI(ge.wPosX, ge.wPosY);
    for (PlayerData* pPlayer : nearbyPlayers) {
        if (!pPlayer || pPlayer->dwHpCur == 0) continue;
        if (pPlayer->dwObjectID == ge.dwCasterID) continue; // 施法者本人免疫自己释放的毒雾

        float dx = (float)pPlayer->wPosX - (float)ge.wPosX;
        float dy = (float)pPlayer->wPosY - (float)ge.wPosY;
        if (sqrtf(dx * dx + dy * dy) > ge.fRadius) continue;

        DWORD finalDmg = dotDmg;
        if (pPlayer->dwHpCur > finalDmg) {
            pPlayer->dwHpCur -= finalDmg;

            // 业务设计意图：给踩中地表毒雾的玩家挂上中毒 Buff，并广播 0x402C
            if (pPlayer->activeBuffs.count(ge.dwMugongID) == 0 || pPlayer->activeBuffs[ge.dwMugongID].dwEndTime < tick + 2000) {
                PlayerData::sActiveBuff debuff;
                debuff.dwMugongID = ge.dwMugongID;
                debuff.bLevel = ge.bLevel;
                debuff.dwEndTime = tick + 4000; // 中毒状态持续 4 秒
                debuff.bIsDebuff = true;
                pPlayer->activeBuffs[ge.dwMugongID] = debuff;

                std::vector<BYTE> buffAck(4 + 11);
                BYTE* bp = buffAck.data() + 4;
                bp[0] = 0; // bResult
                *(DWORD*)(bp + 1) = pPlayer->dwObjectID;
                bp[5] = pPlayer->bObjectType; // 1 = PC
                *(DWORD*)(bp + 6) = ge.dwMugongID;
                bp[10] = ge.bLevel;
                PACKET_HEADER* headB = (PACKET_HEADER*)buffAck.data();
                headB->id = 0x402C; // CS_BT_KEEPUPMUGONGSTART_ACK
                headB->payloadSize = 11;
                EncryptPacket(buffAck.data(), 0x42);
                BroadcastPacketAOI_NoLock(pPlayer->wPosX, pPlayer->wPosY, buffAck);
            }
        } else {
            finalDmg = pPlayer->dwHpCur;
            pPlayer->dwHpCur = 0; // 玩家被毒雾毒死，常规生命心跳会将其引导至死亡复活点
        }

        // 广播 0x4016 伤害飘字同步包
        std::vector<BYTE> dmgPkt(4 + 38, 0);
        BYTE* p = dmgPkt.data() + 4;
        p[0] = 0; // bResult
        *(DWORD*)(p + 1) = ge.dwMugongID;
        p[5] = ge.bLevel;
        p[6] = 1; // bAtkType (PC)
        *(DWORD*)(p + 7) = ge.dwCasterID;
        *(WORD*)(p + 11) = ge.wPosX;
        *(WORD*)(p + 13) = ge.wPosY;
        p[15] = 0; // bAtkHeight
        p[16] = pPlayer->bObjectType; // 1 = PC
        *(DWORD*)(p + 17) = pPlayer->dwObjectID;
        *(DWORD*)(p + 21) = pPlayer->dwHpMax;
        *(DWORD*)(p + 25) = pPlayer->dwHpCur;
        *(DWORD*)(p + 29) = finalDmg;
        *(DWORD*)(p + 33) = 0;
        p[37] = 1; // bHitFlag
        PACKET_HEADER* dh = (PACKET_HEADER*)dmgPkt.data();
        dh->id = 0x4016; dh->payloadSize = 38;
        EncryptPacket(dmgPkt.data(), 0x42);
        BroadcastPacketAOI_NoLock(pPlayer->wPosX, pPlayer->wPosY, dmgPkt);

        // 同步 0x3B0D 属性包以直接更新受害玩家客户端本地的 HP 红色血条渲染
        DWORD targetCharID = pPlayer->dwObjectID - 400000000;
        SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(targetCharID);
        if (targetSock != INVALID_SOCKET) {
            std::vector<BYTE> hpBuf(4);
            auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back(d>>24); };
            push4(pPlayer->dwHpMax);
            push4(pPlayer->dwHpCur);
            push4(pPlayer->wIpMax);
            push4(pPlayer->wIpCur);
            hpBuf.push_back(0); // bType = 0 (silently update HP)
            PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
            hpHead->id = 0x3B0D; // CS_IF_CHARHP_ACK
            hpHead->payloadSize = hpBuf.size() - 4;
            EncryptPacket(hpBuf.data(), 0x42);
            SafeSend(targetSock, (const char*)hpBuf.data(), hpBuf.size(), 0);
        }
    }
}

void CMapInstance::AddGroundEffect(const sGroundEffect& effect) {
    m_groundEffects.push_back(effect);
    LOG("[GroundEffect] Added effect " + std::to_string(effect.dwMugongID)
        + " at (" + std::to_string(effect.wPosX) + "," + std::to_string(effect.wPosY) + ")"
        + " radius=" + std::to_string((int)effect.fRadius) + " dmg=" + std::to_string(effect.dwSnapshotAtk * effect.wAtkPerc / 100)
        + " duration=" + std::to_string((effect.dwEndTime - GetTickCount()) / 1000) + "s");
}

void CMapInstance::InterpolatePlayerPositions(DWORD tick) {
    bool shouldBroadcast = (tick - m_lastSyncBroadcast >= 350);
    if (shouldBroadcast) {
        m_pendingSyncs.clear();
        m_lastSyncBroadcast = tick;
    }
    
    for (auto& pair : m_players) {
        sServerObject& p = pair.second;
        if (!p.bIsMoving) continue;
        if (p.bObjectType != 1) continue;
        
        if (p.fPosX == 0.0f && p.fPosY == 0.0f) {
            p.fPosX = (float)p.wPosX;
            p.fPosY = (float)p.wPosY;
        }
        
        float dx = (float)p.wMoveDesX - p.fPosX;
        float dy = (float)p.wMoveDesY - p.fPosY;
        float dist = sqrtf(dx*dx + dy*dy);
        
        if (dist < 1.5f) {
            p.fPosX = (float)p.wMoveDesX; p.fPosY = (float)p.wMoveDesY;
            p.wPosX = p.wMoveDesX; p.wPosY = p.wMoveDesY;
            continue;
        }
        
        float speedScale = 1.0f;
        if (p.dwMapID == 2 || p.dwMapID == 10 || p.dwMapID == 16) {
            speedScale = 0.25f; // 业务意图：针对 4 倍缩放地图，将服务端插值步长修正为 0.25 倍，与客户端真实 3D 物理运动速度完全咬合
        }
        // 航位推测步长：保持 1:1 物理速度，彻底消除 0.65f 导致的服务端玩家残影滞后
        float speed = (float)(p.wWalkSpeed > 0 ? p.wWalkSpeed : 11) * 0.1f * speedScale;
        float nx = dx / dist, ny = dy / dist;
        p.fPosX += nx * speed;
        p.fPosY += ny * speed;
        
        WORD newX = (WORD)p.fPosX, newY = (WORD)p.fPosY;
        if (newX != p.wPosX || newY != p.wPosY) {
            int oldX = p.wPosX, oldY = p.wPosY;
            p.wPosX = newX; p.wPosY = newY;
            UpdatePlayerGrid(p.dwObjectID, oldX, oldY, p.wPosX, p.wPosY);
        }
        
        // Collect sync data for broadcasting (every 150ms)
        if (shouldBroadcast) {
            PendingSyncMove sm;
            sm.dwObjectID = p.dwObjectID;
            sm.wPosX = p.wPosX; sm.wPosY = p.wPosY; sm.bHeight = p.bHeight;
            sm.wDesX = p.wMoveDesX; sm.wDesY = p.wMoveDesY; sm.bDesH = p.bMoveDesH;
            sm.wDirection = p.wMoveDirection;
            sm.bSpeed = (p.wWalkSpeed & 0xFF) ? (p.wWalkSpeed & 0xFF) : 11;
            m_pendingSyncs.push_back(sm);
        }
    }
}


void CMapInstance::ProcessBuffs(DWORD tick) {
    // Buff expiry is now handled centrally in MonsterAI.cpp
    // which can safely call RecalculateStats() outside the map mutex.
}

bool CMapInstance::IsWalkable(int x, int y) const {
    if (m_collisionGrid.empty()) return true;
    if (x < 0 || y < 0 || x >= m_width || y >= m_height) return false;
    return m_collisionGrid[y * m_width + x] == 0;
}

bool CMapInstance::HasLineOfSight(int x0, int y0, int x1, int y1) const {
    if (m_collisionGrid.empty()) return true;
    int dx = std::abs(x1 - x0);
    int dy = std::abs(y1 - y0);
    int sx = (x0 < x1) ? 1 : -1;
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx - dy;

    int x = x0;
    int y = y0;
    while (x != x1 || y != y1) {
        if (!IsWalkable(x, y)) return false;
        int e2 = 2 * err;
        if (e2 > -dy) {
            err -= dy;
            x += sx;
        }
        if (e2 < dx) {
            err += dx;
            y += sy;
        }
    }
    return IsWalkable(x1, y1);
}

bool CMapInstance::FindPath(int startX, int startY, int goalX, int goalY, std::vector<std::pair<int, int>>& outPath, int maxRange) const {
    outPath.clear();
    if (startX == goalX && startY == goalY) {
        outPath.push_back({startX, startY});
        return true;
    }
    if (m_collisionGrid.empty()) {
        outPath.push_back({startX, startY});
        outPath.push_back({goalX, goalY});
        return true;
    }

    // 终点若不可行走，在周围 2 格内寻找最近的合法格子作为替代目标
    if (!IsWalkable(goalX, goalY)) {
        int bestGx = -1, bestGy = -1;
        int bestDistSq = 9999;
        for (int dy = -2; dy <= 2; ++dy) {
            for (int dx = -2; dx <= 2; ++dx) {
                int candX = goalX + dx;
                int candY = goalY + dy;
                if (IsWalkable(candX, candY)) {
                    int d = dx * dx + dy * dy;
                    if (d < bestDistSq) {
                        bestDistSq = d;
                        bestGx = candX;
                        bestGy = candY;
                    }
                }
            }
        }
        if (bestGx < 0) return false;
        goalX = bestGx;
        goalY = bestGy;
    }

    // 局部 A* 动态包围盒限制，避免全图遍历造成性能开销
    int minX = (std::max)(0, (std::min)(startX, goalX) - 12);
    int maxX = (std::min)(m_width - 1, (std::max)(startX, goalX) + 12);
    int minY = (std::max)(0, (std::min)(startY, goalY) - 12);
    int maxY = (std::min)(m_height - 1, (std::max)(startY, goalY) + 12);

    if ((maxX - minX) > maxRange * 2 || (maxY - minY) > maxRange * 2) {
        return false;
    }

    int boxW = maxX - minX + 1;
    int boxH = maxY - minY + 1;
    int totalCells = boxW * boxH;

    std::vector<int> gScore(totalCells, 100000000);
    std::vector<int> cameFrom(totalCells, -1);

    auto ToLocal = [&](int x, int y) {
        return (y - minY) * boxW + (x - minX);
    };

    struct QNode {
        int x, y;
        int fCost;
        bool operator>(const QNode& o) const { return fCost > o.fCost; }
    };
    std::priority_queue<QNode, std::vector<QNode>, std::greater<QNode>> pq;

    int startLocal = ToLocal(startX, startY);
    gScore[startLocal] = 0;
    int h0 = (std::abs(goalX - startX) + std::abs(goalY - startY)) * 10;
    pq.push({startX, startY, h0});

    static const int dirs[8][2] = {
        {0, 1}, {1, 0}, {0, -1}, {-1, 0},
        {1, 1}, {1, -1}, {-1, 1}, {-1, -1}
    };

    bool found = false;
    int expansions = 0;
    const int maxExpansions = 800;

    while (!pq.empty() && expansions < maxExpansions) {
        QNode cur = pq.top();
        pq.pop();
        expansions++;

        if (cur.x == goalX && cur.y == goalY) {
            found = true;
            break;
        }

        int curLocal = ToLocal(cur.x, cur.y);
        int curG = gScore[curLocal];

        for (int i = 0; i < 8; ++i) {
            int nx = cur.x + dirs[i][0];
            int ny = cur.y + dirs[i][1];

            if (nx < minX || nx > maxX || ny < minY || ny > maxY) continue;
            if (!IsWalkable(nx, ny)) continue;

            // 斜向移动防切死角
            if (dirs[i][0] != 0 && dirs[i][1] != 0) {
                if (!IsWalkable(cur.x + dirs[i][0], cur.y) || !IsWalkable(cur.x, cur.y + dirs[i][1])) {
                    continue;
                }
            }

            int stepCost = (dirs[i][0] != 0 && dirs[i][1] != 0) ? 14 : 10;
            int nextG = curG + stepCost;
            int nextLocal = ToLocal(nx, ny);

            if (nextG < gScore[nextLocal]) {
                gScore[nextLocal] = nextG;
                cameFrom[nextLocal] = curLocal;
                int h = (std::abs(goalX - nx) + std::abs(goalY - ny)) * 10;
                pq.push({nx, ny, nextG + h});
            }
        }
    }

    if (!found) return false;

    // 回溯生成整条路径
    int curr = ToLocal(goalX, goalY);
    while (curr != -1) {
        int ly = curr / boxW;
        int lx = curr % boxW;
        outPath.push_back({minX + lx, minY + ly});
        if (curr == startLocal) break;
        curr = cameFrom[curr];
    }
    std::reverse(outPath.begin(), outPath.end());
    return true;
}

int CMapInstance::FindFurthestVisibleWaypoint(int curX, int curY, const std::vector<std::pair<int, int>>& path) const {
    if (path.empty()) return -1;
    // 逆向回溯检测：从终点或前探 15 格向后逐一检测直通视线（String-Pulling 绳拉直平滑）
    int checkLimit = (std::min)((int)path.size() - 1, 15);
    for (int i = checkLimit; i >= 0; --i) {
        if (HasLineOfSight(curX, curY, path[i].first, path[i].second)) {
            return i;
        }
    }
    return 0;
}

void CMapInstance::ProcessMonsterAI(DWORD tick, MonsterData& obj) {
    if (obj.bObjectType != 3) return; // Monster only
    if (IsGatherResource(obj.bPropType)) return; // 采集资源静态实体，不跑AI，不移动不攻击
    sNpcTemplate& tpl = g_NpcTemplates[obj.bPropType];
    if (tpl.dwHpInit == 0) return; // Missing template

    if (obj.dwHpCur == 0) {
        // Respawn Logic
        if (obj.dwDeadTime > 0) {
            if (obj.wPosX != 0 && tick - obj.dwDeadTime >= 3000) {
                // Send MAPLEAVE to despawn corpse after 3 seconds
                // Client expects: bResult(1) + dwObjectID(4) + bObjectType(1) + dwMapID(4) + bType(1)
                std::vector<BYTE> leaveBuf; leaveBuf.resize(4);
                leaveBuf.push_back(0); // bResult = 0 (success)
                DWORD oid = obj.dwObjectID; 
                leaveBuf.push_back(oid&0xFF); leaveBuf.push_back((oid>>8)&0xFF); leaveBuf.push_back((oid>>16)&0xFF); leaveBuf.push_back(oid>>24);
                leaveBuf.push_back(obj.bObjectType); // bObjectType (3 = NPC)
                DWORD mid = obj.dwMapID;
                leaveBuf.push_back(mid&0xFF); leaveBuf.push_back((mid>>8)&0xFF); leaveBuf.push_back((mid>>16)&0xFF); leaveBuf.push_back(mid>>24);
                leaveBuf.push_back(0); // bType = 0 (normal leave)
                PACKET_HEADER* leaveHead = (PACKET_HEADER*)leaveBuf.data(); leaveHead->id = 0x3506; leaveHead->payloadSize = leaveBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(leaveBuf.data(), 0x42);
                BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, leaveBuf); // Changed to AOI
                // 尸体清除：坐标清零前必须同步 Grid，否则怪物在空间索引中的位置会与实际坐标永久脱节
                int corpseOldX = obj.wPosX, corpseOldY = obj.wPosY;
                obj.wPosX = 0;
                UpdateMonsterGrid(obj.dwObjectID, corpseOldX, corpseOldY, obj.wPosX, corpseOldY);
            }
            
            DWORD regenTimeMs = tpl.wRegen * 1000;
            if (regenTimeMs == 0) regenTimeMs = 10000; // Fallback
            
            if (tick - obj.dwDeadTime >= regenTimeMs) {
                obj.dwHpCur = obj.dwHpMax;
                // 业务设计意图：重置/初始化怪物的内功 (IP) 上限与当前值。
                // 怪物的最大内力设为其最大生命值的一半（兜底最少 1000），用以配合本地与服务端的扣蓝结算。
                obj.wIpMax = obj.dwHpMax / 2;
                if (obj.wIpMax < 1000) obj.wIpMax = 1000;
                obj.wIpCur = obj.wIpMax;

                obj.dwTargetID = 0;
                obj.dwDeadTime = 0;
                obj.activeBuffs.clear(); // 复活时显式清理所有残留的 Buff/Debuff 状态
                
                // Keep the same ObjectID on respawn.
                // m_monsters uses dwObjectID as the map key — changing it without
                // re-inserting would break GetMonster() lookups, making the monster invincible.
                
                int offsetX = 0, offsetY = 0;
                if (obj.wSpawnRange > 0) {
                    offsetX = (rand() % (obj.wSpawnRange * 2 + 1)) - obj.wSpawnRange;
                    offsetY = (rand() % (obj.wSpawnRange * 2 + 1)) - obj.wSpawnRange;
                }
                // 复活：记住旧 Grid 坐标（尸体阶段 wPosX 已被设为 0），设新坐标后同步 Grid
                int respawnOldX = obj.wPosX, respawnOldY = obj.wPosY;
                obj.wPosX = obj.wSpawnX + offsetX;
                if (obj.wPosX < 0) obj.wPosX = 0;
                obj.wPosY = obj.wSpawnY + offsetY;
                if (obj.wPosY < 0) obj.wPosY = 0;
                obj.fPosX = (float)obj.wPosX;
                obj.fPosY = (float)obj.wPosY;
                UpdateMonsterGrid(obj.dwObjectID, respawnOldX, respawnOldY, obj.wPosX, obj.wPosY);
                
                // Broadcast NPCINFO
                {
                    DWORD oid = obj.dwObjectID;
                    std::vector<BYTE> infoBuf; infoBuf.reserve(80);
                    infoBuf.push_back(0);                                               
                    infoBuf.push_back(oid&0xFF); infoBuf.push_back((oid>>8)&0xFF); infoBuf.push_back((oid>>16)&0xFF); infoBuf.push_back(oid>>24); 
                    infoBuf.push_back(obj.bPropType);                                   
                    DWORD mid = obj.dwMapID; infoBuf.push_back(mid&0xFF); infoBuf.push_back((mid>>8)&0xFF); infoBuf.push_back((mid>>16)&0xFF); infoBuf.push_back(mid>>24); 
                    infoBuf.push_back(obj.wPosX & 0xFF); infoBuf.push_back(obj.wPosX >> 8); 
                    WORD wl = obj.wLevel; infoBuf.push_back(wl&0xFF); infoBuf.push_back(wl>>8); 
                    infoBuf.push_back(obj.wPosY & 0xFF); infoBuf.push_back(obj.wPosY >> 8); 
                    infoBuf.push_back(obj.bHeight);                                     
                    infoBuf.push_back(0); infoBuf.push_back(0);                         
                    WORD nlen = (WORD)obj.szName.length(); infoBuf.push_back(nlen&0xFF); infoBuf.push_back(nlen>>8);
                    for (char c : obj.szName) infoBuf.push_back((BYTE)c);               
                    infoBuf.push_back(1); // NPCSTATUS_INIT                                              
                    infoBuf.push_back(obj.wPosX & 0xFF); infoBuf.push_back(obj.wPosX >> 8); 
                    infoBuf.push_back(obj.wPosY & 0xFF); infoBuf.push_back(obj.wPosY >> 8); 
                    infoBuf.push_back(obj.bHeight);                                     
                    DWORD hm = obj.dwHpMax; infoBuf.push_back(hm&0xFF); infoBuf.push_back((hm>>8)&0xFF); infoBuf.push_back((hm>>16)&0xFF); infoBuf.push_back(hm>>24); 
                    DWORD hc = obj.dwHpCur; infoBuf.push_back(hc&0xFF); infoBuf.push_back((hc>>8)&0xFF); infoBuf.push_back((hc>>16)&0xFF); infoBuf.push_back(hc>>24); 
                    infoBuf.push_back(obj.bWalkSpeedByte);   
                    infoBuf.push_back(obj.bGroupOrder);   
                    infoBuf.push_back(255); 
                    
                    std::vector<BYTE> infoFull; infoFull.resize(4); infoFull.insert(infoFull.end(), infoBuf.begin(), infoBuf.end());
                    PACKET_HEADER* infoHead = (PACKET_HEADER*)infoFull.data(); infoHead->id = 0x352C; infoHead->payloadSize = (WORD)infoBuf.size();
                    EncryptPacket(infoFull.data(), 0x42);
                    BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, infoFull); // Changed to AOI
                }
            }
        }
        return;
    }

    // -----------------------------------------------------
    // 1. 全量 Buff 状态刷新与 DoT 更新（一次性提到 AI 最头部，解决判定时序问题）
    // -----------------------------------------------------
    bool isFeared = false;
    bool isBlinded = false;
    bool isParalyzed = false;
    bool isRooted = false;
    for (auto it = obj.activeBuffs.begin(); it != obj.activeBuffs.end(); ) {
        if (tick > it->second.dwEndTime) {
            // 广播 0x402E 清除 Debuff 图标
            std::vector<BYTE> endAck(4 + 11);
            BYTE* ep = endAck.data() + 4;
            ep[0] = 0; // bResult
            *(DWORD*)(ep + 1) = obj.dwObjectID;
            ep[5] = obj.bObjectType; // 3 = Monster/NPC
            *(DWORD*)(ep + 6) = it->second.dwMugongID;
            ep[10] = it->second.bLevel;
            PACKET_HEADER* headE = (PACKET_HEADER*)endAck.data();
            headE->id = 0x402E; // CS_BT_KEEPUPMUGONGEND_ACK
            headE->payloadSize = 11;
            EncryptPacket(endAck.data(), 0x42);
            BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, endAck);

            LOG("[BuffExpiry] Debuff " + std::to_string(it->second.dwMugongID) + " expired on monster " + std::to_string(obj.dwObjectID));
            it = obj.activeBuffs.erase(it);
        } else {
            sMugongTemplate* bfTpl = MugongManager::GetInstance()->GetTemplate(it->second.dwMugongID);
            if (bfTpl && bfTpl->bType == 4) {
                if (bfTpl->bKind == 8)  isFeared = true;
                if (bfTpl->bKind == 16) isBlinded = true;
                if (bfTpl->bKind == 17) isParalyzed = true;
                if (bfTpl->bKind == 18) isRooted = true;

                // DoT (bKind=19 化骨功/毒烟神功)：每 nEtc2 毫秒扣 nEtc1 固定HP
                bool hasDmg = false;
                DWORD dotDmg = 0;
                if (bfTpl->bKind == 19 && it->second.bIsDebuff) {
                    sMugongList* dotData = MugongManager::GetInstance()->GetMugongLevelData(it->second.dwMugongID, it->second.bLevel);
                    if (dotData) {
                        DWORD tickInterval = (dotData->nEtc2 > 0) ? dotData->nEtc2 : 1000;
                        if (tick - it->second.dwLastTickTime >= tickInterval) {
                            it->second.dwLastTickTime = tick;
                            dotDmg = (dotData->nEtc1 > 0) ? dotData->nEtc1 : 1;
                            hasDmg = true;
                        }
                    }
                }

                // DoT (bKind=21 五毒针)：每秒扣 施放者攻击 × wIncAtkPerc / 100
                if (bfTpl->bKind == 21 && it->second.bIsDebuff) {
                    sMugongList* dotData = MugongManager::GetInstance()->GetMugongLevelData(it->second.dwMugongID, it->second.bLevel);
                    if (dotData) {
                        DWORD tickInterval = 1000; // 固定1秒tick
                        if (tick - it->second.dwLastTickTime >= tickInterval) {
                            it->second.dwLastTickTime = tick;
                            dotDmg = it->second.dwSnapshotAtk * dotData->wIncAtkPerc / 100;
                            if (dotDmg < 1) dotDmg = 1;
                            hasDmg = true;
                        }
                    }
                }

                // DoT (bKind=22 化功术/持续减蓝)：每秒扣 nEtc1 固定IP
                if (bfTpl->bKind == 22 && it->second.bIsDebuff) {
                    sMugongList* dotData = MugongManager::GetInstance()->GetMugongLevelData(it->second.dwMugongID, it->second.bLevel);
                    if (dotData) {
                        DWORD tickInterval = 1000; // 固定1秒tick
                        if (tick - it->second.dwLastTickTime >= tickInterval) {
                            it->second.dwLastTickTime = tick;
                            DWORD drain = (dotData->nEtc1 > 0) ? dotData->nEtc1 : 1;
                            if (obj.wIpCur > drain) {
                                obj.wIpCur -= drain;
                            } else {
                                obj.wIpCur = 0;
                            }
                            LOG("[MpDot-Tick] Monster " + std::to_string(obj.dwObjectID) + " drained " + std::to_string(drain) + " IP, current IP: " + std::to_string(obj.wIpCur));
                        }
                    }
                }

                if (hasDmg && dotDmg > 0) {
                    if (obj.dwHpCur > dotDmg) {
                        obj.dwHpCur -= dotDmg;
                    } else {
                        dotDmg = obj.dwHpCur;
                        HandleMonsterDoTDeath(tick, obj, it->second.dwCasterID, it->second.dwMugongID, it->second.bLevel);
                    }

                    // 广播 0x4016 伤害与血量同步包给客户端，让其显示伤害飘字并更新血条
                    std::vector<BYTE> dmgAckBuf(4 + 38, 0);
                    BYTE* sp = dmgAckBuf.data() + 4;
                    sp[0] = 2; // 2 = 命中 (Hit)
                    *(DWORD*)(sp + 1) = it->second.dwMugongID;
                    sp[5] = it->second.bLevel;

                    // 业务设计意图：客户端在处理 0x4016 伤害包时，如果找不到包中指定的攻击者对象(pAttacker == NULL)，会直接丢弃该包退出，
                    // 导致怪物的当前血量无法在客户端内存中更新为 0 (表现为满血倒地且无法清除选中和名字)。
                    // 因此，在此处引入防御性安全后备机制：如果施法者 ID 为 0 (或攻击者不存在)，则将攻击者强制设为怪物自己(类型为 3)，
                    // 确保客户端 100% 能够成功在场找到攻击者并解析更新 HP 状态。
                    DWORD casterID = it->second.dwCasterID;
                    BYTE casterType = 1; // 1 = Player
                    if (casterID == 0) {
                        casterID = obj.dwObjectID;
                        casterType = 3; // 3 = Monster
                    }
                    sp[6] = casterType; 
                    *(DWORD*)(sp + 7) = casterID;

                    *(WORD*)(sp + 11) = obj.wPosX;
                    *(WORD*)(sp + 13) = obj.wPosY;
                    sp[15] = 0;
                    sp[16] = 3; // 3 = Monster/NPC
                    *(DWORD*)(sp + 17) = obj.dwObjectID;
                    *(DWORD*)(sp + 21) = obj.dwHpMax;
                    *(DWORD*)(sp + 25) = obj.dwHpCur;
                    *(DWORD*)(sp + 29) = dotDmg;
                    *(DWORD*)(sp + 33) = 0; 
                    sp[37] = 0; // 不暴击

                    PACKET_HEADER* sHead = (PACKET_HEADER*)dmgAckBuf.data();
                    sHead->id = 0x4016;
                    sHead->payloadSize = 38;
                    EncryptPacket(dmgAckBuf.data(), 0x42);
                    BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, dmgAckBuf);

                    LOG("[DoT-Tick] Skill " + std::to_string(it->second.dwMugongID) + " dealt " + std::to_string(dotDmg) + " dmg to monster " + std::to_string(obj.dwObjectID) + ", HP: " + std::to_string(obj.dwHpCur));

                    // 死亡结算已在 HandleMonsterDoTDeath 中统一处理，此处无需重复
                }
            }
            ++it;
        }
    }

    // 经典硬控（定身/冰冻）判定：Xiah 经典控制类技能 94/95/35/65/125
    bool isCCDebuff = false;
    for (auto& bf : obj.activeBuffs) {
        if (bf.second.bIsDebuff) {
            DWORD mugID = bf.second.dwMugongID;
            if (mugID == 94 || mugID == 35 || mugID == 65) {
                isCCDebuff = true;
                break;
            }
        }
    }

    // -----------------------------------------------------
    // 2. 统一移动受限行动拦截（定身、麻痹、经典冰冻等）
    // -----------------------------------------------------
    bool isMovementBlocked = isCCDebuff || isParalyzed || isRooted;
    if (isMovementBlocked) {
        if (obj.wLastSentDestX != 0 || obj.wLastSentDestY != 0) {
            obj.wLastSentDestX = 0;
            obj.wLastSentDestY = 0;
            obj.wLastSentPosX = 0;
            obj.wLastSentPosY = 0;
            // 业务设计意图：向 AOI 广播怪物停止包，使客户端显示怪物立即定身/打断跑路滑行，解决控制技能生效延迟的现象。
            std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0); 
            DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
            stopBuf.push_back(obj.bObjectType); 
            stopBuf.push_back(obj.wPosX & 0xFF); stopBuf.push_back(obj.wPosX >> 8);
            stopBuf.push_back(obj.wPosY & 0xFF); stopBuf.push_back(obj.wPosY >> 8);
            stopBuf.push_back(obj.bHeight);
            stopBuf.push_back(0); 
            
            PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
            EncryptPacket(stopBuf.data(), 0x42); 
            BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, stopBuf);
        }

        // 麻痹 (isParalyzed) 或经典硬控 (isCCDebuff) 状态下不可攻击：直接拦截并跳过本轮AI决策，实现完全控制。
        if (isParalyzed || isCCDebuff) {
            return;
        }
        // 普通定身 (isRooted) 状态下仅不可移动，但如果在射程内仍可攻击，故此处不return，允许落入下文原地攻击逻辑。
    }
    
    // HP Regeneration
    if (obj.dwHpCur < obj.dwHpMax && tpl.wHealPoint > 0) {
        if (tick - obj.dwLastHealTime >= 5000) {
            obj.dwHpCur += tpl.wHealPoint;
            if (obj.dwHpCur > obj.dwHpMax) obj.dwHpCur = obj.dwHpMax;
            obj.dwLastHealTime = tick;
        }
    }
    
    // (MonsterAI per-tick debug log removed to reduce log spam)
    
    if (obj.dwAttackPattern == 0) {
        obj.dwTargetID = 0;
    }
    
    DWORD targetId = obj.dwTargetID; 
    float minDist = 99999.0f;
    PlayerData* bestPlayer = nullptr;

    // FIND TARGET (Skip entirely if monster is currently leashed and returning to spawn)
    if (obj.bIsReturning) {
        targetId = 0;
        obj.dwTargetID = 0;
    }

    // 恐惧/致盲影响处理：已在心跳头部完成状态与DoT更新，此处直接执行脱战游荡
    if (isFeared || isBlinded) {
        targetId = 0;
        obj.dwTargetID = 0;
    }

    bestPlayer = nullptr;
    targetId = obj.dwTargetID;

    // 1. 如果怪物已有仇恨目标（正在追击中）：优先直接在地图全局玩家列表中追踪该目标
    // 坚决不使用局部 AOI 九宫格，彻底消灭因玩家跑出 100 格网格边界导致的“追击中途被误拉回”
    if (targetId != 0 && !obj.bIsReturning && !isFeared && !isBlinded) {
        auto it = m_players.find(targetId);
        if (it != m_players.end()) {
            PlayerData& p = it->second;
            if (p.dwHpCur > 0 && p.dwInvulnerableUntil <= tick &&
                p.activeBuffs.count(130) == 0 && p.activeBuffs.count(178) == 0) 
            {
                float dx = (float)p.wPosX - (float)obj.wPosX;
                float dy = (float)p.wPosY - (float)obj.wPosY;
                float curDist = std::sqrt(dx * dx + dy * dy);

                // 仇恨脱离距离（Chase Range）：只要玩家未跑离怪物 80 格（或视野*3.5）开外，仇恨绝不丢失
                float maxChaseRange = (float)obj.wSightRange * 3.5f;
                if (maxChaseRange < 80.0f) maxChaseRange = 80.0f;
                if (maxChaseRange > 120.0f) maxChaseRange = 120.0f;

                if (curDist <= maxChaseRange) {
                    bestPlayer = &p;
                }
            }
        }
    }

    // 2. 如果怪物当前无仇恨目标（空闲巡逻状态），才在当前 AOI 网格内扫描进入视野的敌对玩家
    if (!bestPlayer && !obj.bIsReturning && !isFeared && !isBlinded && ((obj.dwAttackPattern & 1) != 0)) {
        std::vector<PlayerData*> nearbyPlayers = GetPlayersInAOI(obj.wPosX, obj.wPosY);
        for (PlayerData* playerPtr : nearbyPlayers) {
            if (!playerPtr) continue;
            PlayerData& player = *playerPtr;
            if (player.dwHpCur == 0) continue;
            if (player.dwInvulnerableUntil > tick) continue; // Death/respawn protection
            if (player.activeBuffs.count(130) > 0) continue; // Ignore players under Turtle Breath (130)
            if (player.activeBuffs.count(178) > 0) continue; // Ignore players under Stealth (178)

            float dx = (float)player.wPosX - (float)obj.wPosX;
            float dy = (float)player.wPosY - (float)obj.wPosY;
            float distSq = (dx*dx + dy*dy);
            float maxRangeSq = (float)(obj.wSightRange * obj.wSightRange);
            
            if (distSq < maxRangeSq && distSq < (minDist * minDist)) {
                minDist = std::sqrt(distSq);
                targetId = player.dwObjectID;
                bestPlayer = &player;
            }
        }
    }

    // DEBUG: Log distance when monster has aggro but can't find target
    if (obj.dwTargetID != 0 && bestPlayer == nullptr) {
        for (auto& pair : m_players) {
            if (pair.second.dwObjectID == obj.dwTargetID) {
                float dx = (float)pair.second.wPosX - (float)obj.wPosX;
                float dy = (float)pair.second.wPosY - (float)obj.wPosY;
                float dist = std::sqrt(dx*dx + dy*dy);
                char dbg[256]; sprintf(dbg, "[MonsterAI] AGGRO LOST: monster(%d,%d) target(%d,%d) dist=%.0f sight=%d chaseRange=%.0f hp=%u",
                    obj.wPosX, obj.wPosY, pair.second.wPosX, pair.second.wPosY, dist, obj.wSightRange, 
                    (obj.wSightRange * 3.0f > 100.0f ? obj.wSightRange * 3.0f : 100.0f), pair.second.dwHpCur);
                LOG(std::string(dbg));
                break;
            }
        }
    }

    if (bestPlayer == nullptr) {
        if (targetId != 0 || obj.dwTargetID != 0) {
            float distToSpawn = std::sqrt(std::pow((float)obj.wPosX - obj.wSpawnX, 2) + std::pow((float)obj.wPosY - obj.wSpawnY, 2));
            if (distToSpawn > 1.0f) {
                obj.bIsReturning = true;
                obj.dwReturnStartTime = tick;
                obj.bInAttackRange = false;
                obj.fPosX = (float)obj.wPosX;
                obj.fPosY = (float)obj.wPosY;
                obj.wDestX = obj.wSpawnX;
                obj.wDestY = obj.wSpawnY;
                obj.wLastSentDestX = 0;
                obj.wLastSentDestY = 0;
                obj.wLastSentPosX = 0;
                obj.wLastSentPosY = 0;
                obj.dwHpCur = obj.dwHpMax;

                // 脱战刹车站定：广播停步包，使客户端模型自然刹车结束追击奔跑转为待机
                std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0);
                DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
                stopBuf.push_back(obj.bObjectType); 
                stopBuf.push_back(obj.wPosX & 0xFF); stopBuf.push_back(obj.wPosX >> 8);
                stopBuf.push_back(obj.wPosY & 0xFF); stopBuf.push_back(obj.wPosY >> 8);
                stopBuf.push_back(obj.bHeight);
                stopBuf.push_back(0); 
                PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(stopBuf.data(), 0x42); 
                BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, stopBuf);

                LOG("[MonsterAI] Target lost/dead. Trigger return: ObjID=" + std::to_string(obj.dwObjectID) + " returning to spawn (" + std::to_string(obj.wSpawnX) + "," + std::to_string(obj.wSpawnY) + ")");
            }
        }
        targetId = 0;
        obj.dwTargetID = 0;
        obj.dwChaseStartTime = 0;
    }

    // Chase Leash Check: if pulled too far from spawn point, break aggro and return
    if (targetId && bestPlayer) {
        if (obj.dwTargetID != targetId) {
            // Target acquired/switched: reset previous wander/move destination
            obj.wLastSentDestX = 0;
            obj.wLastSentDestY = 0;
            obj.wDestX = 0;
            obj.wDestY = 0;
            obj.dwChaseStartTime = tick; // 锁定新目标，记录追击起跑时刻
            obj.dwLastAttackTime = 0;    // 进入战斗，起手第一刀蓄力就绪
        }

        float distToSpawn = std::sqrt(std::pow((float)obj.wPosX - obj.wSpawnX, 2) + std::pow((float)obj.wPosY - obj.wSpawnY, 2));
        float leashRange = (float)(obj.wWanderRange * 6);
        if (leashRange < 180.0f) leashRange = 180.0f; // 保底拉脱距离提升至 180 格，提供充裕引怪与拉扯空间
        if (leashRange > 250.0f) leashRange = 250.0f;
        
        if (distToSpawn > leashRange) {
            char dbgLeash[256];
            sprintf(dbgLeash, "[MonsterAI] LEASH: ObjID=%u pulled too far (dist=%.1f spawn=%u,%u leash=%.1f). Resetting aggro.",
                obj.dwObjectID, distToSpawn, obj.wSpawnX, obj.wSpawnY, leashRange);
            LOG(std::string(dbgLeash));
            
            targetId = 0;
            obj.dwTargetID = 0;
            obj.dwChaseStartTime = 0;
            bestPlayer = nullptr;
            obj.bIsReturning = true; // Enter returning state
            obj.dwReturnStartTime = tick;
            obj.bInAttackRange = false;
            obj.dwHpCur = obj.dwHpMax; // Heal to full immediately
            
            // Force destination to spawn point and reset move tracking
            obj.wDestX = obj.wSpawnX;
            obj.wDestY = obj.wSpawnY;
            obj.wLastSentDestX = 0;
            obj.wLastSentDestY = 0;
            obj.wLastSentPosX = 0;
            obj.wLastSentPosY = 0;
            obj.fPosX = (float)obj.wPosX;
            obj.fPosY = (float)obj.wPosY;

            // 脱战刹车站定：广播停步包，使客户端模型自然刹车结束追击奔跑转为待机
            std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0);
            DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
            stopBuf.push_back(obj.bObjectType); 
            stopBuf.push_back(obj.wPosX & 0xFF); stopBuf.push_back(obj.wPosX >> 8);
            stopBuf.push_back(obj.wPosY & 0xFF); stopBuf.push_back(obj.wPosY >> 8);
            stopBuf.push_back(obj.bHeight);
            stopBuf.push_back(0); 
            PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
            EncryptPacket(stopBuf.data(), 0x42); 
            BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, stopBuf);
        }
    }

    WORD oldX = obj.wPosX;
    WORD oldY = obj.wPosY;

    if (targetId && bestPlayer) {
        obj.dwTargetID = targetId;
        // 修复1：普通物理近战肉搏射程（严禁使用 wShotAtkRange，防止远程配置使怪物在数十格外提前刹车站定）
        float atkRange = 2.5f;
        if (obj.wMeleeAtkRange > 0) {
            atkRange = (float)obj.wMeleeAtkRange;
            if (atkRange > 3.2f) atkRange = 3.2f; // 近战普通肉搏上限封顶 3.2 格，杜绝隔空撕咬
            if (atkRange < 2.0f) atkRange = 2.0f; // 保底 2.0 格，避免穿模贴脸
        }

        // 攻击范围迟滞判断（Hysteresis），防止在攻击临界距离高频反复起步/停步造成模型抽搐
        float enterRange = atkRange;
        float exitRange = atkRange + 1.0f;

        float targetX = (float)bestPlayer->wPosX;
        float targetY = (float)bestPlayer->wPosY;
        int iTargetX = (int)(targetX + 0.5f);
        int iTargetY = (int)(targetY + 0.5f);
        float tdx = targetX - (float)obj.wPosX;
        float tdy = targetY - (float)obj.wPosY;
        float curDist = std::sqrt(tdx * tdx + tdy * tdy);

        // 视线判定：近战贴身距离（<=2.5格或<=攻击距离）直接视为通畅，防止地表微小障碍（如摊位、桌角）导致近身发呆
        bool hasDirectLOS = (curDist <= 2.5f || curDist <= atkRange) ? true : HasLineOfSight(obj.wPosX, obj.wPosY, iTargetX, iTargetY);

        if (!obj.bInAttackRange) {
            if (curDist <= enterRange && hasDirectLOS) {
                obj.bInAttackRange = true;
                
                // 修复2：冲入射程瞬间无条件出刀！重置攻击CD为0，保证冲到面前同tick立即斩出第一刀，杜绝任何发呆
                obj.dwLastAttackTime = 0;
                LOG("[MonsterAI] RUSH ATTACK: Mon=" + std::to_string(obj.dwObjectID) + " rushed into range (dist=" + std::to_string(curDist) + "). Immediate first strike ready!");
                obj.dwChaseStartTime = 0;
            }
        } else {
            if (curDist > exitRange || !hasDirectLOS) {
                obj.bInAttackRange = false;
                // 玩家跑开拉开距离，重新记录追击起跑时刻
                obj.dwChaseStartTime = tick;
            }
        }

        // 进身攻击调试日志：距离 <= 10 格时限频监控状态
        if (curDist <= 10.0f && (tick % 1000 < 100)) {
            char dbgBuf[256];
            sprintf(dbgBuf, "[AtkDebug] Mon=%u pos=(%d,%d) P=(%d,%d) dist=%.2f atkRange=%.2f inRange=%d waitTime=%d cd=%u hasLOS=%d",
                obj.dwObjectID, obj.wPosX, obj.wPosY, bestPlayer->wPosX, bestPlayer->wPosY,
                curDist, atkRange, (int)obj.bInAttackRange, 
                (int)(tick - obj.dwLastAttackTime), tpl.wAtkInterval, (int)hasDirectLOS);
            LOG(std::string(dbgBuf));
        }

        if (obj.bInAttackRange) {
            // Guard: don't attack dead or invulnerable players
            if (bestPlayer->dwHpCur == 0 || bestPlayer->dwInvulnerableUntil > tick) {
                obj.dwTargetID = 0;
                obj.bInAttackRange = false;
                float dist = std::sqrt(std::pow((float)obj.wPosX - obj.wSpawnX, 2) + std::pow((float)obj.wPosY - obj.wSpawnY, 2));
                if (dist > 1.0f) {
                    obj.bIsReturning = true;
                    obj.dwReturnStartTime = tick;
                    obj.fPosX = (float)obj.wPosX;
                    obj.fPosY = (float)obj.wPosY;
                    obj.wDestX = obj.wSpawnX;
                    obj.wDestY = obj.wSpawnY;
                    obj.wLastSentDestX = 0;
                    obj.wLastSentDestY = 0;
                    obj.wLastSentPosX = 0;
                    obj.wLastSentPosY = 0;
                    obj.dwHpCur = obj.dwHpMax;

                    // 脱战刹车站定：广播停步包，使客户端模型自然刹车结束追击奔跑转为待机
                    std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0);
                    DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
                    stopBuf.push_back(obj.bObjectType); 
                    stopBuf.push_back(obj.wPosX & 0xFF); stopBuf.push_back(obj.wPosX >> 8);
                    stopBuf.push_back(obj.wPosY & 0xFF); stopBuf.push_back(obj.wPosY >> 8);
                    stopBuf.push_back(obj.bHeight);
                    stopBuf.push_back(0); 
                    PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
                    EncryptPacket(stopBuf.data(), 0x42); 
                    BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, stopBuf);

                    LOG("[MonsterAI] Target dead/invulnerable in attack range. Trigger return: ObjID=" + std::to_string(obj.dwObjectID));
                }
            } else {
            if (obj.wLastSentDestX != 0 || obj.wLastSentDestY != 0) {
                // 怪物冲锋到达攻击距离刹车站定：保持当前真实模拟物理坐标，不跳越至陈旧预判目的地
                obj.wLastSentDestX = 0;
                obj.wLastSentDestY = 0;
                obj.wLastSentPosX = 0;
                obj.wLastSentPosY = 0;
                obj.wDestX = 0;
                obj.wDestY = 0;
                
                // 停步时立即调整朝向面对玩家
                float fdx = targetX - (float)obj.wPosX;
                float fdy = targetY - (float)obj.wPosY;
                float fAngle = std::atan2(fdy, fdx) * 180.0f / 3.14159265f;
                if (fAngle < 0) fAngle += 360.0f;
                WORD wFaceDir = (WORD)fAngle;
                obj.wLastSentDirection = wFaceDir;

                std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0); 
                DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
                stopBuf.push_back(obj.bObjectType); 
                stopBuf.push_back(obj.wPosX & 0xFF); stopBuf.push_back(obj.wPosX >> 8);
                stopBuf.push_back(obj.wPosY & 0xFF); stopBuf.push_back(obj.wPosY >> 8);
                stopBuf.push_back(obj.bHeight);
                stopBuf.push_back(0); 
                
                PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(stopBuf.data(), 0x42); 
                BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, stopBuf);
            }
            
            DWORD atkCooldown = (tpl.wAtkInterval > 0) ? tpl.wAtkInterval : 1500;
            if (tick - obj.dwLastAttackTime >= atkCooldown) { 
                obj.dwLastAttackTime = tick;
                DWORD oid = obj.dwObjectID;
                LOG("[AtkDebug] TRIGGER ATTACK! Monster=" + std::to_string(oid) + " Target=" + std::to_string(targetId) + " at (" + std::to_string(obj.wPosX) + "," + std::to_string(obj.wPosY) + ")");

                // 攻击前确保朝向面对玩家
                float fdx = targetX - (float)obj.wPosX;
                float fdy = targetY - (float)obj.wPosY;
                float fAngle = std::atan2(fdy, fdx) * 180.0f / 3.14159265f;
                if (fAngle < 0) fAngle += 360.0f;
                WORD wFaceDir = (WORD)fAngle;
                obj.wLastSentDirection = wFaceDir;
                
                // 1. PREATTACK_ACK
                std::vector<BYTE> preBuf; preBuf.reserve(32);
                preBuf.push_back(obj.bObjectType);
                preBuf.push_back(oid&0xFF); preBuf.push_back((oid>>8)&0xFF); preBuf.push_back((oid>>16)&0xFF); preBuf.push_back(oid>>24);
                preBuf.push_back(obj.wPosX & 0xFF); preBuf.push_back(obj.wPosX >> 8);
                preBuf.push_back(obj.wPosY & 0xFF); preBuf.push_back(obj.wPosY >> 8);
                preBuf.push_back(obj.bHeight);
                preBuf.push_back(1); 
                preBuf.push_back(targetId&0xFF); preBuf.push_back((targetId>>8)&0xFF); preBuf.push_back((targetId>>16)&0xFF); preBuf.push_back(targetId>>24);
                preBuf.push_back(0); 
                std::vector<BYTE> preFull; preFull.resize(4); preFull.insert(preFull.end(), preBuf.begin(), preBuf.end());
                PACKET_HEADER* preHead = (PACKET_HEADER*)preFull.data(); preHead->id = 0x4004; preHead->payloadSize = preBuf.size();
                EncryptPacket(preFull.data(), 0x42); 
                BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, preFull);
                
                // 2. ATTACK_ACK 
                std::vector<BYTE> ackBuf; ackBuf.reserve(64);
                ackBuf.push_back(0); // placeholder for bResult, will be overwritten
                ackBuf.push_back(obj.bObjectType);
                ackBuf.push_back(oid&0xFF); ackBuf.push_back((oid>>8)&0xFF); ackBuf.push_back((oid>>16)&0xFF); ackBuf.push_back(oid>>24);
                ackBuf.push_back(obj.wPosX & 0xFF); ackBuf.push_back(obj.wPosX >> 8);
                ackBuf.push_back(obj.wPosY & 0xFF); ackBuf.push_back(obj.wPosY >> 8);
                ackBuf.push_back(obj.bHeight);
                ackBuf.push_back(1); 
                ackBuf.push_back(targetId&0xFF); ackBuf.push_back((targetId>>8)&0xFF); ackBuf.push_back((targetId>>16)&0xFF); ackBuf.push_back(targetId>>24);
                
                auto pushDWord = [&](std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); };
            DWORD damage = obj.wWepAtk;
                if (damage == 0) damage = 50;
                
                DWORD playerHpCur = 60000;
                DWORD playerHpMax = 60000;
                BYTE bResult = 2; // 2 = Hit
                BYTE bHitFlag = 0; // 0 = Normal, 1 = Critical
                
                if (m_players.count(targetId)) {
                    PlayerData& player = m_players[targetId];
                    
                    playerHpMax = player.dwHpMax;
                    playerHpCur = player.dwHpCur;
                    
                    // Hit/Miss judgment: monsterAtkRatio / (monsterAtkRatio + playerDodge)
                    DWORD monsterHit = 50 + obj.wAtkRatio;
                    DWORD playerDodge = player.dwTotalDodge;
                    float hitChance = (float)monsterHit / (float)(monsterHit + playerDodge);

                    // 降命中 debuff (bKind=20)：命中率乘以 wIncRatePerc/100
                    for (const auto& bf : obj.activeBuffs) {
                        if (bf.second.bIsDebuff) {
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

                    float roll = (float)(rand() % 10000) / 10000.0f;
                    
                    if (playerDodge > 0 && roll > hitChance) {
                        // MISS
                        bResult = 1;
                        damage = 0;
                    } else if (player.activeBuffs.count(130) > 0) {
                        // 130 龟息大法防守端免伤无敌拦截
                        bResult = 1; // MISS (完全免疫)
                        damage = 0;
                        LOG("[MonsterAtk] Player " + std::to_string(targetId) + " protected by Turtle Breath. Zero Damage.");
                    } else {
                        // 如果防御方玩家挂有隐身术 178 却受创，破隐
                        if (player.activeBuffs.count(178) > 0) {
                            player.activeBuffs[178].dwEndTime = 0; // 受伤动作破隐
                            LOG("[MonsterAtk] Player " + std::to_string(targetId) + " hit while stealth. Expiring Stealth.");
                        }
                        // HIT - apply damage variance +/-10%
                        float dmgFloat = (float)damage;
                        float variance = 0.9f + ((float)(rand() % 2000) / 10000.0f);
                        dmgFloat *= variance;
                        
                        // Monster Critical Hit (based on level: 5% base + level/20, cap 15%)
                        WORD critRate = 5 + obj.wLevel / 20;
                        if (critRate > 15) critRate = 15;
                        if ((WORD)(rand() % 100) < critRate) {
                            dmgFloat *= 2.0f;
                            bHitFlag = 1;
                        }
                        
                        damage = (DWORD)dmgFloat;
                        
                        // Defense reduction
                        DWORD defense = player.dwTotalDef;
                        if (damage > defense) {
                            damage = damage - defense;
                        } else {
                            damage = 1;
                        }

                        // 五行二期：怪物对玩家属性附加伤害及凡胎穿透结算
                        DWORD elemDmg = 0;
                        if (obj.bFiveElm > 0) {
                            DWORD baseAtkExp = obj.wIncFiveElmExp;
                            if (baseAtkExp == 0) {
                                baseAtkExp = g_NpcTemplates.count(obj.bPropType) ? g_NpcTemplates[obj.bPropType].wFiveElmExp : 0;
                            }
                            BYTE attLvl = 0; // 怪物默认无五行被动等级
                            BYTE defLvl = player.GetFiveElmPassiveLevel();
                            float fCounter = CalculateFiveElmCounter(obj.bFiveElm, player.bCurFiveElm, attLvl, defLvl, player.bIgnoreFiveElm);
                            
                            WORD playerDefExp = 0;
                            if (player.bCurFiveElm > 0) {
                                switch (obj.bFiveElm) {
                                    case 1: playerDefExp = player.wFireExp; break;
                                    case 2: playerDefExp = player.wWaterExp; break;
                                    case 3: playerDefExp = player.wWoodExp; break;
                                    case 4: playerDefExp = player.wMetalExp; break;
                                    case 5: playerDefExp = player.wEarthExp; break;
                                }
                            }
                            elemDmg = CalculateFiveElmDamage(baseAtkExp, attLvl, fCounter, 0, playerDefExp, player.bIgnoreDefFiveElm);
                        }
                        damage += elemDmg;

                        // bType=4, bKind=5（狂魔毒功/神功）：受伤放大 nEtc1%
                        for (auto& bf : player.activeBuffs) {
                            sMugongTemplate* bfTpl = MugongManager::GetInstance()->GetTemplate(bf.second.dwMugongID);
                            if (bfTpl && bfTpl->bType == 4 && bfTpl->bKind == 5) {
                                sMugongList* bfData = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                                if (bfData && bfData->nEtc1 > 0) {
                                    damage = damage * bfData->nEtc1 / 100;
                                }
                                break;
                            }
                        }
                    }
                    
                    if (bResult == 2) {
                        if (playerHpCur > damage) playerHpCur -= damage;
                        else playerHpCur = 0;
                    }
                    
                    player.dwHpCur = playerHpCur;

                    // bType=4, bKind=1（天魔护体）：被命中时反弹伤害
                    if (bResult == 2 && damage > 0) {
                        LOG("[MonsterAtk] Monster " + std::to_string(obj.dwObjectID) + " at (" + std::to_string(obj.wPosX) + "," + std::to_string(obj.wPosY) + ") HIT player " + std::to_string(targetId) + " for " + std::to_string(damage) + " dmg. activeBuffs count=" + std::to_string(player.activeBuffs.size()) + " has101=" + std::to_string(player.activeBuffs.count(101)));

                        for (auto& bf : player.activeBuffs) {
                            sMugongTemplate* bfTpl = MugongManager::GetInstance()->GetTemplate(bf.second.dwMugongID);
                            if (bfTpl && bfTpl->bType == 4 && bfTpl->bKind == 1) {
                                sMugongList* bfData = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                                if (bfData && bfData->nEtc1 > 0) {
                                    // 成功率判定
                                    WORD successRate = bfData->wSuccessRatePerc;
                                    if (successRate >= 100 || (WORD)(rand() % 100) < successRate) {
                                        DWORD reflectDmg = damage * bfData->nEtc1 / 100;
                                        if (reflectDmg == 0) reflectDmg = 1; // 保底1点反伤
                                            if (obj.dwHpCur > reflectDmg) {
                                                obj.dwHpCur -= reflectDmg;
                                            } else {
                                                obj.dwHpCur = 1; // 反弹不击杀，保底1HP
                                            }

                                            // 广播 0x4016 反弹特效包（客户端 case OUTGONGID_BANTANKANGKI 播放 eBantankangki_Hit）
                                            std::vector<BYTE> reflectPkt(4 + 38, 0);
                                            BYTE* rp = reflectPkt.data() + 4;
                                            rp[0] = 2; // bResult = 2 (HIT)——客户端 case 0 什么都不做，必须用 2
                                            *(DWORD*)(rp + 1) = bf.second.dwMugongID; // 101 = OUTGONGID_BANTANKANGKI
                                            rp[5] = bf.second.bLevel;
                                            rp[6] = 1; // bAtkType = PC
                                            *(DWORD*)(rp + 7) = player.dwObjectID; // 反弹者 = 玩家
                                            *(WORD*)(rp + 11) = player.wPosX;
                                            *(WORD*)(rp + 13) = player.wPosY;
                                            rp[15] = 0;
                                            rp[16] = obj.bObjectType; // bDefType = 怪物
                                            *(DWORD*)(rp + 17) = obj.dwObjectID;
                                            *(DWORD*)(rp + 21) = obj.dwHpMax;
                                            *(DWORD*)(rp + 25) = obj.dwHpCur;
                                            *(DWORD*)(rp + 29) = reflectDmg;
                                            *(DWORD*)(rp + 33) = 0;
                                            rp[37] = 1; // bHitFlag
                                            PACKET_HEADER* rh = (PACKET_HEADER*)reflectPkt.data();
                                            rh->id = 0x4016; rh->payloadSize = 38;
                                            EncryptPacket(reflectPkt.data(), 0x42);
                                            BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, reflectPkt);

                                            LOG("[MonsterAtk] Reflect: Player " + std::to_string(targetId) + " reflected " + std::to_string(reflectDmg) + " dmg to monster " + std::to_string(obj.dwObjectID) + " (nEtc1=" + std::to_string(bfData->nEtc1) + "%)");
                                    }
                                }
                                break; // 只处理第一个反弹buff
                            }
                        }
                    }
                }
                
                ackBuf[0] = bResult; // Write final hit/miss result
                pushDWord(ackBuf, playerHpMax); 
                pushDWord(ackBuf, playerHpCur); 
                pushDWord(ackBuf, damage); 
                pushDWord(ackBuf, 0); 
                ackBuf.push_back(0); 
                ackBuf.push_back(bHitFlag);
                
                std::vector<BYTE> fullAck; fullAck.resize(4); fullAck.insert(fullAck.end(), ackBuf.begin(), ackBuf.end());
                PACKET_HEADER* ackHead = (PACKET_HEADER*)fullAck.data(); ackHead->id = 0x4006; ackHead->payloadSize = ackBuf.size();
                EncryptPacket(fullAck.data(), 0x42); 
                BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, fullAck);

                // Send 0x3B0D (CS_IF_CHARHP_ACK) to the attacked player to update their HP/IP bar
                // 分身没有真实 socket，跳过发包
                if (m_players.count(targetId) && !m_players[targetId].bIsBunsin) {
                    PlayerData& targetPlayer = m_players[targetId];
                    DWORD charID = targetId - 400000000;
                    
                    std::vector<BYTE> hpBuf(4);
                    auto push4hp = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                    auto push2hp = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                    push4hp(playerHpMax);
                    push4hp(playerHpCur);
                    push4hp(targetPlayer.wIpMax);
                    push4hp(targetPlayer.wIpCur);
                    hpBuf.push_back(0); // bType = 0 (silent update, no effect)
                    PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                    hpHead->id = 0x3B0D;
                    hpHead->payloadSize = hpBuf.size() - 4;
                    EncryptPacket(hpBuf.data(), 0x42);
                    
                    // Find socket for this player and send directly
                    {
                        SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(charID);
                        if (targetSock != INVALID_SOCKET) {
                            SafeSend(targetSock, (const char*)hpBuf.data(), hpBuf.size(), 0);
                        }
                    }
                }

                if (playerHpCur == 0) {
                    auto triggerReturn = [&](MonsterData& m) {
                        m.dwTargetID = 0;
                        m.bInAttackRange = false;
                        float dist = std::sqrt(std::pow((float)m.wPosX - m.wSpawnX, 2) + std::pow((float)m.wPosY - m.wSpawnY, 2));
                        if (dist > 1.0f) {
                            m.bIsReturning = true;
                            m.dwReturnStartTime = tick;
                            m.fPosX = (float)m.wPosX;
                            m.fPosY = (float)m.wPosY;
                            m.wDestX = m.wSpawnX;
                            m.wDestY = m.wSpawnY;
                            m.wLastSentDestX = 0;
                            m.wLastSentDestY = 0;
                            m.wLastSentPosX = 0;
                            m.wLastSentPosY = 0;
                            m.dwHpCur = m.dwHpMax;

                            // 广播停步刹车包
                            std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0);
                            DWORD oid = m.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
                            stopBuf.push_back(m.bObjectType); 
                            stopBuf.push_back(m.wPosX & 0xFF); stopBuf.push_back(m.wPosX >> 8);
                            stopBuf.push_back(m.wPosY & 0xFF); stopBuf.push_back(m.wPosY >> 8);
                            stopBuf.push_back(m.bHeight);
                            stopBuf.push_back(0); 
                            PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
                            EncryptPacket(stopBuf.data(), 0x42); 
                            BroadcastPacketAOI_NoLock(m.wPosX, m.wPosY, stopBuf);
                        } else {
                            m.bIsReturning = false;
                            m.wDestX = 0;
                            m.wDestY = 0;
                        }
                    };

                    if (m_players.count(targetId) && m_players[targetId].bIsBunsin) {
                        // 分身死亡：广播 DIE + 从地图和追踪表中清理
                        DWORD ownerCharID = m_players[targetId].dwOwnerID;
                        CleanupSingleBunsin(ownerCharID, targetId, m_dwMapID);
                        RemovePlayer(targetId);
                        triggerReturn(obj);
                        for (auto& mPair : m_monsters) {
                            if (mPair.second.dwTargetID == targetId) triggerReturn(mPair.second);
                        }
                        LOG("[BunsinDeath] Bunsin " + std::to_string(targetId) + " killed by monster. Released locks & set return.");
                    } else {
                        // [业务设计意图]
                        // 真正的玩家遭受怪物物理攻击死亡时，必须立刻在服务端清理其召唤的所有分身，防止分身残留。
                        // [潜在风险]
                        // 此时处于地图锁内，直接调用 CleanupAllBunsins 是线程安全的，不会死锁。
                        extern void CleanupAllBunsins(DWORD ownerCharID, DWORD mapID);
                        CleanupAllBunsins(targetId - 400000000, m_dwMapID);

                        if (m_players.count(targetId) > 0) {
                            extern void ClearPlayerBuffsOnDeath(PlayerData& player, DWORD mapID);
                            ClearPlayerBuffsOnDeath(m_players[targetId], m_dwMapID);
                        }

                        // Mark player for deferred death broadcast (sent in next Update tick)
                        m_players[targetId].dwDeadTime = tick;
                        m_players[targetId].dwInvulnerableUntil = tick + 15000; // 15s death protection
                        triggerReturn(obj);
                    
                        // Release ALL monsters that were targeting this dead player and set return
                        for (auto& mPair : m_monsters) {
                            if (mPair.second.dwTargetID == targetId) {
                                triggerReturn(mPair.second);
                            }
                        }
                    
                        LOG("[PlayerDeath] Player " + std::to_string(targetId) + " HP=0, deferred death. Released all monster locks and set return state.");
                    } // end else (real player death)
                }
            }
            } // end invulnerability else
        } else if (!isRooted) {
            // Move Towards (CHASE) —— 定身状态跳过移动
            if (obj.dwChaseStartTime == 0) {
                obj.dwChaseStartTime = tick;
            }
            // 追击速度平滑拟合：与客户端 NPC Run 骨骼动画位移速率 1:1 咬合
            // 客户端所有普通 NPC 播放 eLAT_Run 奔跑动画时，动画位移速率均约为 18.0 ~ 20.0 格/秒
            // 若怪物的 wRunSpeed100 配置较小(如野兔651=6.5格/秒、恶狼815=8.1格/秒)，服务端会落后客户端整整数倍，导致怪物在客户端早早到达站定发呆
            // 因此保底速度设为 18.5 格/秒，与引路犬(19.64)及客户端奔跑动画完全同频，冲到面前同tick立即出刀
            float dbSpeed = (tpl.wRunSpeed100 > 0) ? ((float)tpl.wRunSpeed100 / 100.0f) : 19.0f;
            if (dbSpeed < 18.5f) dbSpeed = 18.5f;
            if (dbSpeed > 25.0f) dbSpeed = 25.0f;
            float step = dbSpeed / 10.0f; 

            // 智能避障与平滑移动：统一使用玩家当前真实物理坐标
            float targetX = (float)bestPlayer->wPosX;
            float targetY = (float)bestPlayer->wPosY;
            int iTargetX = (int)(targetX + 0.5f);
            int iTargetY = (int)(targetY + 0.5f);

            bool hasLos = HasLineOfSight(obj.wPosX, obj.wPosY, iTargetX, iTargetY);
            float moveDirX = 0.0f;
            float moveDirY = 0.0f;

            if (hasLos) {
                // 视线通畅：直接向玩家前进
                float dx = targetX - (float)obj.wPosX;
                float dy = targetY - (float)obj.wPosY;
                float dLen = std::sqrt(dx * dx + dy * dy);
                if (dLen > 0.001f) {
                    moveDirX = dx / dLen;
                    moveDirY = dy / dLen;
                }
            } else {
                // 视线受阻：局部 A* 搜索绕障路径
                std::vector<std::pair<int, int>> path;
                if (FindPath(obj.wPosX, obj.wPosY, iTargetX, iTargetY, path, 35) && path.size() >= 2) {
                    // String-Pulling 绳拉直：前探获取最远可视路标
                    int wpIdx = FindFurthestVisibleWaypoint(obj.wPosX, obj.wPosY, path);
                    int nextWp = (wpIdx > 0) ? wpIdx : 1;
                    float dx = (float)path[nextWp].first - (float)obj.wPosX;
                    float dy = (float)path[nextWp].second - (float)obj.wPosY;
                    float dLen = std::sqrt(dx * dx + dy * dy);
                    if (dLen > 0.001f) {
                        moveDirX = dx / dLen;
                        moveDirY = dy / dLen;
                    }
                } else {
                    // A* 未找到完整路径时：不拉脱！沿目标方向贪心前进
                    float dx = targetX - (float)obj.wPosX;
                    float dy = targetY - (float)obj.wPosY;
                    float dLen = std::sqrt(dx * dx + dy * dy);
                    if (dLen > 0.001f) {
                        moveDirX = dx / dLen;
                        moveDirY = dy / dLen;
                    }
                }
            }

            obj.fPosX += moveDirX * step;
            obj.fPosY += moveDirY * step;
            int nextX = (int)(obj.fPosX + 0.5f);
            int nextY = (int)(obj.fPosY + 0.5f);

            bool canMove = IsWalkable(nextX, nextY);
            if (!canMove) {
                // 滑动碰撞：尝试单轴移动 (X或Y) 绕开障碍
                if (IsWalkable(nextX, obj.wPosY)) {
                    nextY = obj.wPosY;
                    obj.fPosY = (float)obj.wPosY;
                    canMove = true;
                } else if (IsWalkable(obj.wPosX, nextY)) {
                    nextX = obj.wPosX;
                    obj.fPosX = (float)obj.wPosX;
                    canMove = true;
                }
            }

            if (canMove) {
                int ox = obj.wPosX, oy = obj.wPosY;
                obj.wPosX = (WORD)nextX; 
                obj.wPosY = (WORD)nextY;
                UpdateMonsterGrid(obj.dwObjectID, ox, oy, obj.wPosX, obj.wPosY);

                // 追击目的地：停在距玩家攻击距离边缘处，而非直接冲入玩家身体
                float toMonX = (float)obj.wPosX - targetX;
                float toMonY = (float)obj.wPosY - targetY;
                float toMonDist = std::sqrt(toMonX * toMonX + toMonY * toMonY);
                // 追击停靠点：停在攻击距离的 75% 处（确保必定处于攻击距离 enterRange 之内，绝对触发攻击）
                float standDist = atkRange * 0.75f;
                if (standDist < 1.2f) standDist = 1.2f;

                WORD targetDestX, targetDestY;
                if (toMonDist > standDist && toMonDist > 0.001f) {
                    float nx = toMonX / toMonDist;
                    float ny = toMonY / toMonDist;
                    targetDestX = (WORD)(targetX + nx * standDist + 0.5f);
                    targetDestY = (WORD)(targetY + ny * standDist + 0.5f);
                } else {
                    targetDestX = obj.wPosX;
                    targetDestY = obj.wPosY;
                }
                obj.wDestX = targetDestX;
                obj.wDestY = targetDestY;

                float angle = std::atan2(moveDirY, moveDirX) * 180.0f / 3.14159265f;
                if (angle < 0) angle += 360.0f;
                WORD wDirection = (WORD)angle;

                bool isFirstMove = (obj.wLastSentDestX == 0 && obj.wLastSentDestY == 0);
                int angleDiff = std::abs((int)wDirection - (int)obj.wLastSentDirection);
                if (angleDiff > 180) angleDiff = 360 - angleDiff;

                // 目标位置变化检测：玩家走位 >= 1.5 格且满 250ms 冷却时，即时重定向
                float destDelta = std::sqrt(
                    std::pow((float)targetDestX - (float)obj.wLastSentDestX, 2) +
                    std::pow((float)targetDestY - (float)obj.wLastSentDestY, 2)
                );
                bool targetMoved = (destDelta >= 1.5f && (tick - obj.dwLastMoveSendTime >= 250));
                bool directionChanged = (angleDiff > 35 && destDelta >= 1.0f && (tick - obj.dwLastMoveSendTime >= 350));

                // 仅在初次起步、目标位移或显著转向时发包，取消 800ms 盲目重发以防打断客户端寻路
                if (isFirstMove || targetMoved || directionChanged) {
                    obj.wLastSentDestX = targetDestX;
                    obj.wLastSentDestY = targetDestY;
                    obj.wLastSentPosX = obj.wPosX;
                    obj.wLastSentPosY = obj.wPosY;
                    obj.dwLastMoveSendTime = tick;
                    obj.wLastSentDirection = wDirection;

                    std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); 
                    DWORD oid = obj.dwObjectID; ackBuf.push_back(oid&0xFF); ackBuf.push_back((oid>>8)&0xFF); ackBuf.push_back((oid>>16)&0xFF); ackBuf.push_back(oid>>24);
                    ackBuf.push_back(obj.bObjectType); 
                    ackBuf.push_back(obj.wPosX & 0xFF); ackBuf.push_back(obj.wPosX >> 8);
                    ackBuf.push_back(obj.wPosY & 0xFF); ackBuf.push_back(obj.wPosY >> 8);
                    ackBuf.push_back(obj.bHeight);
                    ackBuf.push_back(targetDestX & 0xFF); ackBuf.push_back(targetDestX >> 8);
                    ackBuf.push_back(targetDestY & 0xFF); ackBuf.push_back(targetDestY >> 8);
                    ackBuf.push_back(obj.bHeight); 
                    ackBuf.push_back(wDirection & 0xFF); ackBuf.push_back(wDirection >> 8); 
                    ackBuf.push_back(21); // NPCSTATUS_RUN (21) 追击全面启用跑步，播放 eLAT_Run 奔跑动作
                    BYTE chaseSpeedByte = tpl.bRunSpeed > 0 ? tpl.bRunSpeed : (obj.bRunSpeedByte > 0 ? obj.bRunSpeedByte : 10);
                    ackBuf.push_back(chaseSpeedByte); 
                    
                    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3508; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                    EncryptPacket(ackBuf.data(), 0x42); 
                    BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, ackBuf);
                }
            } else {
                obj.fPosX = (float)obj.wPosX;
                obj.fPosY = (float)obj.wPosY;
            }
        }
    } else {
        // Return to Spawn or Wander
        float distToSpawn = std::sqrt(std::pow((float)obj.wPosX - obj.wSpawnX, 2) + std::pow((float)obj.wPosY - obj.wSpawnY, 2));

        if (obj.bIsReturning) {
            // 刹车站定缓冲：脱战后先原地站定 300ms（由脱战瞬间广播的 0x350C 维持待机姿态），平滑过渡后再起步掉头回程
            if (tick - obj.dwReturnStartTime < 300) {
                return;
            }

            // 动态超时保护：根据拉脱距离动态分配充裕时间，避免回程走到一半强行瞬移
            DWORD returnTimeoutMs = (DWORD)(distToSpawn * 600);
            if (returnTimeoutMs < 20000) returnTimeoutMs = 20000;
            bool returnTimeout = (obj.dwReturnStartTime > 0 && tick - obj.dwReturnStartTime >= returnTimeoutMs);

            // 1. 到达出生点（保底1.5格内平滑完成回程）或极端卡死超时
            if (distToSpawn <= 1.5f || returnTimeout) {
                int oldX = obj.wPosX, oldY = obj.wPosY;
                if (obj.wPosX != obj.wSpawnX || obj.wPosY != obj.wSpawnY) {
                    if (IsWalkable(obj.wSpawnX, obj.wSpawnY)) {
                        obj.wPosX = obj.wSpawnX;
                        obj.wPosY = obj.wSpawnY;
                        obj.fPosX = (float)obj.wSpawnX;
                        obj.fPosY = (float)obj.wSpawnY;
                        UpdateMonsterGrid(obj.dwObjectID, oldX, oldY, obj.wPosX, obj.wPosY);
                    }
                }

                obj.bIsReturning = false;
                obj.dwReturnStartTime = 0;
                obj.wDestX = 0;
                obj.wDestY = 0;
                obj.wLastSentDestX = 0;
                obj.wLastSentDestY = 0;
                obj.wLastSentPosX = 0;
                obj.wLastSentPosY = 0;
                obj.dwLastAttackTime = 0;
                obj.dwLastWanderTime = tick;
                obj.dwChaseStartTime = 0;

                std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0);
                DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
                stopBuf.push_back(obj.bObjectType); 
                stopBuf.push_back(obj.wPosX & 0xFF); stopBuf.push_back(obj.wPosX >> 8);
                stopBuf.push_back(obj.wPosY & 0xFF); stopBuf.push_back(obj.wPosY >> 8);
                stopBuf.push_back(obj.bHeight);
                stopBuf.push_back(0); 
                PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(stopBuf.data(), 0x42); 
                BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, stopBuf);

                LOG("[MonsterAI] RETURN COMPLETED: ObjID=" + std::to_string(obj.dwObjectID) + " returned to spawn (" + std::to_string(obj.wSpawnX) + "," + std::to_string(obj.wSpawnY) + ")");
                return;
            }

            // 2. 回程路线避障：直通路则直接回出生点，否则使用 A* 寻找回程路标
            WORD targetDestX = obj.wSpawnX;
            WORD targetDestY = obj.wSpawnY;
            if (!HasLineOfSight(obj.wPosX, obj.wPosY, obj.wSpawnX, obj.wSpawnY)) {
                std::vector<std::pair<int, int>> path;
                if (FindPath(obj.wPosX, obj.wPosY, obj.wSpawnX, obj.wSpawnY, path, 80) && path.size() >= 2) {
                    int wpIdx = FindFurthestVisibleWaypoint(obj.wPosX, obj.wPosY, path);
                    if (wpIdx > 0) {
                        targetDestX = (WORD)path[wpIdx].first;
                        targetDestY = (WORD)path[wpIdx].second;
                    }
                }
            }
            obj.wDestX = targetDestX;
            obj.wDestY = targetDestY;

            // 3. 回程移动步长计算（与客户端奔跑动画 1:1 咬合，平滑疾奔返回出生点）
            float distToDest = std::sqrt(std::pow((float)obj.wPosX - obj.wDestX, 2) + std::pow((float)obj.wPosY - obj.wDestY, 2));
            float returnSpeed = (tpl.wRunSpeed100 > 0) ? ((float)tpl.wRunSpeed100 / 100.0f) : 19.0f;
            if (returnSpeed < 18.5f) returnSpeed = 18.5f;
            if (returnSpeed > 25.0f) returnSpeed = 25.0f;
            float step = returnSpeed / 10.0f;

            float dirX = (distToDest > 0.001f) ? (((float)obj.wDestX - (float)obj.wPosX) / distToDest) : 0.0f;
            float dirY = (distToDest > 0.001f) ? (((float)obj.wDestY - (float)obj.wPosY) / distToDest) : 0.0f;
            
            float angle = std::atan2(dirY, dirX) * 180.0f / 3.14159265f;
            if (angle < 0) angle += 360.0f;
            WORD wDirection = (WORD)angle;

            obj.fPosX += dirX * step;
            obj.fPosY += dirY * step;

            WORD nx = (WORD)(obj.fPosX + 0.5f);
            WORD ny = (WORD)(obj.fPosY + 0.5f);

            bool canMove = IsWalkable(nx, ny);
            if (!canMove) {
                // 单轴滑动避障：沿障碍边缘滑行，坚决不下发 0x350C 避免客户端急刹抽搐
                if (IsWalkable(nx, obj.wPosY)) {
                    ny = obj.wPosY;
                    obj.fPosY = (float)obj.wPosY;
                    canMove = true;
                } else if (IsWalkable(obj.wPosX, ny)) {
                    nx = obj.wPosX;
                    obj.fPosX = (float)obj.wPosX;
                    canMove = true;
                }
            }

            if (canMove) {
                int ox = obj.wPosX, oy = obj.wPosY;
                obj.wPosX = nx; obj.wPosY = ny;
                UpdateMonsterGrid(obj.dwObjectID, ox, oy, nx, ny);
            } else {
                obj.fPosX = (float)obj.wPosX;
                obj.fPosY = (float)obj.wPosY;
            }

            // 4. 发包控制：仅在初次起步或目标拐点显著改变时下发 0x3508，彻底消灭 1500ms 盲目重发导致的闪跳与拉扯
            bool isFirstMove = (obj.wLastSentDestX == 0 && obj.wLastSentDestY == 0);
            float destDelta = std::sqrt(
                std::pow((float)obj.wDestX - (float)obj.wLastSentDestX, 2) +
                std::pow((float)obj.wDestY - (float)obj.wLastSentDestY, 2)
            );
            bool destChanged = (destDelta >= 2.0f && (tick - obj.dwLastMoveSendTime >= 500));

            if (isFirstMove || destChanged) {
                obj.wLastSentDestX = obj.wDestX;
                obj.wLastSentDestY = obj.wDestY;
                obj.wLastSentPosX = obj.wPosX;
                obj.wLastSentPosY = obj.wPosY;
                obj.dwLastMoveSendTime = tick;
                obj.wLastSentDirection = wDirection;

                std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); 
                DWORD oid = obj.dwObjectID; ackBuf.push_back(oid&0xFF); ackBuf.push_back((oid>>8)&0xFF); ackBuf.push_back((oid>>16)&0xFF); ackBuf.push_back(oid>>24);
                ackBuf.push_back(obj.bObjectType); 
                ackBuf.push_back(obj.wPosX & 0xFF); ackBuf.push_back(obj.wPosX >> 8);
                ackBuf.push_back(obj.wPosY & 0xFF); ackBuf.push_back(obj.wPosY >> 8);
                ackBuf.push_back(obj.bHeight);
                ackBuf.push_back(obj.wDestX & 0xFF); ackBuf.push_back(obj.wDestX >> 8);
                ackBuf.push_back(obj.wDestY & 0xFF); ackBuf.push_back(obj.wDestY >> 8);
                ackBuf.push_back(obj.bHeight); 
                ackBuf.push_back(wDirection & 0xFF); ackBuf.push_back(wDirection >> 8); 
                ackBuf.push_back(21); // NPCSTATUS_RUN (21) 脱战回程使用跑步，疾奔返回出生点
                BYTE retSpeedByte = tpl.bRunSpeed > 0 ? tpl.bRunSpeed : (obj.bRunSpeedByte > 0 ? obj.bRunSpeedByte : 10);
                ackBuf.push_back(retSpeedByte); 
                
                PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3508; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(ackBuf.data(), 0x42); 
                BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, ackBuf);
            }
            return;
        } else {
            // Idle wander: stationary monsters or monsters with no wander range stay still
            if (obj.dwMovePattern == 1) return; 

            int range = obj.wWanderRange;
            if (range <= 0) return; 

            if (distToSpawn > range) {
                // 超出漫游范围拉回
                WORD targetDestX = obj.wSpawnX;
                WORD targetDestY = obj.wSpawnY;
                if (!HasLineOfSight(obj.wPosX, obj.wPosY, obj.wSpawnX, obj.wSpawnY)) {
                    std::vector<std::pair<int, int>> path;
                    if (FindPath(obj.wPosX, obj.wPosY, obj.wSpawnX, obj.wSpawnY, path, 30) && path.size() >= 2) {
                        int wpIdx = FindFurthestVisibleWaypoint(obj.wPosX, obj.wPosY, path);
                        if (wpIdx > 0) {
                            targetDestX = (WORD)path[wpIdx].first;
                            targetDestY = (WORD)path[wpIdx].second;
                        }
                    }
                }
                obj.wDestX = targetDestX;
                obj.wDestY = targetDestY;
            } else if (obj.wDestX == 0 && obj.wDestY == 0) {
                if (tick - obj.dwLastWanderTime > (DWORD)(2000 + rand() % 3000)) { 
                    if (tpl.bIdleRatio > 0 && (rand() % 100) < tpl.bIdleRatio) {
                        obj.dwLastWanderTime = tick; 
                    } else {
                        // 寻找合法可通行的巡逻游荡点（必须具备直通视线，杜绝撞墙）
                        WORD candX = obj.wSpawnX;
                        WORD candY = obj.wSpawnY;
                        bool foundCand = false;
                        for (int attempt = 0; attempt < 8; ++attempt) {
                            int rx = (int)obj.wSpawnX + (rand() % (range * 2 + 1)) - range;
                            int ry = (int)obj.wSpawnY + (rand() % (range * 2 + 1)) - range;
                            if (rx >= 0 && ry >= 0 && IsWalkable(rx, ry) && HasLineOfSight(obj.wPosX, obj.wPosY, rx, ry)) {
                                candX = (WORD)rx;
                                candY = (WORD)ry;
                                foundCand = true;
                                break;
                            }
                        }
                        if (foundCand && (candX != obj.wPosX || candY != obj.wPosY)) {
                            obj.wDestX = candX;
                            obj.wDestY = candY;
                        }
                        obj.dwLastWanderTime = tick;
                    }
                }
            }
        }

        if (obj.wDestX != 0 || obj.wDestY != 0) {
            float distToDest = std::sqrt(std::pow((float)obj.wPosX - obj.wDestX, 2) + std::pow((float)obj.wPosY - obj.wDestY, 2));
            float dbSpeed = (tpl.wWalkSpeed100 > 0) ? ((float)tpl.wWalkSpeed100 / 100.0f) : 2.0f;
            if (dbSpeed < 1.5f) dbSpeed = 1.5f;
            if (dbSpeed > 4.5f) dbSpeed = 4.5f;
            float step = dbSpeed / 10.0f;
            
            if (distToDest <= step) {
                int ox = obj.wPosX, oy = obj.wPosY;
                obj.wPosX = obj.wDestX;
                obj.wPosY = obj.wDestY;
                UpdateMonsterGrid(obj.dwObjectID, ox, oy, obj.wPosX, obj.wPosY);
                obj.fPosX = (float)obj.wDestX;
                obj.fPosY = (float)obj.wDestY;
                obj.wDestX = 0;
                obj.wDestY = 0;
                obj.wLastSentDestX = 0;
                obj.wLastSentDestY = 0;
                obj.wLastSentPosX = 0;
                obj.wLastSentPosY = 0;
                obj.dwLastWanderTime = tick; 
                
                std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0);
                DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
                stopBuf.push_back(obj.bObjectType); 
                stopBuf.push_back(obj.wPosX & 0xFF); stopBuf.push_back(obj.wPosX >> 8);
                stopBuf.push_back(obj.wPosY & 0xFF); stopBuf.push_back(obj.wPosY >> 8);
                stopBuf.push_back(obj.bHeight);
                stopBuf.push_back(0); 
                PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(stopBuf.data(), 0x42); 
                BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, stopBuf);
            } else {
                float dirX = ((float)obj.wDestX - (float)obj.wPosX) / distToDest;
                float dirY = ((float)obj.wDestY - (float)obj.wPosY) / distToDest;
                
                float angle = std::atan2(dirY, dirX) * 180.0f / 3.14159265f;
                if (angle < 0) angle += 360.0f;
                WORD wDirection = (WORD)angle;
                
                obj.fPosX += dirX * step;
                obj.fPosY += dirY * step;
                
                WORD nx = (WORD)(obj.fPosX + 0.5f);
                WORD ny = (WORD)(obj.fPosY + 0.5f);
                
                bool canMove = IsWalkable(nx, ny);
                if (canMove) {
                    int ox = obj.wPosX, oy = obj.wPosY;
                    obj.wPosX = nx; obj.wPosY = ny;
                    UpdateMonsterGrid(obj.dwObjectID, ox, oy, nx, ny);
                    
                    bool wanderNeedSend = (obj.wDestX != obj.wLastSentDestX || obj.wDestY != obj.wLastSentDestY || (tick - obj.dwLastMoveSendTime > 1500));
                    if (wanderNeedSend) {
                        obj.wLastSentDestX = obj.wDestX;
                        obj.wLastSentDestY = obj.wDestY;
                        obj.wLastSentPosX = obj.wPosX;
                        obj.wLastSentPosY = obj.wPosY;
                        obj.dwLastMoveSendTime = tick;
                        obj.wLastSentDirection = wDirection;

                        std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); 
                        DWORD oid = obj.dwObjectID; ackBuf.push_back(oid&0xFF); ackBuf.push_back((oid>>8)&0xFF); ackBuf.push_back((oid>>16)&0xFF); ackBuf.push_back(oid>>24);
                        ackBuf.push_back(obj.bObjectType); 
                        ackBuf.push_back(obj.wPosX & 0xFF); ackBuf.push_back(obj.wPosX >> 8);
                        ackBuf.push_back(obj.wPosY & 0xFF); ackBuf.push_back(obj.wPosY >> 8);
                        ackBuf.push_back(obj.bHeight);
                        ackBuf.push_back(obj.wDestX & 0xFF); ackBuf.push_back(obj.wDestX >> 8);
                        ackBuf.push_back(obj.wDestY & 0xFF); ackBuf.push_back(obj.wDestY >> 8);
                        ackBuf.push_back(obj.bHeight); 
                        ackBuf.push_back(wDirection & 0xFF); ackBuf.push_back(wDirection >> 8); 
                        ackBuf.push_back(20); // NPCSTATUS_WALK (20) 巡逻游荡保持散步
                        BYTE wanderSpeedByte = tpl.bWalkSpeed > 0 ? tpl.bWalkSpeed : (obj.bWalkSpeedByte > 0 ? obj.bWalkSpeedByte : 8);
                        ackBuf.push_back(wanderSpeedByte); 
                        
                        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3508; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                        EncryptPacket(ackBuf.data(), 0x42); 
                        BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, ackBuf);
                    }
                } else {
                    // 漫游或回程遇阻：停步并发送停止包
                    obj.wDestX = 0;
                    obj.wDestY = 0;
                    obj.wLastSentDestX = 0;
                    obj.wLastSentDestY = 0;
                    obj.fPosX = (float)obj.wPosX;
                    obj.fPosY = (float)obj.wPosY;

                    std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0);
                    DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
                    stopBuf.push_back(obj.bObjectType); 
                    stopBuf.push_back(obj.wPosX & 0xFF); stopBuf.push_back(obj.wPosX >> 8);
                    stopBuf.push_back(obj.wPosY & 0xFF); stopBuf.push_back(obj.wPosY >> 8);
                    stopBuf.push_back(obj.bHeight);
                    stopBuf.push_back(0); 
                    PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
                    EncryptPacket(stopBuf.data(), 0x42); 
                    BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, stopBuf);
                }
            }
        }
    }
}

// ==========================================
// AOI Grid System Implementation
// ==========================================

int CMapInstance::GetGridIndex(int x, int y) const {
    if (x < 0) x = 0;
    if (x >= m_width) x = m_width - 1;
    if (y < 0) y = 0;
    if (y >= m_height) y = m_height - 1;
    
    int gx = x / GRID_SIZE;
    int gy = y / GRID_SIZE;
    if (gx >= m_gridCols) gx = m_gridCols - 1;
    if (gy >= m_gridRows) gy = m_gridRows - 1;
    
    return gy * m_gridCols + gx;
}

void CMapInstance::RemoveFromGrid(std::vector<std::vector<DWORD>>& grid, int gridIdx, DWORD dwObjectID) {
    if (gridIdx >= 0 && gridIdx < (int)grid.size()) {
        auto& list = grid[gridIdx];
        for (auto it = list.begin(); it != list.end(); ++it) {
            if (*it == dwObjectID) {
                list.erase(it);
                break;
            }
        }
    }
}

void CMapInstance::AddToGrid(std::vector<std::vector<DWORD>>& grid, int gridIdx, DWORD dwObjectID) {
    if (gridIdx >= 0 && gridIdx < (int)grid.size()) {
        grid[gridIdx].push_back(dwObjectID);
    }
}

void CMapInstance::UpdatePlayerGrid(DWORD dwObjectID, int oldX, int oldY, int newX, int newY) {
    int oldIdx = GetGridIndex(oldX, oldY);
    int newIdx = GetGridIndex(newX, newY);
    if (oldIdx != newIdx) {
        RemoveFromGrid(m_playerGrid, oldIdx, dwObjectID);
        AddToGrid(m_playerGrid, newIdx, dwObjectID);
    }
}

void CMapInstance::UpdateMonsterGrid(DWORD dwObjectID, int oldX, int oldY, int newX, int newY) {
    int oldIdx = GetGridIndex(oldX, oldY);
    int newIdx = GetGridIndex(newX, newY);
    if (oldIdx != newIdx) {
        RemoveFromGrid(m_monsterGrid, oldIdx, dwObjectID);
        AddToGrid(m_monsterGrid, newIdx, dwObjectID);
    }
}

std::vector<PlayerData*> CMapInstance::GetPlayersInAOI(int x, int y) {
    std::vector<PlayerData*> result;
    int gx = x / GRID_SIZE;
    int gy = y / GRID_SIZE;
    
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int cx = gx + dx;
            int cy = gy + dy;
            if (cx >= 0 && cx < m_gridCols && cy >= 0 && cy < m_gridRows) {
                int idx = cy * m_gridCols + cx;
                for (DWORD dwObjectID : m_playerGrid[idx]) {
                    auto it = m_players.find(dwObjectID);
                    if (it != m_players.end()) {
                        result.push_back(&(it->second));
                    }
                }
            }
        }
    }
    return result;
}

std::vector<MonsterData*> CMapInstance::GetMonstersInAOI(int x, int y) {
    std::vector<MonsterData*> result;
    int gx = x / GRID_SIZE;
    int gy = y / GRID_SIZE;
    
    for (int dy = -1; dy <= 1; ++dy) {
        for (int dx = -1; dx <= 1; ++dx) {
            int cx = gx + dx;
            int cy = gy + dy;
            if (cx >= 0 && cx < m_gridCols && cy >= 0 && cy < m_gridRows) {
                int idx = cy * m_gridCols + cx;
                for (DWORD dwObjectID : m_monsterGrid[idx]) {
                    auto it = m_monsters.find(dwObjectID);
                    if (it != m_monsters.end()) {
                        result.push_back(&(it->second));
                    }
                }
            }
        }
    }
    return result;
}

void CMapInstance::BroadcastPacketAOI_NoLock(int x, int y, const std::vector<BYTE>& packet, SOCKET excludeSocket) {
    std::unordered_set<DWORD> targetIDs;
    std::vector<sServerObject*> players = GetPlayersInAOI(x, y);
    for (sServerObject* p : players) {
        targetIDs.insert(p->dwObjectID);
    }
    if (targetIDs.empty()) return;

    SessionMgr::GetInstance().SendToObjectIDs(targetIDs, m_dwMapID, packet, excludeSocket);
}

void CMapInstance::BroadcastPacketAOI(int x, int y, const std::vector<BYTE>& packet, SOCKET excludeSocket) {
    std::unordered_set<DWORD> targetIDs;
    {
        std::lock_guard<std::mutex> lock(m_mapMutex);
        std::vector<sServerObject*> players = GetPlayersInAOI(x, y);
        for (sServerObject* p : players) {
            targetIDs.insert(p->dwObjectID);
        }
        // Debug: log how many players found in AOI
        static DWORD s_lastBroadcastLog = 0;
        DWORD now = GetTickCount();
        if (now - s_lastBroadcastLog > 3000) {
            s_lastBroadcastLog = now;
            std::string ids = "";
            for (DWORD id : targetIDs) ids += std::to_string(id) + " ";
            char dbg[512]; sprintf(dbg, "[BroadcastAOI] pos(%d,%d) grid(%d,%d) found %d players: %s", 
                x, y, x/100, y/100, (int)targetIDs.size(), ids.c_str());
            LOG(std::string(dbg));
        }
    }
    if (targetIDs.empty()) return;

    SessionMgr::GetInstance().SendToObjectIDs(targetIDs, m_dwMapID, packet, excludeSocket);
}

void CMapInstance::HandleMonsterDoTDeath(DWORD tick, MonsterData& obj, DWORD casterID, DWORD dwMugongID, BYTE bLevel) {
    obj.dwHpCur = 0;
    obj.dwDeadTime = tick;
    obj.dwTargetID = 0;

    LOG("[DoT-Death] Monster " + std::to_string(obj.dwObjectID) + " (PropType=" + std::to_string((int)obj.bPropType) + ") died from skill " + std::to_string(dwMugongID) + " by Caster " + std::to_string(casterID));

    // 1. 生成怪物掉落物品：若施法者为战宠/召唤物(800000000~850000000)，掉落归属权赋给主人
    DWORD actualKillerCharID = 0;
    DWORD actualDropCasterID = casterID;
    if (casterID >= 800000000 && casterID < 850000000) {
        PlayerData* pPet = GetPlayer(casterID);
        if (pPet && pPet->dwOwnerID > 0) {
            actualKillerCharID = pPet->dwOwnerID;
            actualDropCasterID = actualKillerCharID + 400000000;
        }
    } else if (casterID >= 400000000 && casterID < 800000000) {
        actualKillerCharID = casterID - 400000000;
    }

    DropManager::GetInstance()->GenerateDrops(actualDropCasterID, obj);

    // 2. 广播 0x3510 死亡动作包。这非常关键，客户端在此包里处理 NPCSTATUS_DIE 并丢弃选中目标、关闭目标面板
    std::vector<BYTE> animBuf;
    animBuf.resize(4);
    animBuf.push_back(obj.bObjectType); // 3 = Monster/NPC
    DWORD oid = obj.dwObjectID;
    animBuf.push_back(oid & 0xFF);
    animBuf.push_back((oid >> 8) & 0xFF);
    animBuf.push_back((oid >> 16) & 0xFF);
    animBuf.push_back(oid >> 24);
    animBuf.push_back(3); // bStatus = 3 (Dead)
    animBuf.push_back(0); 
    animBuf.push_back(0); 
    animBuf.push_back(0xFF); 

    PACKET_HEADER* animHead = (PACKET_HEADER*)animBuf.data();
    animHead->id = 0x3510;
    animHead->payloadSize = animBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(animBuf.data(), 0x42);
    BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, animBuf);

    // 3. 结算击杀经验与五行经验给施法者玩家，并处理潜在的升级属性同步
    if (actualKillerCharID > 0 && actualKillerCharID < 400000000) {
        DWORD deadExp = obj.dwExp;
        DWORD targetFiveElmExp = g_NpcTemplates.count(obj.bPropType) ? g_NpcTemplates[obj.bPropType].wFiveElmExp : 0;
        targetFiveElmExp += obj.wIncFiveElmExp;

        // 统一调用队伍经验分配逻辑（包含同地图与50格距离严格校验，以及升级广播）
        DistributePartyExp(actualKillerCharID, deadExp, targetFiveElmExp, m_dwMapID, obj.wPosX, obj.wPosY, true /* callerHoldsMapLock */);
    }
}


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
                    if (dist >= 18) {
                        int oldX = pl.wPosX; int oldY = pl.wPosY;
                        int gridIdxOld = GetGridIndex(oldX, oldY);
                        if (gridIdxOld >= 0) RemoveFromGrid(m_playerGrid, gridIdxOld, pl.dwObjectID);
                        pl.wPosX = owner.wPosX; pl.wPosY = owner.wPosY;
                        pl.fPosX = (float)pl.wPosX; pl.fPosY = (float)pl.wPosY;
                        int gridIdxNew = GetGridIndex(pl.wPosX, pl.wPosY);
                        if (gridIdxNew >= 0) AddToGrid(m_playerGrid, gridIdxNew, pl.dwObjectID);
                        std::vector<BYTE> leaveBuf; leaveBuf.resize(4);
                        leaveBuf.push_back(0);
                        leaveBuf.push_back(pl.dwObjectID & 0xFF); leaveBuf.push_back((pl.dwObjectID >> 8) & 0xFF);
                        leaveBuf.push_back((pl.dwObjectID >> 16) & 0xFF); leaveBuf.push_back(pl.dwObjectID >> 24);
                        leaveBuf.push_back(4);
                        leaveBuf.push_back(m_dwMapID & 0xFF); leaveBuf.push_back((m_dwMapID >> 8) & 0xFF);
                        leaveBuf.push_back((m_dwMapID >> 16) & 0xFF); leaveBuf.push_back(m_dwMapID >> 24);
                        leaveBuf.push_back(0);
                        PACKET_HEADER* lh = (PACKET_HEADER*)leaveBuf.data();
                        lh->id = 0x3506; lh->payloadSize = leaveBuf.size() - 4;
                        EncryptPacket(leaveBuf.data(), 0x42);
                        BroadcastPacketAOI_NoLock(oldX, oldY, leaveBuf);
                        std::vector<BYTE> enterBuf; enterBuf.resize(4);
                        enterBuf.push_back(0);
                        enterBuf.push_back(m_dwMapID & 0xFF); enterBuf.push_back((m_dwMapID >> 8) & 0xFF);
                        enterBuf.push_back((m_dwMapID >> 16) & 0xFF); enterBuf.push_back(m_dwMapID >> 24);
                        enterBuf.push_back(pl.dwObjectID & 0xFF); enterBuf.push_back((pl.dwObjectID >> 8) & 0xFF);
                        enterBuf.push_back((pl.dwObjectID >> 16) & 0xFF); enterBuf.push_back(pl.dwObjectID >> 24);
                        enterBuf.push_back(4);
                        enterBuf.push_back(pl.wPosX & 0xFF); enterBuf.push_back(pl.wPosX >> 8);
                        enterBuf.push_back(pl.wPosY & 0xFF); enterBuf.push_back(pl.wPosY >> 8);
                        enterBuf.push_back(pl.bHeight);
                        enterBuf.push_back(0); enterBuf.push_back(0);
                        enterBuf.push_back(0);
                        enterBuf.push_back(pl.wWalkSpeed & 0xFF);
                        PACKET_HEADER* eh = (PACKET_HEADER*)enterBuf.data();
                        eh->id = 0x3502; eh->payloadSize = enterBuf.size() - 4;
                        EncryptPacket(enterBuf.data(), 0x42);
                        BroadcastPacketAOI_NoLock(pl.wPosX, pl.wPosY, enterBuf);
                    }
                    else if (dist > 2) {
                        if (tick - pl.dwLastPetMoveTime >= 1000) {
                            pl.dwLastPetMoveTime = tick;
                            int oldX = pl.wPosX; int oldY = pl.wPosY;
                            int gridIdxOld = GetGridIndex(oldX, oldY);
                            if (gridIdxOld >= 0) RemoveFromGrid(m_playerGrid, gridIdxOld, pl.dwObjectID);
                            int stepX = (owner.wPosX > pl.wPosX) ? 1 : ((owner.wPosX < pl.wPosX) ? -1 : 0);
                            int stepY = (owner.wPosY > pl.wPosY) ? 1 : ((owner.wPosY < pl.wPosY) ? -1 : 0);
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
                            moveBuf.push_back(owner.wPosX & 0xFF); moveBuf.push_back(owner.wPosX >> 8);
                            moveBuf.push_back(owner.wPosY & 0xFF); moveBuf.push_back(owner.wPosY >> 8);
                            moveBuf.push_back(owner.bHeight);
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
        if (!pMon || pMon->dwHpCur == 0) continue;
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
        float speed = (float)(p.wWalkSpeed > 0 ? p.wWalkSpeed : 11) * 0.1f * 0.65f * speedScale;
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

void CMapInstance::ProcessMonsterAI(DWORD tick, MonsterData& obj) {
    if (obj.bObjectType != 3) return; // Monster only
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

    if (!obj.bIsReturning && !isFeared && !isBlinded && ((obj.dwAttackPattern & 1) != 0 || targetId != 0)) {
        std::vector<PlayerData*> nearbyPlayers = GetPlayersInAOI(obj.wPosX, obj.wPosY);
        for (PlayerData* playerPtr : nearbyPlayers) {
            if (!playerPtr) continue;
            PlayerData& player = *playerPtr;
            if (player.dwHpCur == 0) continue;
            if (player.dwInvulnerableUntil > tick) continue; // Death/respawn protection
            if (player.activeBuffs.count(130) > 0) continue; // Ignore players under Turtle Breath (130)
            if (player.activeBuffs.count(178) > 0) continue; // Ignore players under Stealth (178)
            if (targetId != 0 && player.dwObjectID != targetId) continue;

            float dx = (float)player.wPosX - (float)obj.wPosX;
            float dy = (float)player.wPosY - (float)obj.wPosY;
            float distSq = (dx*dx + dy*dy);
            
            // If monster already has aggro (was hit), use larger chase range
            // If scanning for new targets, use normal sight range
            float maxRangeSq;
            if (obj.dwTargetID != 0) {
                float chaseBase = (float)obj.wSightRange * 3.0f;
                float chaseRange = (chaseBase > 100.0f) ? chaseBase : 100.0f;
                maxRangeSq = chaseRange * chaseRange;
            } else {
                maxRangeSq = (float)(obj.wSightRange * obj.wSightRange);
            }
            
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
        targetId = 0;
        obj.dwTargetID = 0;
    }

    // Chase Leash Check: if pulled too far from spawn point, break aggro and return
    if (targetId && bestPlayer) {
        float distToSpawn = std::sqrt(std::pow((float)obj.wPosX - obj.wSpawnX, 2) + std::pow((float)obj.wPosY - obj.wSpawnY, 2));
        float leashRange = (float)(obj.wWanderRange * 3);
        if (leashRange < 40.0f) leashRange = 40.0f;
        
        if (distToSpawn > leashRange) {
            char dbgLeash[256];
            sprintf(dbgLeash, "[MonsterAI] LEASH: ObjID=%u pulled too far (dist=%.1f spawn=%u,%u leash=%.1f). Resetting aggro.",
                obj.dwObjectID, distToSpawn, obj.wSpawnX, obj.wSpawnY, leashRange);
            LOG(std::string(dbgLeash));
            
            targetId = 0;
            obj.dwTargetID = 0;
            bestPlayer = nullptr;
            obj.bIsReturning = true; // Enter returning state
            obj.dwHpCur = obj.dwHpMax; // Heal to full immediately
            
            // Force destination to spawn point and reset move tracking
            obj.wDestX = obj.wSpawnX;
            obj.wDestY = obj.wSpawnY;
            obj.wLastSentDestX = 0;
            obj.wLastSentDestY = 0;
        }
    }

    WORD oldX = obj.wPosX;
    WORD oldY = obj.wPosY;
    


    if (targetId && bestPlayer) {
        obj.dwTargetID = targetId;
        float atkRange = (obj.wShotAtkRange > 0) ? obj.wShotAtkRange : obj.wMeleeAtkRange;
        

        
        if (minDist <= atkRange) {
            // Guard: don't attack dead or invulnerable players
            if (bestPlayer->dwHpCur == 0 || bestPlayer->dwInvulnerableUntil > tick) {
                obj.dwTargetID = 0;
            } else {
            if (obj.wLastSentDestX != 0 || obj.wLastSentDestY != 0) {
                obj.wLastSentDestX = 0;
                obj.wLastSentDestY = 0;
                obj.wLastSentPosX = 0;
                obj.wLastSentPosY = 0;
                
                std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0); 
                DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
                stopBuf.push_back(obj.bObjectType); 
                stopBuf.push_back(oldX & 0xFF); stopBuf.push_back(oldX >> 8);
                stopBuf.push_back(oldY & 0xFF); stopBuf.push_back(oldY >> 8);
                stopBuf.push_back(obj.bHeight);
                stopBuf.push_back(0); 
                
                PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(stopBuf.data(), 0x42); 
                BroadcastPacketAOI_NoLock(oldX, oldY, stopBuf); // Changed to AOI
            }
            
            if (tpl.wAtkInterval > 0 && tick - obj.dwLastAttackTime > tpl.wAtkInterval) { 
                obj.dwLastAttackTime = tick;
                DWORD oid = obj.dwObjectID;
                
                // 1. PREATTACK_ACK
                std::vector<BYTE> preBuf; preBuf.reserve(32);
                preBuf.push_back(obj.bObjectType);
                preBuf.push_back(oid&0xFF); preBuf.push_back((oid>>8)&0xFF); preBuf.push_back((oid>>16)&0xFF); preBuf.push_back(oid>>24);
                preBuf.push_back(oldX & 0xFF); preBuf.push_back(oldX >> 8);
                preBuf.push_back(oldY & 0xFF); preBuf.push_back(oldY >> 8);
                preBuf.push_back(obj.bHeight);
                preBuf.push_back(1); 
                preBuf.push_back(targetId&0xFF); preBuf.push_back((targetId>>8)&0xFF); preBuf.push_back((targetId>>16)&0xFF); preBuf.push_back(targetId>>24);
                preBuf.push_back(0); 
                std::vector<BYTE> preFull; preFull.resize(4); preFull.insert(preFull.end(), preBuf.begin(), preBuf.end());
                PACKET_HEADER* preHead = (PACKET_HEADER*)preFull.data(); preHead->id = 0x4004; preHead->payloadSize = preBuf.size();
                EncryptPacket(preFull.data(), 0x42); 
                BroadcastPacketAOI_NoLock(oldX, oldY, preFull); // Changed to AOI
                
                // 2. ATTACK_ACK 
                std::vector<BYTE> ackBuf; ackBuf.reserve(64);
                ackBuf.push_back(0); // placeholder for bResult, will be overwritten
                ackBuf.push_back(obj.bObjectType);
                ackBuf.push_back(oid&0xFF); ackBuf.push_back((oid>>8)&0xFF); ackBuf.push_back((oid>>16)&0xFF); ackBuf.push_back(oid>>24);
                ackBuf.push_back(oldX & 0xFF); ackBuf.push_back(oldX >> 8);
                ackBuf.push_back(oldY & 0xFF); ackBuf.push_back(oldY >> 8);
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
                        LOG("[MonsterAtk] HIT player " + std::to_string(targetId) + " for " + std::to_string(damage) + " dmg. activeBuffs count=" + std::to_string(player.activeBuffs.size()) + " has101=" + std::to_string(player.activeBuffs.count(101)));

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
                    if (m_players.count(targetId) && m_players[targetId].bIsBunsin) {
                        // 分身死亡：广播 DIE + 从地图和追踪表中清理
                        DWORD ownerCharID = m_players[targetId].dwOwnerID;
                        CleanupSingleBunsin(ownerCharID, targetId, m_dwMapID);
                        RemovePlayer(targetId);
                        obj.dwTargetID = 0;
                        for (auto& mPair : m_monsters) {
                            if (mPair.second.dwTargetID == targetId) mPair.second.dwTargetID = 0;
                        }
                        LOG("[BunsinDeath] Bunsin " + std::to_string(targetId) + " killed by monster.");
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
                        obj.dwTargetID = 0;
                    
                    // Release ALL monsters that were targeting this dead player
                    for (auto& mPair : m_monsters) {
                        if (mPair.second.dwTargetID == targetId) {
                            mPair.second.dwTargetID = 0;
                        }
                    }
                    
                    LOG("[PlayerDeath] Player " + std::to_string(targetId) + " HP=0, deferred death. Released all monster locks.");
                    } // end else (real player death)
                }
            }
            } // end invulnerability else
        } else if (!isRooted) {
            // Move Towards (CHASE) —— 定身状态跳过移动
            float dx = (float)bestPlayer->wPosX - (float)obj.wPosX;
            float dy = (float)bestPlayer->wPosY - (float)obj.wPosY;
            float dirX = dx / minDist;
            float dirY = dy / minDist;
            
            float angle = std::atan2(dy, dx) * 180.0f / 3.14159265f;
            if (angle < 0) angle += 360.0f;
            WORD wDirection = (WORD)angle;
            
            float dbSpeed = (float)tpl.wWalkSpeed100 / 100.0f; 
            if (dbSpeed < 1.0f) dbSpeed = 3.0f; 
            float step = dbSpeed / 10.0f; 
            
            obj.fPosX += dirX * step;
            obj.fPosY += dirY * step;
            
            WORD nx = (WORD)obj.fPosX;
            WORD ny = (WORD)obj.fPosY;
            
            auto Walkable = [&](WORD tx, WORD ty) -> bool {
                if (m_collisionGrid.empty()) return true;
                if (tx >= m_width || ty >= m_height) return false;
                return m_collisionGrid[ty * m_width + tx] == 0;
            };

            bool canMove = Walkable(nx, ny);
            
            // DEBUG: log chase movement for type 187
            if (obj.bPropType == 187) {
                char dbg4[256]; sprintf(dbg4, "[MonsterAI] CHASE: pos(%.1f,%.1f)->(%d,%d) step=%.2f speed=%.1f canMove=%d collGridSize=%d",
                    obj.fPosX, obj.fPosY, nx, ny, step, dbSpeed, canMove ? 1 : 0, (int)m_collisionGrid.size());
                LOG(std::string(dbg4));
            }
            
            if (canMove) {
                int ox = obj.wPosX, oy = obj.wPosY;
                obj.wPosX = nx; obj.wPosY = ny;
                UpdateMonsterGrid(obj.dwObjectID, ox, oy, nx, ny);
                
                // 投射目的地：不使用玩家坐标（太近导致客户端提前到达停步），
                // 而是沿追击方向投射 40 像素远的虚拟终点，让客户端持续行走
                WORD projDestX = (WORD)(obj.wPosX + dirX * 40.0f);
                WORD projDestY = (WORD)(obj.wPosY + dirY * 40.0f);
                obj.wDestX = projDestX;
                obj.wDestY = projDestY;

                // 追击发包策略（配合客户端动画防重入，彻底消除小碎步）：
                //   条件1: 首次开始移动（之前是停步状态）→ 立即发包触发 Walk 动画
                //   条件2: 方向变化超过 25° → 立即发包更新追击方向
                //   条件3: 距上次发包 ≥800ms → 定时校正（位置+方向同步）
                bool isFirstMove = (obj.wLastSentDestX == 0 && obj.wLastSentDestY == 0);

                // 方向变化判定：计算当前方向与上次发包方向的角度差（处理 0°/360° 跨界）
                int angleDiff = abs((int)wDirection - (int)obj.wLastSentDirection);
                if (angleDiff > 180) angleDiff = 360 - angleDiff;
                bool directionChanged = (angleDiff > 25);

                bool timePassed = (tick - obj.dwLastMoveSendTime >= 800);

                if (isFirstMove || directionChanged || timePassed) {
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
                    ackBuf.push_back(projDestX & 0xFF); ackBuf.push_back(projDestX >> 8);
                    ackBuf.push_back(projDestY & 0xFF); ackBuf.push_back(projDestY >> 8);
                    ackBuf.push_back(obj.bHeight); 
                    ackBuf.push_back(wDirection & 0xFF); ackBuf.push_back(wDirection >> 8); 
                    ackBuf.push_back(20); 
                    ackBuf.push_back((BYTE)dbSpeed); 
                    
                    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3508; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                    EncryptPacket(ackBuf.data(), 0x42); 
                    BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, ackBuf);
                }
            } else {
                // Slide Collision Option B
                bool slid = false;
                // Try X-only slide
                WORD testX = (WORD)(obj.wPosX + (dirX > 0 ? 1 : (dirX < 0 ? -1 : 0)));
                WORD testY = obj.wPosY;
                if (testX != obj.wPosX && Walkable(testX, testY)) {
                    int ox = obj.wPosX, oy = obj.wPosY;
                    obj.wPosX = testX;
                    obj.fPosX = (float)testX;
                    obj.fPosY = (float)obj.wPosY; // clamp Y
                    UpdateMonsterGrid(obj.dwObjectID, ox, oy, testX, testY);
                    slid = true;
                }
                // Try Y-only slide
                else {
                    testX = obj.wPosX;
                    testY = (WORD)(obj.wPosY + (dirY > 0 ? 1 : (dirY < 0 ? -1 : 0)));
                    if (testY != obj.wPosY && Walkable(testX, testY)) {
                        int ox = obj.wPosX, oy = obj.wPosY;
                        obj.wPosY = testY;
                        obj.fPosY = (float)testY;
                        obj.fPosX = (float)obj.wPosX; // clamp X
                        UpdateMonsterGrid(obj.dwObjectID, ox, oy, testX, testY);
                        slid = true;
                    }
                }

                if (slid) {
                    WORD projDestX = (WORD)(obj.wPosX + dirX * 40.0f);
                    WORD projDestY = (WORD)(obj.wPosY + dirY * 40.0f);
                    obj.wDestX = projDestX;
                    obj.wDestY = projDestY;

                    // 滑行追击发包：与正常追击使用一致的方向变化+时间门控
                    int angleDiff2 = abs((int)wDirection - (int)obj.wLastSentDirection);
                    if (angleDiff2 > 180) angleDiff2 = 360 - angleDiff2;
                    bool dirChanged = (angleDiff2 > 25);
                    bool tPassed = (tick - obj.dwLastMoveSendTime >= 800);
                    bool firstMove = (obj.wLastSentDestX == 0 && obj.wLastSentDestY == 0);

                    if (firstMove || dirChanged || tPassed) {
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
                        ackBuf.push_back(projDestX & 0xFF); ackBuf.push_back(projDestX >> 8);
                        ackBuf.push_back(projDestY & 0xFF); ackBuf.push_back(projDestY >> 8);
                        ackBuf.push_back(obj.bHeight); 
                        ackBuf.push_back(wDirection & 0xFF); ackBuf.push_back(wDirection >> 8); 
                        ackBuf.push_back(20); 
                        ackBuf.push_back((BYTE)dbSpeed); 
                        
                        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3508; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                        EncryptPacket(ackBuf.data(), 0x42); 
                        BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, ackBuf);
                    }
                } else {
                    // Fully blocked: clamp float coords back to grid cell
                    obj.fPosX = (float)obj.wPosX;
                    obj.fPosY = (float)obj.wPosY;
                }
            }
        }
    } else {
        // Return to Spawn or Wander
        if (obj.bPropType >= 224 && obj.bPropType <= 231) return; 
        if (obj.bPropType == 255) return; 
        if (obj.dwMovePattern == 1) return; 

        int range = obj.wWanderRange;
        if (range <= 0) return; 

        float distToSpawn = std::sqrt(std::pow((float)obj.wPosX - obj.wSpawnX, 2) + std::pow((float)obj.wPosY - obj.wSpawnY, 2));

        if (obj.bIsReturning) {
            // Force destination to spawn point
            obj.wDestX = obj.wSpawnX;
            obj.wDestY = obj.wSpawnY;
            
            // If we have returned close to spawn (within 2 tiles), clear returning state
            if (distToSpawn <= 2.0f) {
                obj.bIsReturning = false;
                obj.wDestX = 0;
                obj.wDestY = 0;
                obj.wLastSentDestX = 0;
                obj.wLastSentDestY = 0;
                
                LOG("[MonsterAI] LEASH RETURN COMPLETED: ObjID=" + std::to_string(obj.dwObjectID) + " returned to spawn.");
            }
        } else {
            if (distToSpawn > range) {
                obj.wDestX = obj.wSpawnX;
                obj.wDestY = obj.wSpawnY;
            } else if (obj.wDestX == 0 && obj.wDestY == 0) {
                if (tick - obj.dwLastAttackTime > (DWORD)(2000 + rand() % 3000)) { 
                    if (tpl.bIdleRatio > 0 && (rand() % 100) < tpl.bIdleRatio) {
                        obj.dwLastAttackTime = tick; 
                    } else {
                        obj.wDestX = obj.wSpawnX + (rand() % (range * 2)) - range;
                        obj.wDestY = obj.wSpawnY + (rand() % (range * 2)) - range;
                        obj.dwLastAttackTime = tick;
                    }
                }
            }
        }

        if (obj.wDestX != 0 || obj.wDestY != 0) {
            float distToDest = std::sqrt(std::pow((float)obj.wPosX - obj.wDestX, 2) + std::pow((float)obj.wPosY - obj.wDestY, 2));
            float dbSpeed = (float)tpl.wWalkSpeed100 / 100.0f;
            if (dbSpeed < 1.0f) dbSpeed = 3.0f;
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
                obj.dwLastAttackTime = tick; 
                
                std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0);
                DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
                stopBuf.push_back(obj.bObjectType); 
                stopBuf.push_back(obj.wPosX & 0xFF); stopBuf.push_back(obj.wPosX >> 8);
                stopBuf.push_back(obj.wPosY & 0xFF); stopBuf.push_back(obj.wPosY >> 8);
                stopBuf.push_back(obj.bHeight);
                stopBuf.push_back(0); 
                PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(stopBuf.data(), 0x42); 
                BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, stopBuf); // Changed to AOI
            } else {
                float dirX = ((float)obj.wDestX - (float)obj.wPosX) / distToDest;
                float dirY = ((float)obj.wDestY - (float)obj.wPosY) / distToDest;
                
                float angle = std::atan2(dirY, dirX) * 180.0f / 3.14159265f;
                if (angle < 0) angle += 360.0f;
                WORD wDirection = (WORD)angle;
                
                obj.fPosX += dirX * step;
                obj.fPosY += dirY * step;
                
                WORD nx = (WORD)obj.fPosX;
                WORD ny = (WORD)obj.fPosY;
                
                auto Walkable = [&](WORD tx, WORD ty) -> bool {
                    if (m_collisionGrid.empty()) return true;
                    if (tx >= m_width || ty >= m_height) return false;
                    return m_collisionGrid[ty * m_width + tx] == 0;
                };

                bool canMove = Walkable(nx, ny);
                
                if (canMove) {
                    int ox = obj.wPosX, oy = obj.wPosY;
                    obj.wPosX = nx; obj.wPosY = ny;
                    UpdateMonsterGrid(obj.dwObjectID, ox, oy, nx, ny);
                    
                    // 漫游发包：目的地固定，只在首次设定或目的地变化时发一次起步包
                    // 客户端自行按速度插值走到终点，中间不需要额外包
                    bool wanderNeedSend = (obj.wDestX != obj.wLastSentDestX || obj.wDestY != obj.wLastSentDestY);
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
                        ackBuf.push_back(20); 
                        ackBuf.push_back((BYTE)dbSpeed); 
                        
                        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3508; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                        EncryptPacket(ackBuf.data(), 0x42); 
                        BroadcastPacketAOI_NoLock(obj.wPosX, obj.wPosY, ackBuf);
                    }
                } else {
                    obj.wDestX = 0;
                    obj.wDestY = 0;
                    obj.wLastSentDestX = 0;
                    obj.wLastSentDestY = 0;
                    // Clamp float coords to grid to prevent drift on wander collision
                    obj.fPosX = (float)obj.wPosX;
                    obj.fPosY = (float)obj.wPosY;
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

    // 1. 生成怪物掉落物品
    DropManager::GetInstance()->GenerateDrops(casterID, obj);

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
    DWORD attackerCharID = casterID - 400000000;
    if (attackerCharID > 0 && attackerCharID < 400000000) {
        DWORD deadExp = obj.dwExp;
        DWORD targetFiveElmExp = g_NpcTemplates.count(obj.bPropType) ? g_NpcTemplates[obj.bPropType].wFiveElmExp : 0;
        targetFiveElmExp += obj.wIncFiveElmExp;

        bool needRefresh = GrantExpToPlayer(attackerCharID, deadExp, targetFiveElmExp);
        if (needRefresh) {
            SOCKET clientSocket = SessionMgr::GetInstance().GetSocketByCharID(attackerCharID);
            if (clientSocket != INVALID_SOCKET && clientSocket != 0) {
                UpdatePlayerStatsAndSend(clientSocket, attackerCharID);

                // 升级后将在场角色的 HP/IP 回满并刷新数据库状态
                DWORD dwObjID = attackerCharID + 400000000;
                PlayerData* pCaster = GetPlayer(dwObjID);
                if (pCaster) {
                    pCaster->dwHpCur = pCaster->dwHpMax;
                    pCaster->wIpCur = pCaster->wIpMax;
                }
                CharacterDB::GetInstance().RestoreHpIpToMax(attackerCharID);
            }
        }
    }
}

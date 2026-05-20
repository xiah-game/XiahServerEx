#include "MapInstance.h"
#include "../Network/SessionMgr.h"
#include <cmath>
#include <unordered_set>



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
    // 0. Process Deferred Player Death Broadcasts
    // -----------------------------------------------------
    for (auto& pair : m_players) {
        PlayerData& pl = pair.second;
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
    // 3. Process Monster AI
    // -----------------------------------------------------
    for (auto& pair : m_monsters) {
        ProcessMonsterAI(tick, pair.second);
    }
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
        
        float speed = (float)(p.wWalkSpeed > 0 ? p.wWalkSpeed : 11) * 0.1f * 0.65f;
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
                BroadcastPacket(leaveBuf);
                obj.wPosX = 0;
            }
            
            DWORD regenTimeMs = tpl.wRegen * 1000;
            if (regenTimeMs == 0) regenTimeMs = 10000; // Fallback
            
            if (tick - obj.dwDeadTime >= regenTimeMs) {
                obj.dwHpCur = obj.dwHpMax;
                obj.dwTargetID = 0;
                obj.dwDeadTime = 0;
                
                // Keep the same ObjectID on respawn.
                // m_monsters uses dwObjectID as the map key — changing it without
                // re-inserting would break GetMonster() lookups, making the monster invincible.
                
                int offsetX = 0, offsetY = 0;
                if (obj.wSpawnRange > 0) {
                    offsetX = (rand() % (obj.wSpawnRange * 2 + 1)) - obj.wSpawnRange;
                    offsetY = (rand() % (obj.wSpawnRange * 2 + 1)) - obj.wSpawnRange;
                }
                obj.wPosX = obj.wSpawnX + offsetX;
                if (obj.wPosX < 0) obj.wPosX = 0;
                obj.wPosY = obj.wSpawnY + offsetY;
                if (obj.wPosY < 0) obj.wPosY = 0;
                obj.fPosX = (float)obj.wPosX;
                obj.fPosY = (float)obj.wPosY;
                
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
                    BroadcastPacket(infoFull);
                }
            }
        }
        return;
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

    // FIND TARGET (Only checks players in THIS map now, not all active players globally!)
    if ((obj.dwAttackPattern & 1) != 0 || targetId != 0) {
        for (auto& pair : m_players) {
            PlayerData& player = pair.second;
            if (player.dwHpCur == 0) continue;
            if (player.dwInvulnerableUntil > tick) continue; // Death/respawn protection
            if (player.activeBuffs.count(130) > 0) continue; // Ignore players under Turtle Breath (130)
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
                
                std::vector<BYTE> stopBuf; stopBuf.resize(4); stopBuf.push_back(0); 
                DWORD oid = obj.dwObjectID; stopBuf.push_back(oid&0xFF); stopBuf.push_back((oid>>8)&0xFF); stopBuf.push_back((oid>>16)&0xFF); stopBuf.push_back(oid>>24);
                stopBuf.push_back(obj.bObjectType); 
                stopBuf.push_back(oldX & 0xFF); stopBuf.push_back(oldX >> 8);
                stopBuf.push_back(oldY & 0xFF); stopBuf.push_back(oldY >> 8);
                stopBuf.push_back(obj.bHeight);
                stopBuf.push_back(0); 
                
                PACKET_HEADER* stopHead = (PACKET_HEADER*)stopBuf.data(); stopHead->id = 0x350C; stopHead->payloadSize = stopBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(stopBuf.data(), 0x42); 
                BroadcastPacket(stopBuf);
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
                BroadcastPacket(preFull);
                
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
                    float roll = (float)(rand() % 10000) / 10000.0f;
                    
                    if (playerDodge > 0 && roll > hitChance) {
                        // MISS
                        bResult = 1;
                        damage = 0;
                    } else {
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
                    }
                    
                    if (bResult == 2) {
                        if (playerHpCur > damage) playerHpCur -= damage;
                        else playerHpCur = 0;
                    }
                    
                    player.dwHpCur = playerHpCur;
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
                if (m_players.count(targetId)) {
                    PlayerData& targetPlayer = m_players[targetId];
                    DWORD charID = targetId - 400000000;
                    
                    std::vector<BYTE> hpBuf(4);
                    auto push4hp = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                    auto push2hp = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                    push4hp(playerHpMax);
                    push4hp(playerHpCur);
                    push2hp(targetPlayer.wIpMax);
                    push2hp(targetPlayer.wIpCur);
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
                }
            }
            } // end invulnerability else
        } else {
            // Move Towards (CHASE)
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
            
            bool canMove = true;
            if (m_collisionGrid.size() > 0 && nx < m_width && ny < m_height && nx >= 0 && ny >= 0) {
                if (m_collisionGrid[ny * m_width + nx] != 0) canMove = false;
            }
            
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
                
                // Chase destination = player position (monster walks towards player)
                obj.wDestX = bestPlayer->wPosX;
                obj.wDestY = bestPlayer->wPosY;

                // Only send move packet when destination changes significantly (player moved)
                if (abs(obj.wDestX - obj.wLastSentDestX) > 3 || abs(obj.wDestY - obj.wLastSentDestY) > 3) {
                    obj.wLastSentDestX = obj.wDestX;
                    obj.wLastSentDestY = obj.wDestY;

                    std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); 
                    DWORD oid = obj.dwObjectID; ackBuf.push_back(oid&0xFF); ackBuf.push_back((oid>>8)&0xFF); ackBuf.push_back((oid>>16)&0xFF); ackBuf.push_back(oid>>24);
                    ackBuf.push_back(obj.bObjectType); 
                    ackBuf.push_back(oldX & 0xFF); ackBuf.push_back(oldX >> 8);
                    ackBuf.push_back(oldY & 0xFF); ackBuf.push_back(oldY >> 8);
                    ackBuf.push_back(obj.bHeight);
                    ackBuf.push_back(obj.wDestX & 0xFF); ackBuf.push_back(obj.wDestX >> 8);
                    ackBuf.push_back(obj.wDestY & 0xFF); ackBuf.push_back(obj.wDestY >> 8);
                    ackBuf.push_back(obj.bHeight); 
                    ackBuf.push_back(wDirection & 0xFF); ackBuf.push_back(wDirection >> 8); 
                    ackBuf.push_back(20); 
                    ackBuf.push_back((BYTE)dbSpeed); 
                    
                    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3508; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                    EncryptPacket(ackBuf.data(), 0x42); 
                    BroadcastPacket(ackBuf);
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
                BroadcastPacket(stopBuf);
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
                
                bool canMove = true;
                if (m_collisionGrid.size() > 0 && nx < m_width && ny < m_height && nx >= 0 && ny >= 0) {
                    if (m_collisionGrid[ny * m_width + nx] != 0) canMove = false;
                }
                
                if (canMove) {
                    int ox = obj.wPosX, oy = obj.wPosY;
                    obj.wPosX = nx; obj.wPosY = ny;
                    UpdateMonsterGrid(obj.dwObjectID, ox, oy, nx, ny);
                    
                    if (obj.wDestX != obj.wLastSentDestX || obj.wDestY != obj.wLastSentDestY) {
                        obj.wLastSentDestX = obj.wDestX;
                        obj.wLastSentDestY = obj.wDestY;

                        std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); 
                        DWORD oid = obj.dwObjectID; ackBuf.push_back(oid&0xFF); ackBuf.push_back((oid>>8)&0xFF); ackBuf.push_back((oid>>16)&0xFF); ackBuf.push_back(oid>>24);
                        ackBuf.push_back(obj.bObjectType); 
                        ackBuf.push_back(oldX & 0xFF); ackBuf.push_back(oldX >> 8);
                        ackBuf.push_back(oldY & 0xFF); ackBuf.push_back(oldY >> 8);
                        ackBuf.push_back(obj.bHeight);
                        ackBuf.push_back(obj.wDestX & 0xFF); ackBuf.push_back(obj.wDestX >> 8);
                        ackBuf.push_back(obj.wDestY & 0xFF); ackBuf.push_back(obj.wDestY >> 8);
                        ackBuf.push_back(obj.bHeight); 
                        ackBuf.push_back(wDirection & 0xFF); ackBuf.push_back(wDirection >> 8); 
                        ackBuf.push_back(20); 
                        ackBuf.push_back((BYTE)dbSpeed); 
                        
                        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3508; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                        EncryptPacket(ackBuf.data(), 0x42); 
                        BroadcastPacket(ackBuf);
                    }
                } else {
                    obj.wDestX = 0;
                    obj.wDestY = 0;
                    obj.wLastSentDestX = 0;
                    obj.wLastSentDestY = 0;
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

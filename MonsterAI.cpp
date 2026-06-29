#include "MonsterAI.h"
#include "Handlers/PartyHandler.h"
#include "Network/SessionMgr.h"
#include "Network/PacketRouter.h"
#include "GameObjects/MapInstance.h"
#include "GameObjects/PlayerManager.h"
#include "GameObjects/ExpSystem.h"
#include "GameObjects/DropManager.h"
#include "GameObjects/MugongManager.h"
#include "Handlers/MugongHandler.h"
#include <cmath>
#include <thread>
#include <time.h>
#include <set>
#include <unordered_set>

void MonsterAIWorker(int workerId, int totalWorkers) {
    srand((unsigned int)time(NULL) ^ workerId);
    while (true) {
        Sleep(100); // 100ms tick for responsive AI
        DWORD tick = GetTickCount();

        static DWORD s_lastLog = 0;
        if (workerId == 0 && tick - s_lastLog > 10000) {
            s_lastLog = tick;
            LOG("[UnitSvr AI] Tick. Maps active: " + std::to_string(g_MapInstances.size()));
        }

        int idx = 0;
        for (auto& pair : g_MapInstances) {
            if (idx % totalWorkers == workerId) {
                if (pair.second) {
                    struct SyncToSend {
                        CMapInstance::PendingSyncMove move;
                        std::unordered_set<DWORD> aoiPlayerIDs;
                    };
                    std::vector<SyncToSend> syncsToSend;
                    DWORD mapID = pair.first;
                    {
                        std::lock_guard<std::mutex> lock(pair.second->GetMutex());
                        pair.second->Update(tick);
                        for (const auto& sm : pair.second->m_pendingSyncs) {
                            SyncToSend sts;
                            sts.move = sm;
                            // 业务设计意图：在持地图锁期间，收集该移动玩家当前坐标九宫格范围内的其他在线玩家，用以在锁外进行精确局部广播，从而在保证并发性能的同时彻底消灭全图广播负载。
                            std::vector<PlayerData*> aoiPlayers = pair.second->GetPlayersInAOI(sm.wPosX, sm.wPosY);
                            for (auto* p : aoiPlayers) {
                                if (p->dwObjectID != sm.dwObjectID) {
                                    sts.aoiPlayerIDs.insert(p->dwObjectID);
                                }
                            }
                            syncsToSend.push_back(sts);
                        }
                        pair.second->m_pendingSyncs.clear();
                    }
                    
                    // Broadcast pending sync moves outside map mutex to prevent deadlock
                    if (!syncsToSend.empty()) {
                        for (const auto& sts : syncsToSend) {
                            const auto& sm = sts.move;
                            // Build SYNCMOVE_ACK (0x430E)
                            std::vector<BYTE> buf; buf.resize(4);
                            buf.push_back(0); // bResult
                            buf.push_back(sm.dwObjectID & 0xFF); buf.push_back((sm.dwObjectID>>8)&0xFF); buf.push_back((sm.dwObjectID>>16)&0xFF); buf.push_back((sm.dwObjectID>>24)&0xFF);
                            // 意图：NV_SYNCMOVE_ACK 协议不包含 bObjectType，直接追加坐标以和客户端 OnCS_NV_SYNCMOVE_ACK 对齐，消除 1 字节位移
                            buf.push_back(sm.wPosX & 0xFF); buf.push_back(sm.wPosX >> 8);
                            buf.push_back(sm.wPosY & 0xFF); buf.push_back(sm.wPosY >> 8);
                            buf.push_back(sm.bHeight);
                            buf.push_back(sm.wDesX & 0xFF); buf.push_back(sm.wDesX >> 8);
                            buf.push_back(sm.wDesY & 0xFF); buf.push_back(sm.wDesY >> 8);
                            buf.push_back(sm.bDesH);
                            buf.push_back(sm.wDirection & 0xFF); buf.push_back(sm.wDirection >> 8);
                            buf.push_back(0); // bStatus
                            buf.push_back(sm.bSpeed);
                            WORD wDiffTime = 150;
                            buf.push_back(wDiffTime & 0xFF); buf.push_back(wDiffTime >> 8);
                            
                            PACKET_HEADER* h = (PACKET_HEADER*)buf.data();
                            h->id = 0x430E; // CS_NV_SYNCMOVE_ACK
                            h->payloadSize = buf.size() - sizeof(PACKET_HEADER);
                            EncryptPacket(buf.data(), 0x42);
                            
                            // 业务设计意图：局部广播——仅发送给处于该玩家九宫格 AOI 范围内的其他在线玩家，彻底消灭全图广播负载，节省服务器网络带宽。
                            if (!sts.aoiPlayerIDs.empty()) {
                                SessionMgr::GetInstance().SendToObjectIDs(sts.aoiPlayerIDs, mapID, buf, INVALID_SOCKET);
                            }
                        }
                    }

                    // === Player HP/IP Auto-Recovery (every 5 seconds) ===
                    struct RegenInfo {
                        DWORD dwObjectID;
                        DWORD dwHpMax, dwHpCur;
                        DWORD wIpMax, wIpCur;
                    };
                    std::vector<RegenInfo> regenPackets;
                    {
                        std::lock_guard<std::mutex> lock(pair.second->GetMutex());
                        auto& players = pair.second->GetPlayers();
                        for (auto& pp : players) {
                            PlayerData& pl = pp.second;
                            if (pl.bObjectType != 1) continue; // only players
                            if (pl.bIsBunsin) continue; // 分身不需要自然回血
                            if (pl.dwHpCur == 0) continue; // do not regen dead players
                            if (tick - pl.dwLastRegenTime < 5000) continue; // not yet 5 seconds
                            
                            pl.dwLastRegenTime = tick;

                            // 动态计算该周期的恢复速率总量
                            DWORD totalRegenHp = 0;
                            DWORD totalRegenIp = 0;

                            // 1. 基础自然回复 (生命上限的 1% + 2, 内力上限的 1% + 1)
                            totalRegenHp += (pl.dwHpMax * 1 / 100) + 2;
                            totalRegenIp += (pl.wIpMax * 1 / 100) + 1;

                            // 2. 装备带来的生命内力恢复属性
                            totalRegenHp += pl.wEquipRestoreHp;
                            totalRegenIp += pl.wEquipRestoreIp;

                            // 3. 学习的被动回复技能带来的加成 (例如 6 洗髓经被动回血, 7 易筋经被动回蓝)
                            for (auto& mg : pl.learnedMugongs) {
                                DWORD mugID = mg.first;
                                BYTE mugLvl = mg.second;
                                if (mugID >= 1 && mugID <= 29) {
                                    sMugongList* pd = MugongManager::GetInstance()->GetMugongLevelData(mugID, mugLvl);
                                    if (pd) {
                                        totalRegenHp += pd->wRecoverHp;
                                        totalRegenIp += pd->wRecoverIp;
                                        if (pd->wRecoverHpPerc > 100) {
                                            totalRegenHp += (pl.dwHpMax * (pd->wRecoverHpPerc - 100) / 100);
                                        }
                                        if (pd->wRecoverIpPerc > 100) {
                                            totalRegenIp += (pl.wIpMax * (pd->wRecoverIpPerc - 100) / 100);
                                        }
                                    }
                                }
                            }

                            // 4. 当前生效的 Active Buff 状态回复
                            for (auto& bf : pl.activeBuffs) {
                                sMugongList* bd = MugongManager::GetInstance()->GetMugongLevelData(bf.second.dwMugongID, bf.second.bLevel);
                                if (bd) {
                                    totalRegenHp += bd->wRecoverHp;
                                    totalRegenIp += bd->wRecoverIp;
                                    if (bd->wRecoverHpPerc > 100) {
                                        totalRegenHp += (pl.dwHpMax * (bd->wRecoverHpPerc - 100) / 100);
                                    }
                                    if (bd->wRecoverIpPerc > 100) {
                                        totalRegenIp += (pl.wIpMax * (bd->wRecoverIpPerc - 100) / 100);
                                    }
                                }
                            }

                            // 若无任何加成，或血蓝已经完全回满，则不需要重算和发送同步数据
                            if (totalRegenHp == 0 && totalRegenIp == 0) continue;
                            if (pl.dwHpCur >= pl.dwHpMax && pl.wIpCur >= pl.wIpMax) continue;

                            bool changed = false;
                            DWORD oldHp = pl.dwHpCur; DWORD oldIp = pl.wIpCur;
                            
                            if (pl.dwHpCur < pl.dwHpMax && totalRegenHp > 0) {
                                pl.dwHpCur += totalRegenHp;
                                if (pl.dwHpCur > pl.dwHpMax) pl.dwHpCur = pl.dwHpMax;
                                changed = true;
                            }
                            if (pl.wIpCur < pl.wIpMax && totalRegenIp > 0) {
                                pl.wIpCur += totalRegenIp;
                                if (pl.wIpCur > pl.wIpMax) pl.wIpCur = pl.wIpMax;
                                changed = true;
                            }
                            
                            LOG("[Regen] ObjID=" + std::to_string(pl.dwObjectID) + " HP:" + std::to_string(oldHp) + "/" + std::to_string(pl.dwHpMax) + "->" + std::to_string(pl.dwHpCur) + " IP:" + std::to_string(oldIp) + "/" + std::to_string(pl.wIpMax) + "->" + std::to_string(pl.wIpCur) + " changed=" + std::to_string(changed));
                            
                            if (changed) {
                                RegenInfo ri;
                                ri.dwObjectID = pl.dwObjectID;
                                ri.dwHpMax = pl.dwHpMax;
                                ri.dwHpCur = pl.dwHpCur;
                                ri.wIpMax = pl.wIpMax;
                                ri.wIpCur = pl.wIpCur;
                                regenPackets.push_back(ri);
                            }
                        }
                    }
                    // Send regen packets outside map mutex
                    if (!regenPackets.empty()) {
                        for (const auto& ri : regenPackets) {
                            DWORD charID = ri.dwObjectID - 400000000;
                            SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(charID);
                            if (targetSock == INVALID_SOCKET) continue;
                            
                            std::vector<BYTE> hpBuf(4);
                            auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                            push4(ri.dwHpMax);
                            push4(ri.dwHpCur);
                            push4(ri.wIpMax);
                            push4(ri.wIpCur);
                            hpBuf.push_back(0); // bType = 0 (auto recovery, no effect)
                            PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                            hpHead->id = 0x3B0D; // CS_IF_CHARHP_ACK
                            hpHead->payloadSize = hpBuf.size() - 4;
                            EncryptPacket(hpBuf.data(), 0x42);
                            SafeSend(targetSock, (const char*)hpBuf.data(), hpBuf.size(), 0);
                        }
                    }

                    // === BUFF Expiry Check (every tick) ===
                    struct ExpiredBuff {
                        DWORD dwObjectID;
                        DWORD dwCharID;
                        DWORD dwMugongID;
                        BYTE bLevel;
                    };
                    std::vector<ExpiredBuff> expiredBuffs;
                    std::vector<DWORD> expiredSpirits;
                    {
                        std::lock_guard<std::mutex> lock(pair.second->GetMutex());
                        auto& players = pair.second->GetPlayers();
                        for (auto& pp : players) {
                            sServerObject& pl = pp.second;
                            if (pl.bObjectType != 1) continue;

                            // 检查暴气状态是否超时
                            if (pl.bSpiritActive && tick >= pl.dwSpiritEndTime) {
                                pl.bSpiritActive = false;
                                expiredSpirits.push_back(pl.dwObjectID);
                            }

                            bool hasIpRegenSync = false;
                            for (auto it = pl.activeBuffs.begin(); it != pl.activeBuffs.end(); ) {
                                if (tick >= it->second.dwEndTime) {
                                    ExpiredBuff eb;
                                    eb.dwObjectID = pl.dwObjectID;
                                    eb.dwCharID = pl.dwObjectID - 400000000;
                                    eb.dwMugongID = it->second.dwMugongID;
                                    eb.bLevel = it->second.bLevel;
                                    expiredBuffs.push_back(eb);
                                    LOG("[BuffExpiry] Buff " + std::to_string(it->second.dwMugongID) + " expired on player " + std::to_string(pl.dwObjectID));
                                    it = pl.activeBuffs.erase(it);
                                } else {
                                    // 业务设计意图：处理玩家身上的持续减蓝 Buff (bKind=22) Tick 结算。
                                    // 每 1000 毫秒扣减玩家的 wIpCur，并发送 0x3B0D (CS_IF_CHARHP_ACK) 刷新客户端自身的蓝条。
                                    sMugongTemplate* bfTpl = MugongManager::GetInstance()->GetTemplate(it->second.dwMugongID);
                                    if (bfTpl && bfTpl->bType == 4 && bfTpl->bKind == 22 && it->second.bIsDebuff) {
                                        sMugongList* dotData = MugongManager::GetInstance()->GetMugongLevelData(it->second.dwMugongID, it->second.bLevel);
                                        if (dotData) {
                                            DWORD tickInterval = 1000; // 每秒 Tick
                                            if (tick - it->second.dwLastTickTime >= tickInterval) {
                                                it->second.dwLastTickTime = tick;
                                                DWORD drain = (dotData->nEtc1 > 0) ? dotData->nEtc1 : 1;
                                                if (pl.wIpCur > drain) {
                                                    pl.wIpCur -= drain;
                                                } else {
                                                    pl.wIpCur = 0;
                                                }
                                                hasIpRegenSync = true;
                                                LOG("[MpDot-Tick] Player " + std::to_string(pl.dwObjectID) + " drained " + std::to_string(drain) + " IP, current IP: " + std::to_string(pl.wIpCur));
                                            }
                                        }
                                    }
                                    ++it;
                                }
                            }

                            if (hasIpRegenSync) {
                                DWORD charID = pl.dwObjectID - 400000000;
                                SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(charID);
                                if (targetSock != INVALID_SOCKET) {
                                    std::vector<BYTE> hpBuf(4);
                                    auto push4 = [&](DWORD d) { hpBuf.push_back(d&0xFF); hpBuf.push_back((d>>8)&0xFF); hpBuf.push_back((d>>16)&0xFF); hpBuf.push_back((d>>24)&0xFF); };
                                    push4(pl.dwHpMax);
                                    push4(pl.dwHpCur);
                                    push4(pl.wIpMax);
                                    push4(pl.wIpCur);
                                    hpBuf.push_back(0); // bType = 0 (auto recovery, no effect)
                                    PACKET_HEADER* hpHead = (PACKET_HEADER*)hpBuf.data();
                                    hpHead->id = 0x3B0D; // CS_IF_CHARHP_ACK
                                    hpHead->payloadSize = hpBuf.size() - 4;
                                    EncryptPacket(hpBuf.data(), 0x42);
                                    SafeSend(targetSock, (const char*)hpBuf.data(), hpBuf.size(), 0);
                                }
                            }
                        }
                    }
                    // Send buff end packets & refresh stats outside map mutex
                    if (!expiredSpirits.empty()) {
                        for (DWORD dwObjID : expiredSpirits) {
                            std::vector<BYTE> buf;
                            buf.resize(4, 0); // Header placeholder
                            buf.push_back(11); // bType = 11 (Spirit/Potion)
                            
                            auto pushDWord = [&](DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back(d >> 24); };
                            auto pushString = [&](const std::string& str) {
                                WORD len = (WORD)str.length();
                                buf.push_back(len & 0xFF); buf.push_back(len >> 8);
                                for (char c : str) buf.push_back(c);
                            };
                            
                            pushDWord(dwObjID);
                            pushDWord(1); // dwData1 -> bInstanceType = 1 (Spirit)
                            pushDWord(0); // dwData2 -> 0 (Stop spirit)
                            pushDWord(0); // dwData3 -> 0
                            pushString(""); pushString(""); pushString("");

                            WORD packetID = 0x3F10; // CS_CD_CHARUPDATE_ACK
                            WORD payloadSize = (WORD)(buf.size() - 4);
                            memcpy(&buf[0], &packetID, 2);
                            memcpy(&buf[2], &payloadSize, 2);
                            EncryptPacket(buf.data(), 0x42);

                            SessionMgr::GetInstance().BroadcastToMap(mapID, buf);

                            DWORD charID = dwObjID - 400000000;
                            PlayerManager::GetInstance().RecalculateStats(charID, true);
                            LOG("[SpiritExpiry] Spirit expired on player dwObjectID=" + std::to_string(dwObjID));
                        }
                    }
                    if (!expiredBuffs.empty()) {
                        // 1. Send 0x402E (KeepUpMugongEnd_ACK) to remove buff icon
                        {
                            for (const auto& eb : expiredBuffs) {
                                std::vector<BYTE> endAck(4 + 11);
                                BYTE* ep = endAck.data() + 4;
                                ep[0] = 0; // bResult
                                *(DWORD*)(ep + 1) = eb.dwObjectID;
                                ep[5] = 1; // bObjectType (Player)
                                *(DWORD*)(ep + 6) = eb.dwMugongID;
                                ep[10] = eb.bLevel;
                                PACKET_HEADER* headE = (PACKET_HEADER*)endAck.data();
                                headE->id = 0x402E; // CS_BT_KEEPUPMUGONGEND_ACK
                                headE->payloadSize = 11;
                                EncryptPacket(endAck.data(), 0x42);
                                // Broadcast to all players on same map
                                SessionMgr::GetInstance().BroadcastToMap(mapID, endAck);
                            }
                        }
                        // 1.5 分身 buff 过期，清理所有分身
                        for (const auto& eb : expiredBuffs) {
                            if (eb.dwMugongID == 41) {
                                CleanupAllBunsins(eb.dwCharID, mapID);
                            }
                        }
                        // 2. Recalculate stats for affected players (outside all mutexes) (do NOT send 0x4414 to prevent clearing client visuals)
                        std::set<DWORD> refreshedChars;
                        for (const auto& eb : expiredBuffs) {
                            if (refreshedChars.count(eb.dwCharID) == 0) {
                                refreshedChars.insert(eb.dwCharID);
                                PlayerManager::GetInstance().RecalculateStats(eb.dwCharID, false);
                                LOG("[BuffExpiry] Refreshed stats for charID " + std::to_string(eb.dwCharID));
                            }
                        }
                    }
                }
            }
            idx++;
        }

        // === Drop Expiration Cleanup (every 5 seconds, worker 0 only) ===
        if (workerId == 0) {
            static DWORD s_lastDropCleanupTime = 0;
            if (tick - s_lastDropCleanupTime >= 5000) {
                s_lastDropCleanupTime = tick;
                DropManager::GetInstance()->CleanupExpiredDrops();
            }
        }

        // === Party Position Sync (every 3 seconds, worker 0 only) ===
        if (workerId == 0) {
            static DWORD s_lastPartySyncTime = 0;
            if (tick - s_lastPartySyncTime >= 3000) {
                s_lastPartySyncTime = tick;
                // Iterate all active parties and broadcast member positions
                // We do this by iterating all online players and checking if they're in a party
                std::vector<std::pair<DWORD, SOCKET>> onlinePlayers;
                {
                    auto allSockets = SessionMgr::GetInstance().GetAllSockets();
                    for (SOCKET s : allSockets) {
                        DWORD cid = SessionMgr::GetInstance().GetCharID(s);
                        if (cid > 0) onlinePlayers.push_back({cid, s});
                    }
                }
                // Track which parties we've already synced
                std::set<DWORD> syncedParties;
                for (auto& op : onlinePlayers) {
                    DWORD charID = op.first;
                    SOCKET sock = op.second;
                    DWORD partyID = PartyManager::GetInstance().GetPartyID(charID);
                    if (partyID == 0 || syncedParties.count(partyID)) continue;
                    syncedParties.insert(partyID);

                    auto members = PartyManager::GetInstance().GetMembers(partyID);
                    if (members.size() <= 1) continue;

                    // Gather fresh position data for each member
                    struct MemberPos {
                        DWORD dwCharID, dwObjID;
                        WORD wLevel;
                        DWORD dwHpCur, dwHpMax, dwMapID;
                        WORD wPosX, wPosY;
                    };
                    std::vector<MemberPos> posData;
                    for (auto& m : members) {
                        MemberPos mp = {};
                        mp.dwCharID = m.dwCharID;
                        mp.dwObjID = m.dwCharID + 400000000;
                        // Find in maps
                        for (auto& mapPair : g_MapInstances) {
                            if (!mapPair.second) continue;
                            std::lock_guard<std::mutex> mlock(mapPair.second->GetMutex());
                            sServerObject* pl = mapPair.second->GetPlayer(mp.dwObjID);
                            if (pl) {
                                mp.wLevel = pl->wLevel;
                                mp.dwHpCur = pl->dwHpCur;
                                mp.dwHpMax = pl->dwHpMax;
                                mp.dwMapID = pl->dwMapID;
                                mp.wPosX = pl->wPosX;
                                mp.wPosY = pl->wPosY;
                                break;
                            }
                        }
                        posData.push_back(mp);
                    }

                    // Send each member's position to all other members
                    for (auto& pos : posData) {
                        // Build PARTYPOSITION_ACK payload
                        std::vector<BYTE> payload;
                        auto push4 = [&](DWORD d) { payload.push_back(d&0xFF); payload.push_back((d>>8)&0xFF); payload.push_back((d>>16)&0xFF); payload.push_back((d>>24)&0xFF); };
                        auto push2 = [&](WORD w) { payload.push_back(w&0xFF); payload.push_back((w>>8)&0xFF); };
                        push4(partyID);
                        push4(pos.dwObjID);
                        push2(pos.wLevel);
                        push4(pos.dwHpCur);
                        push4(pos.dwHpMax);
                        push4(pos.dwMapID);
                        push2(pos.wPosX);
                        push2(pos.wPosY);

                        std::vector<BYTE> pkt(4);
                        pkt.insert(pkt.end(), payload.begin(), payload.end());
                        PACKET_HEADER* h = (PACKET_HEADER*)pkt.data();
                        h->id = PKT_PARTYPOSITION_ACK;
                        h->payloadSize = (WORD)payload.size();
                        EncryptPacket(pkt.data(), 0x42);

                        // Send to all party members
                        for (auto& m2 : members) {
                            if (m2.dwCharID == pos.dwCharID) continue; // skip self
                            SOCKET mSock = SessionMgr::GetInstance().GetSocketByCharID(m2.dwCharID);
                            if (mSock != INVALID_SOCKET) {
                                SafeSend(mSock, (const char*)pkt.data(), (int)pkt.size(), 0);
                            }
                        }
                    }
                }
            }
        }
    }
}

void MonsterAIThread() {
    int numWorkers = std::thread::hardware_concurrency();
    if (numWorkers <= 0) numWorkers = 4;
    if (numWorkers > 8) numWorkers = 8; // Cap at 8 to avoid excessive context switching

    LOG("[UnitSvr AI] Map Instance Engine Started with " + std::to_string(numWorkers) + " Worker Threads. 10 Tick / Sec.");
    
    std::vector<std::thread> workers;
    for (int i = 0; i < numWorkers; i++) {
        workers.push_back(std::thread(MonsterAIWorker, i, numWorkers));
    }
    
    for (auto& t : workers) {
        if (t.joinable()) t.join();
    }
}









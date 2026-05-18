#include "MonsterAI.h"
#include "Handlers/PartyHandler.h"
#include "Network/SessionMgr.h"
#include "Network/PacketRouter.h"
#include "GameObjects/MapInstance.h"
#include "GameObjects/PlayerManager.h"
#include "GameObjects/ExpSystem.h"
#include <cmath>
#include <thread>
#include <time.h>
#include <set>

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
                    std::vector<CMapInstance::PendingSyncMove> syncsToSend;
                    DWORD mapID = pair.first;
                    {
                        std::lock_guard<std::mutex> lock(pair.second->GetMutex());
                        pair.second->Update(tick);
                        syncsToSend = pair.second->m_pendingSyncs;
                        pair.second->m_pendingSyncs.clear();
                    }
                    
                    // Broadcast pending sync moves outside map mutex to prevent deadlock
                    if (!syncsToSend.empty()) {
                        for (const auto& sm : syncsToSend) {
                            // Build SYNCMOVE_ACK (0x430E)
                            std::vector<BYTE> buf; buf.resize(4);
                            buf.push_back(0); // bResult
                            buf.push_back(sm.dwObjectID & 0xFF); buf.push_back((sm.dwObjectID>>8)&0xFF); buf.push_back((sm.dwObjectID>>16)&0xFF); buf.push_back((sm.dwObjectID>>24)&0xFF);
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
                            
                            // Send to all other players on same map (exclude self)
                            DWORD selfCharID = sm.dwObjectID - 400000000;
                            SOCKET selfSock = SessionMgr::GetInstance().GetSocketByCharID(selfCharID);
                            SessionMgr::GetInstance().ForEachSocketInMap(mapID, [&](SOCKET s, DWORD sCharID) {
                                DWORD sObjID = sCharID + 400000000;
                                if (sObjID != sm.dwObjectID) {
                                    SafeSend(s, (const char*)buf.data(), buf.size(), 0);
                                }
                            });
                        }
                    }

                    // === Player HP/IP Auto-Recovery (every 5 seconds) ===
                    struct RegenInfo {
                        DWORD dwObjectID;
                        DWORD dwHpMax, dwHpCur;
                        WORD wIpMax, wIpCur;
                    };
                    std::vector<RegenInfo> regenPackets;
                    {
                        std::lock_guard<std::mutex> lock(pair.second->GetMutex());
                        auto& players = pair.second->GetPlayers();
                        for (auto& pp : players) {
                            sServerObject& pl = pp.second;
                            if (pl.bObjectType != 1) continue; // only players
                            if (pl.dwHpCur == 0) continue; // do not regen dead players
                            if (pl.wEquipRestoreHp == 0 && pl.wEquipRestoreIp == 0) continue; // no regen stats
                            if (tick - pl.dwLastRegenTime < 5000) continue; // not yet 5 seconds
                            
                            pl.dwLastRegenTime = tick;
                            bool changed = false;
                            
                            DWORD oldHp = pl.dwHpCur; WORD oldIp = pl.wIpCur;
                            if (pl.wEquipRestoreHp > 0 && pl.dwHpCur < pl.dwHpMax) {
                                pl.dwHpCur += pl.wEquipRestoreHp;
                                if (pl.dwHpCur > pl.dwHpMax) pl.dwHpCur = pl.dwHpMax;
                                changed = true;
                            }
                            if (pl.wEquipRestoreIp > 0 && pl.wIpCur < pl.wIpMax) {
                                pl.wIpCur += pl.wEquipRestoreIp;
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
                            auto push2 = [&](WORD w) { hpBuf.push_back(w&0xFF); hpBuf.push_back((w>>8)&0xFF); };
                            push4(ri.dwHpMax);
                            push4(ri.dwHpCur);
                            push2(ri.wIpMax);
                            push2(ri.wIpCur);
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
                    {
                        std::lock_guard<std::mutex> lock(pair.second->GetMutex());
                        auto& players = pair.second->GetPlayers();
                        for (auto& pp : players) {
                            sServerObject& pl = pp.second;
                            if (pl.bObjectType != 1) continue;
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
                                    ++it;
                                }
                            }
                        }
                    }
                    // Send buff end packets & refresh stats outside map mutex
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
                        // 2. Recalculate stats for affected players (outside all mutexes)
                        std::set<DWORD> refreshedChars;
                        for (const auto& eb : expiredBuffs) {
                            if (refreshedChars.count(eb.dwCharID) == 0) {
                                refreshedChars.insert(eb.dwCharID);
                                PlayerManager::GetInstance().RecalculateStats(eb.dwCharID, true);
                                LOG("[BuffExpiry] Refreshed stats for charID " + std::to_string(eb.dwCharID));
                            }
                        }
                    }
                }
            }
            idx++;
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









#include "MoveHandler.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../GameObjects/DropManager.h"

extern std::map<DWORD, CMapInstance*> g_MapInstances;

static void SendSystemWarningChat(SOCKET clientSocket, const std::string& msg) {
    std::vector<BYTE> buf;
    buf.resize(4, 0);
    DWORD senderObjID = 0;
    buf.push_back(senderObjID & 0xFF); buf.push_back((senderObjID >> 8) & 0xFF); buf.push_back((senderObjID >> 16) & 0xFF); buf.push_back(senderObjID >> 24);
    buf.push_back(8); // CT_TIMEMESSAGE
    WORD len = (WORD)msg.size();
    buf.push_back(len & 0xFF); buf.push_back((len >> 8) & 0xFF);
    buf.insert(buf.end(), msg.begin(), msg.end());
    WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
}

void OnMoveReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize, WORD headerId) {
    DWORD dwMoveID = *(DWORD*)(payload);
    DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    CMapInstance* mapInst = nullptr;
    if (g_MapInstances.count(pMapID)) mapInst = g_MapInstances[pMapID];
    if (!mapInst) return;

    WORD pX = 0, pY = 0;
    int lastUX = 0, lastUY = 0;
    bool needsUpdate = false;
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwMoveID);
        if (!pObj) {
            sServerObject p; p.dwObjectID = dwMoveID; p.dwMapID = pMapID; p.bObjectType = 1; p.dwHpCur = 60000; p.dwHpMax = 60000;
            p.wPosX = *(WORD*)(payload + 4); p.wPosY = *(WORD*)(payload + 6);
            mapInst->AddPlayer(p);
            pObj = mapInst->GetPlayer(dwMoveID);
        }
        
        if (pObj) {
            if (pObj->dwHpCur == 0) {
                LOG("[MoveHandler] Blocked movement for dead player charID=" + std::to_string(dwMoveID));
                return;
            }
            if (pObj->bShopStatus == 1) {
                SendSystemWarningChat(clientSocket, "[Shop] Setup is active. Player movement is blocked!");
                return;
            }
        }

        // 检查控制类 Debuff (定身/冰冻/眩晕) 拦截玩家移动
        bool isCC = false;
        if (pObj) {
            for (const auto& bf : pObj->activeBuffs) {
                if (bf.second.bIsDebuff) {
                    DWORD mugID = bf.second.dwMugongID;
                    if (mugID == 94 || mugID == 35 || mugID == 65) {
                        isCC = true;
                        break;
                    }
                }
            }
        }
        if (isCC) {
            SendSystemWarningChat(clientSocket, "[Control] You are frozen, stunned or immobilized and cannot move!");
            return;
        }

        if (pObj && pObj->activeBuffs.count(130) > 0) {
            pObj->activeBuffs[130].dwEndTime = 0; // Mark for instant expiry in MonsterAI loop
            LOG("[MoveHandler] Player " + std::to_string(dwMoveID) + " moved during Turtle Breath. Expiring buff 130.");
        }
        
        int oldX = pObj->wPosX;
        int oldY = pObj->wPosY;
        pObj->wPosX = *(WORD*)(payload + 4);
        pObj->wPosY = *(WORD*)(payload + 6);
        // Sync float accumulators for interpolation
        pObj->fPosX = (float)pObj->wPosX;
        pObj->fPosY = (float)pObj->wPosY;
        mapInst->UpdatePlayerGrid(dwMoveID, oldX, oldY, pObj->wPosX, pObj->wPosY);
        
        int dx = pObj->wPosX - pObj->wLastUpdateX;
        int dy = pObj->wPosY - pObj->wLastUpdateY;
        const int AOI_REFRESH_DIST_SQ = 2 * 2; // 业务意图：结合 1:4 地图缩放，物理移动约 8 米（2逻辑格）即触发九宫格视野刷新
        needsUpdate = (dx*dx + dy*dy >= AOI_REFRESH_DIST_SQ || pObj->wLastUpdateX == 0);
        
        // Save old update pos for AOI entry check
        lastUX = pObj->wLastUpdateX;
        lastUY = pObj->wLastUpdateY;
        
        if (needsUpdate) {
            pObj->wLastUpdateX = pObj->wPosX;
            pObj->wLastUpdateY = pObj->wPosY;
        }
        
        pX = pObj->wPosX;
        pY = pObj->wPosY;
        
        // Track movement state for AOI entry re-sync
        if ((headerId == 0x430B || headerId == 0x430D) && totalSize >= 18) { // STARTMOVE or SYNCMOVE
            pObj->bIsMoving = true;
            pObj->wMoveDesX = *(WORD*)(payload + 9);
            pObj->wMoveDesY = *(WORD*)(payload + 11);
            pObj->bMoveDesH = *(BYTE*)(payload + 13);
            pObj->wMoveDirection = *(WORD*)(payload + 14);
            
            BYTE moveState = *(BYTE*)(payload + 16);
            if (pObj->activeBuffs.count(34) > 0 || 
                pObj->activeBuffs.count(64) > 0 || 
                pObj->activeBuffs.count(94) > 0 || 
                pObj->activeBuffs.count(124) > 0) 
            {
                // Force Qinggong state (4) so the client plays the lightness footsteps visual effect
                moveState = 4;
            }
            pObj->bMoveState = moveState;
            *(BYTE*)(payload + 16) = moveState;
        } else if (headerId == 0x430F) { // ENDMOVE
            pObj->bIsMoving = false;
        }
    }
    
    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); // bResult

    // 业务设计意图：提取对象的类型(1=Player, 3=Monster, 4=Pet, 5=FuncNPC)以对齐ACK包的结构。
    BYTE bObjectType = 1;
    if (mapInst) {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwMoveID);
        if (pObj) {
            bObjectType = pObj->bObjectType;
        }
    }

    // 业务设计意图：定义协议对齐组装器，用于将 bObjectType 插入到 dwObjectID 之后。
    auto insertPayloadWithObjectType = [&](std::vector<BYTE>& dest, BYTE* src, WORD size) {
        if (size >= 4) {
            dest.insert(dest.end(), src, src + 4);
            dest.push_back(bObjectType);
            dest.insert(dest.end(), src + 4, src + size);
        } else {
            dest.insert(dest.end(), src, src + size);
        }
    };
    
    if (headerId == 0x430B) { // STARTMOVE
        if (totalSize >= 18) {
            BYTE bSpeed = 11;
            if (mapInst) {
                std::lock_guard<std::mutex> lock(mapInst->GetMutex());
                sServerObject* pObj = mapInst->GetPlayer(dwMoveID);
                if (pObj && (pObj->wWalkSpeed & 0xFF) > 0) {
                    bSpeed = pObj->wWalkSpeed & 0xFF;
                }
            }
            if (bSpeed == 0) bSpeed = 11;
            payload[17] = bSpeed; // Force correct speed, client often sends 0
        }
        ackBuf.insert(ackBuf.end(), payload, payload + totalSize);
        ackBuf.push_back(0); // bFastMove
    } else if (headerId == 0x430D) { // SYNCMOVE
        ackBuf.insert(ackBuf.end(), payload, payload + totalSize);
        
        DWORD currentTick = GetTickCount();
        WORD wDiffTime = 100; // default to 100ms
        {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwMoveID);
            if (pObj) {
                if (pObj->dwLastMoveTime > 0 && currentTick > pObj->dwLastMoveTime) {
                    wDiffTime = (WORD)(currentTick - pObj->dwLastMoveTime);
                }
                pObj->dwLastMoveTime = currentTick;
            }
        }
        
        ackBuf.push_back(wDiffTime & 0xFF); // wDiffTime LSB
        ackBuf.push_back((wDiffTime >> 8) & 0xFF); // wDiffTime MSB
    } else if (headerId == 0x430F) { // ENDMOVE
        // REQ: dwObjectID (4) + wPosX (2) + wPosY (2) + bHeight (1) + bState (1) + bSpeed (1) -> 11 bytes
        // ACK: dwObjectID (4) + wPosX (2) + wPosY (2) + bHeight (1) + bState (1) -> 10 bytes
        int copySize = (totalSize > 10) ? 10 : totalSize;
        ackBuf.insert(ackBuf.end(), payload, payload + copySize);
    } else {
        ackBuf.insert(ackBuf.end(), payload, payload + totalSize);
    }
    
    PACKET_HEADER* mh = (PACKET_HEADER*)ackBuf.data(); 
    mh->id = headerId + 1; 
    mh->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);

    std::string dump = "[MoveHandler] Move ACK Dump (" + std::to_string(ackBuf.size()) + " bytes, ID: 0x" + std::to_string(mh->id) + "): ";
    for (size_t i = 0; i < ackBuf.size(); ++i) {
        char hex[10];
        sprintf(hex, "%02X ", ackBuf[i]);
        dump += hex;
    }
    LOG(dump);

    EncryptPacket(ackBuf.data(), 0x42); 
    
    // 1. Send explicitly to self first to ensure immediate client confirmation and prevent disconnects
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    
    // 2. Broadcast to other players in the AOI (excluding self)
    mapInst->BroadcastPacketAOI(pX, pY, ackBuf, clientSocket);

    // Check if we need to send an AoI update
    if (needsUpdate) {
        // Sync any active drops that have newly entered the player's AOI
        DropManager::GetInstance()->SendActiveDropsInAOI(clientSocket, pMapID, pX, pY, lastUX, lastUY);

        std::vector<BYTE> objListBuf;
        objListBuf.resize(4);
        objListBuf.push_back(0); // bResult
        
        WORD wNumObject = 0;
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        
        // 1. 处理离开我的视野 (在旧视野 lastUX, lastUY，但不在新视野 pX, pY) 的怪物、功能 NPC 和其他玩家
        if (lastUX > 0 && lastUY > 0) {
            auto oldMonsters = mapInst->GetMonstersInAOI(lastUX, lastUY);
            for (auto* o : oldMonsters) {
                if (o->dwObjectID != dwMoveID) {
                    int ox = o->wPosX - pX;
                    int oy = o->wPosY - pY;
                    if (ox * ox + oy * oy > 150 * 150) {
                        std::vector<BYTE> leaveBuf; leaveBuf.resize(4);
                        leaveBuf.push_back(0); // bResult
                        DWORD oid = o->dwObjectID;
                        leaveBuf.push_back(oid & 0xFF); leaveBuf.push_back((oid >> 8) & 0xFF); leaveBuf.push_back((oid >> 16) & 0xFF); leaveBuf.push_back(oid >> 24);
                        leaveBuf.push_back(o->bObjectType); // 3=Monster, 5=FuncNPC
                        leaveBuf.push_back(pMapID & 0xFF); leaveBuf.push_back((pMapID >> 8) & 0xFF); leaveBuf.push_back((pMapID >> 16) & 0xFF); leaveBuf.push_back(pMapID >> 24);
                        leaveBuf.push_back(0); // bType
                        
                        PACKET_HEADER* leaveHead = (PACKET_HEADER*)leaveBuf.data();
                        leaveHead->id = 0x3506; leaveHead->payloadSize = leaveBuf.size() - sizeof(PACKET_HEADER);
                        EncryptPacket(leaveBuf.data(), 0x42);
                        SafeSend(clientSocket, (const char*)leaveBuf.data(), leaveBuf.size(), 0);
                    }
                }
            }
            
            auto oldPlayers = mapInst->GetPlayersInAOI(lastUX, lastUY);
            for (auto* o : oldPlayers) {
                if (o->dwObjectID != dwMoveID && o->dwObjectID < 850000000) {
                    int ox = o->wPosX - pX;
                    int oy = o->wPosY - pY;
                    if (ox * ox + oy * oy > 150 * 150) {
                        // 我看不见 o 了，向我发送删除 o 的消息
                        std::vector<BYTE> leaveBuf; leaveBuf.resize(4);
                        leaveBuf.push_back(0);
                        DWORD oid = o->dwObjectID;
                        leaveBuf.push_back(oid & 0xFF); leaveBuf.push_back((oid >> 8) & 0xFF); leaveBuf.push_back((oid >> 16) & 0xFF); leaveBuf.push_back(oid >> 24);
                        leaveBuf.push_back(o->bObjectType); // 1=Player
                        leaveBuf.push_back(pMapID & 0xFF); leaveBuf.push_back((pMapID >> 8) & 0xFF); leaveBuf.push_back((pMapID >> 16) & 0xFF); leaveBuf.push_back(pMapID >> 24);
                        leaveBuf.push_back(0);
                        
                        PACKET_HEADER* leaveHead = (PACKET_HEADER*)leaveBuf.data();
                        leaveHead->id = 0x3506; leaveHead->payloadSize = leaveBuf.size() - sizeof(PACKET_HEADER);
                        EncryptPacket(leaveBuf.data(), 0x42);
                        SafeSend(clientSocket, (const char*)leaveBuf.data(), leaveBuf.size(), 0);
                        
                        // o 也看不见我了，向 o 发送删除我的消息
                        DWORD otherCharID = o->dwObjectID % 400000000; // 业务意图：使用模运算精准还原出无偏移的纯净 CharID，消除 800M 格式下的寻址偏差
                        SOCKET otherSock = SessionMgr::GetInstance().GetSocketByCharID(otherCharID);
                        if (otherSock != INVALID_SOCKET) {
                            std::vector<BYTE> otherLeave; otherLeave.resize(4);
                            otherLeave.push_back(0);
                            DWORD myid = dwMoveID;
                            otherLeave.push_back(myid & 0xFF); otherLeave.push_back((myid >> 8) & 0xFF); otherLeave.push_back((myid >> 16) & 0xFF); otherLeave.push_back(myid >> 24);
                            otherLeave.push_back(1); // Player
                            otherLeave.push_back(pMapID & 0xFF); otherLeave.push_back((pMapID >> 8) & 0xFF); otherLeave.push_back((pMapID >> 16) & 0xFF); otherLeave.push_back(pMapID >> 24);
                            otherLeave.push_back(0);
                            
                            PACKET_HEADER* otherHead = (PACKET_HEADER*)otherLeave.data();
                            otherHead->id = 0x3506; otherHead->payloadSize = otherLeave.size() - sizeof(PACKET_HEADER);
                            EncryptPacket(otherLeave.data(), 0x42);
                            SafeSend(otherSock, (const char*)otherLeave.data(), otherLeave.size(), 0);
                        }
                    }
                }
            }
        }

        auto monsters = mapInst->GetMonstersInAOI(pX, pY);
        for (auto* o : monsters) {
            if (o->dwObjectID != dwMoveID) {
                int ox = o->wPosX - pX;
                int oy = o->wPosY - pY;
                int old_ox = o->wPosX - lastUX;
                int old_oy = o->wPosY - lastUY;
                bool inNew = (ox*ox + oy*oy <= 150*150);
                bool inOld = (old_ox*old_ox + old_oy*old_oy <= 150*150);
                if (inNew && (!inOld || lastUX == 0)) wNumObject++;
            }
        }
        auto players = mapInst->GetPlayersInAOI(pX, pY);
        for (auto* o : players) {
            if (o->dwObjectID >= 850000000) continue;
            if (o->dwObjectID != dwMoveID) {
                int ox = o->wPosX - pX;
                int oy = o->wPosY - pY;
                int old_ox = o->wPosX - lastUX;
                int old_oy = o->wPosY - lastUY;
                bool inNew = (ox*ox + oy*oy <= 150*150);
                bool inOld = (old_ox*old_ox + old_oy*old_oy <= 150*150);
                if (inNew && (!inOld || lastUX == 0)) wNumObject++;
            }
        }
        
        if (wNumObject > 0) {
            objListBuf.push_back(wNumObject & 0xFF);
            objListBuf.push_back((wNumObject >> 8) & 0xFF);
            
            auto pushDWord = [&](DWORD d) { objListBuf.push_back(d & 0xFF); objListBuf.push_back((d>>8)&0xFF); objListBuf.push_back((d>>16)&0xFF); objListBuf.push_back((d>>24)&0xFF); };
            auto pushWord = [&](WORD w) { objListBuf.push_back(w & 0xFF); objListBuf.push_back((w>>8)&0xFF); };
            auto pushByte = [&](BYTE b) { objListBuf.push_back(b); };
            
            for (auto* o : monsters) {
                if (o->dwObjectID != dwMoveID) {
                    int ox = o->wPosX - pX;
                    int oy = o->wPosY - pY;
                    int old_ox = o->wPosX - lastUX;
                    int old_oy = o->wPosY - lastUY;
                    bool inNew = (ox*ox + oy*oy <= 150*150);
                    bool inOld = (old_ox*old_ox + old_oy*old_oy <= 150*150);
                    if (inNew && (!inOld || lastUX == 0)) {
                        pushDWord(o->dwObjectID); pushByte(o->bObjectType); pushWord(o->wPosX); pushWord(o->wPosY); pushByte(o->bHeight);
                    }
                }
            }
            for (auto* o : players) {
                if (o->dwObjectID >= 850000000) continue;
                if (o->dwObjectID != dwMoveID) {
                    int ox = o->wPosX - pX;
                    int oy = o->wPosY - pY;
                    int old_ox = o->wPosX - lastUX;
                    int old_oy = o->wPosY - lastUY;
                    bool inNew = (ox*ox + oy*oy <= 150*150);
                    bool inOld = (old_ox*old_ox + old_oy*old_oy <= 150*150);
                    if (inNew && (!inOld || lastUX == 0)) {
                        pushDWord(o->dwObjectID); pushByte(o->bObjectType); pushWord(o->wPosX); pushWord(o->wPosY); pushByte(o->bHeight);
                        
                        // 业务意图：如果我新看到的玩家 o 正在移动，向我自己补发 o 正在移动的 STARTMOVE_ACK，使我客户端立刻播放 o 的走路动画，彻底消除“静止滑行”与“瞬移”Bug
                        if (o->bIsMoving) {
                            std::vector<BYTE> moveBuf; moveBuf.resize(4);
                            moveBuf.push_back(0); // bResult
                            // dwObjectID
                            moveBuf.push_back(o->dwObjectID & 0xFF); moveBuf.push_back((o->dwObjectID>>8)&0xFF); moveBuf.push_back((o->dwObjectID>>16)&0xFF); moveBuf.push_back((o->dwObjectID>>24)&0xFF);
                            // 意图：NV_STARTMOVE_ACK 协议不包含 bObjectType，直接追加坐标，消除 1 字节解包位移
                            // wPosX, wPosY
                            moveBuf.push_back(o->wPosX & 0xFF); moveBuf.push_back(o->wPosX >> 8);
                            moveBuf.push_back(o->wPosY & 0xFF); moveBuf.push_back(o->wPosY >> 8);
                            moveBuf.push_back(o->bHeight); // bHeight
                            // wDesPosX, wDesPosY
                            moveBuf.push_back(o->wMoveDesX & 0xFF); moveBuf.push_back(o->wMoveDesX >> 8);
                            moveBuf.push_back(o->wMoveDesY & 0xFF); moveBuf.push_back(o->wMoveDesY >> 8);
                            moveBuf.push_back(o->bMoveDesH); // bDesHeight
                            // wDirection
                            moveBuf.push_back(o->wMoveDirection & 0xFF); moveBuf.push_back(o->wMoveDirection >> 8);
                            moveBuf.push_back(o->bMoveState); // bStatus
                            BYTE speed = (o->wWalkSpeed & 0xFF);
                            if (speed == 0) speed = 11;
                            moveBuf.push_back(speed); // bSpeed
                            moveBuf.push_back(0); // bFastMove
                            
                            PACKET_HEADER* moveHead = (PACKET_HEADER*)moveBuf.data();
                            moveHead->id = 0x430C; // STARTMOVE_ACK
                            moveHead->payloadSize = moveBuf.size() - sizeof(PACKET_HEADER);
                            EncryptPacket(moveBuf.data(), 0x42);
                            SafeSend(clientSocket, (const char*)moveBuf.data(), moveBuf.size(), 0);
                            LOG("[MoveHandler] Re-sent STARTMOVE_ACK to self for newly seen moving player " + std::to_string(o->dwObjectID));
                        }

                        // Tell the OTHER player about ME, because I just entered their AOI!
                        DWORD otherCharID = o->dwObjectID % 400000000; // 业务意图：使用模运算精准还原出无偏移的纯净 CharID，消除 800M 格式下的寻址偏差
                        SOCKET otherSock = SessionMgr::GetInstance().GetSocketByCharID(otherCharID);
                        if (otherSock != INVALID_SOCKET && otherSock != clientSocket) {
                            std::vector<BYTE> otherBuf;
                            otherBuf.resize(4);
                            otherBuf.push_back(0); // bResult
                            otherBuf.push_back(1); otherBuf.push_back(0); // wNumObject = 1
                            otherBuf.push_back(dwMoveID & 0xFF); otherBuf.push_back((dwMoveID>>8)&0xFF); otherBuf.push_back((dwMoveID>>16)&0xFF); otherBuf.push_back((dwMoveID>>24)&0xFF);
                            otherBuf.push_back(1); // bObjectType = 1 (Player)
                            otherBuf.push_back(pX & 0xFF); otherBuf.push_back(pX >> 8);
                            otherBuf.push_back(pY & 0xFF); otherBuf.push_back(pY >> 8);
                            BYTE myHeight = *(BYTE*)(payload + 8);
                            otherBuf.push_back(myHeight); // my height
                            
                            PACKET_HEADER* otherHead = (PACKET_HEADER*)otherBuf.data();
                            otherHead->id = 0x4312;
                            otherHead->payloadSize = otherBuf.size() - sizeof(PACKET_HEADER);
                            
                            std::string dump2 = "[MoveHandler] otherBuf MAPOBJECTLIST_ACK Dump (" + std::to_string(otherBuf.size()) + " bytes): ";
                            for (size_t i = 0; i < otherBuf.size(); ++i) {
                                char hex[10];
                                sprintf(hex, "%02X ", otherBuf[i]);
                                dump2 += hex;
                            }
                            LOG(dump2);

                            EncryptPacket(otherBuf.data(), 0x42);
                            SafeSend(otherSock, (const char*)otherBuf.data(), otherBuf.size(), 0);
                            
                            // Re-send current movement state so the other client starts walking animation immediately
                            {
                                sServerObject* mover = mapInst->GetPlayer(dwMoveID);
                                if (mover && mover->bIsMoving) {
                                    // Build a synthetic STARTMOVE_ACK (0x430C)
                                    // ACK format: bResult + dwObjectID + wPosX + wPosY + bHeight + wDesPosX + wDesPosY + bDesHeight + wDirection + bStatus + bSpeed + bFastMove
                                    std::vector<BYTE> moveBuf; moveBuf.resize(4);
                                    moveBuf.push_back(0); // bResult
                                    // dwObjectID
                                    moveBuf.push_back(dwMoveID & 0xFF); moveBuf.push_back((dwMoveID>>8)&0xFF); moveBuf.push_back((dwMoveID>>16)&0xFF); moveBuf.push_back((dwMoveID>>24)&0xFF);
                                    // 意图：NV_STARTMOVE_ACK 协议不包含 bObjectType，直接追加坐标，消除其他玩家解包此包时的 1 字节位移
                                    // wPosX, wPosY
                                    moveBuf.push_back(mover->wPosX & 0xFF); moveBuf.push_back(mover->wPosX >> 8);
                                    moveBuf.push_back(mover->wPosY & 0xFF); moveBuf.push_back(mover->wPosY >> 8);
                                    moveBuf.push_back(mover->bHeight); // bHeight
                                    // wDesPosX, wDesPosY
                                    moveBuf.push_back(mover->wMoveDesX & 0xFF); moveBuf.push_back(mover->wMoveDesX >> 8);
                                    moveBuf.push_back(mover->wMoveDesY & 0xFF); moveBuf.push_back(mover->wMoveDesY >> 8);
                                    moveBuf.push_back(mover->bMoveDesH); // bDesHeight
                                    // wDirection
                                    moveBuf.push_back(mover->wMoveDirection & 0xFF); moveBuf.push_back(mover->wMoveDirection >> 8);
                                    moveBuf.push_back(mover->bMoveState); // bStatus
                                    BYTE speed = (mover->wWalkSpeed & 0xFF);
                                    if (speed == 0) speed = 11;
                                    moveBuf.push_back(speed); // bSpeed
                                    moveBuf.push_back(0); // bFastMove
                                    
                                    PACKET_HEADER* moveHead = (PACKET_HEADER*)moveBuf.data();
                                    moveHead->id = 0x430C; // STARTMOVE_ACK
                                    moveHead->payloadSize = moveBuf.size() - sizeof(PACKET_HEADER);
                                    EncryptPacket(moveBuf.data(), 0x42);
                                    SafeSend(otherSock, (const char*)moveBuf.data(), moveBuf.size(), 0);
                                    LOG("[MoveHandler] Re-sent STARTMOVE_ACK to other player for moving player " + std::to_string(dwMoveID));
                                }
                            }
                        }
                    }
                }
            }
            
            PACKET_HEADER* olh = (PACKET_HEADER*)objListBuf.data();
            olh->id = 0x4312; // CS_NV_MAPOBJECTLIST_ACK
            olh->payloadSize = objListBuf.size() - sizeof(PACKET_HEADER);
            
            std::string dump3 = "[MoveHandler] objListBuf MAPOBJECTLIST_ACK Dump (" + std::to_string(objListBuf.size()) + " bytes): ";
            for (size_t i = 0; i < objListBuf.size(); ++i) {
                char hex[10];
                sprintf(hex, "%02X ", objListBuf[i]);
                dump3 += hex;
            }
            LOG(dump3);

            EncryptPacket(objListBuf.data(), 0x42);
            SafeSend(clientSocket, (const char*)objListBuf.data(), objListBuf.size(), 0);
            
            LOG("[MoveHandler] Player moved. Sent MAPOBJECTLIST_ACK with " + std::to_string(wNumObject) + " newly entered objects.");
        }
    }
}

// =========================================================================
// 战宠与灵宠移动及状态同步协议族 (CS_NC family)
// =========================================================================

void OnPetMoveReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize, WORD headerId) {
    if (!payload || totalSize < 4) return;
    DWORD dwPetID = *(DWORD*)payload;

    DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    CMapInstance* mapInst = nullptr;
    if (g_MapInstances.count(pMapID)) mapInst = g_MapInstances[pMapID];
    if (!mapInst) return;

    std::vector<BYTE> ackBuf;
    WORD ackId = 0;
    WORD pX = 0, pY = 0;

    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwPetID);
        if (!pObj) {
            LOG("[PetMove] Pet object not found in map: dwPetID=" + std::to_string(dwPetID));
            return;
        }

        int oldX = pObj->wPosX;
        int oldY = pObj->wPosY;

        if (headerId == 0x3507 || headerId == 0x3509) { // STARTMOVE (0x3507) or SYNCMOVE (0x3509)
            if (totalSize < 18) return;
            WORD wPosX = *(WORD*)(payload + 4);
            WORD wPosY = *(WORD*)(payload + 6);
            BYTE bHeight = *(BYTE*)(payload + 8);
            WORD wDesPosX = *(WORD*)(payload + 9);
            WORD wDesPosY = *(WORD*)(payload + 11);
            BYTE bDesHeight = *(BYTE*)(payload + 13);
            WORD wDirection = *(WORD*)(payload + 14);
            BYTE bState = *(BYTE*)(payload + 16);
            BYTE bSpeed = *(BYTE*)(payload + 17);

            pObj->wPosX = wPosX;
            pObj->wPosY = wPosY;
            pObj->fPosX = (float)wPosX;
            pObj->fPosY = (float)wPosY;
            pObj->bHeight = bHeight;
            pObj->wMoveDesX = wDesPosX;
            pObj->wMoveDesY = wDesPosY;
            pObj->bMoveDesH = bDesHeight;
            pObj->wMoveDirection = wDirection;
            pObj->bMoveState = bState;
            pObj->bIsMoving = true;
            pX = wPosX;
            pY = wPosY;

            mapInst->UpdatePlayerGrid(dwPetID, oldX, oldY, wPosX, wPosY);

            ackBuf.resize(4, 0);
            ackBuf.push_back(0); // bResult
            // dwObjectID
            ackBuf.push_back(dwPetID & 0xFF); ackBuf.push_back((dwPetID >> 8) & 0xFF);
            ackBuf.push_back((dwPetID >> 16) & 0xFF); ackBuf.push_back((dwPetID >> 24) & 0xFF);
            ackBuf.push_back(pObj->bObjectType); // 4 = OBJTYPE_PET
            // wPosX, wPosY, bHeight
            ackBuf.push_back(wPosX & 0xFF); ackBuf.push_back(wPosX >> 8);
            ackBuf.push_back(wPosY & 0xFF); ackBuf.push_back(wPosY >> 8);
            ackBuf.push_back(bHeight);
            // wDesPosX, wDesPosY, bDesHeight
            ackBuf.push_back(wDesPosX & 0xFF); ackBuf.push_back(wDesPosX >> 8);
            ackBuf.push_back(wDesPosY & 0xFF); ackBuf.push_back(wDesPosY >> 8);
            ackBuf.push_back(bDesHeight);
            // wDirection, bStatus, bSpeed
            ackBuf.push_back(wDirection & 0xFF); ackBuf.push_back(wDirection >> 8);
            ackBuf.push_back(bState);
            ackBuf.push_back(bSpeed);

            if (headerId == 0x3507) {
                ackId = 0x3508; // CS_NC_STARTMOVE_ACK
            } else {
                ackId = 0x350A; // CS_NC_SYNCMOVE_ACK
                ackBuf.push_back(0); ackBuf.push_back(0); // wDiffTime
            }

            LOG("[PetMove] " + std::string(headerId == 0x3507 ? "STARTMOVE" : "SYNCMOVE") + " for Pet " + std::to_string(dwPetID) + " from (" + std::to_string(wPosX) + "," + std::to_string(wPosY) + ") to (" + std::to_string(wDesPosX) + "," + std::to_string(wDesPosY) + ")");

        } else if (headerId == 0x350B) { // ENDMOVE (0x350B)
            if (totalSize < 12) return;
            WORD wPosX = *(WORD*)(payload + 4);
            WORD wPosY = *(WORD*)(payload + 6);
            BYTE bHeight = *(BYTE*)(payload + 8);
            WORD wDirection = *(WORD*)(payload + 9);
            BYTE bState = *(BYTE*)(payload + 11);

            pObj->wPosX = wPosX;
            pObj->wPosY = wPosY;
            pObj->fPosX = (float)wPosX;
            pObj->fPosY = (float)wPosY;
            pObj->bHeight = bHeight;
            pObj->wMoveDirection = wDirection;
            pObj->bMoveState = bState;
            pObj->bIsMoving = false;
            pX = wPosX;
            pY = wPosY;

            mapInst->UpdatePlayerGrid(dwPetID, oldX, oldY, wPosX, wPosY);

            ackBuf.resize(4, 0);
            ackBuf.push_back(0); // bResult
            ackBuf.push_back(dwPetID & 0xFF); ackBuf.push_back((dwPetID >> 8) & 0xFF);
            ackBuf.push_back((dwPetID >> 16) & 0xFF); ackBuf.push_back((dwPetID >> 24) & 0xFF);
            ackBuf.push_back(pObj->bObjectType); // 4 = OBJTYPE_PET
            ackBuf.push_back(wPosX & 0xFF); ackBuf.push_back(wPosX >> 8);
            ackBuf.push_back(wPosY & 0xFF); ackBuf.push_back(wPosY >> 8);
            ackBuf.push_back(bHeight);
            ackBuf.push_back(bState);

            ackId = 0x350C; // CS_NC_ENDMOVE_ACK
            LOG("[PetMove] ENDMOVE for Pet " + std::to_string(dwPetID) + " at (" + std::to_string(wPosX) + "," + std::to_string(wPosY) + ")");
        }
    }

    if (ackId != 0 && ackBuf.size() > 4) {
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = ackId;
        head->payloadSize = (WORD)(ackBuf.size() - 4);
        EncryptPacket(ackBuf.data(), 0x42);
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        mapInst->BroadcastPacketAOI_NoLock(pX, pY, ackBuf);
    }
}

void OnPetStatusChangeReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (!payload || totalSize < 8) return;
    BYTE bObjectType = payload[0];
    DWORD dwObjectID = *(DWORD*)(payload + 1);
    BYTE bStatus = payload[5];
    WORD wDirection = *(WORD*)(payload + 6);

    DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    CMapInstance* mapInst = nullptr;
    if (g_MapInstances.count(pMapID)) mapInst = g_MapInstances[pMapID];
    if (!mapInst) return;

    WORD pX = 0, pY = 0;
    std::vector<BYTE> ackBuf;

    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwObjectID);
        if (!pObj) return;

        pObj->bMoveState = bStatus;
        pObj->wMoveDirection = wDirection;
        pX = pObj->wPosX;
        pY = pObj->wPosY;

        ackBuf.resize(4, 0);
        ackBuf.push_back(bObjectType);
        ackBuf.push_back(dwObjectID & 0xFF); ackBuf.push_back((dwObjectID >> 8) & 0xFF);
        ackBuf.push_back((dwObjectID >> 16) & 0xFF); ackBuf.push_back((dwObjectID >> 24) & 0xFF);
        ackBuf.push_back(bStatus);
        ackBuf.push_back(wDirection & 0xFF); ackBuf.push_back(wDirection >> 8);
        ackBuf.push_back(0); // bSubType
    }

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x3510; // CS_NC_STATUSCHANGE_ACK
    head->payloadSize = (WORD)(ackBuf.size() - 4);
    EncryptPacket(ackBuf.data(), 0x42);

    std::lock_guard<std::mutex> lock(mapInst->GetMutex());
    mapInst->BroadcastPacketAOI_NoLock(pX, pY, ackBuf);
    LOG("[PetStatus] Status changed for Object " + std::to_string(dwObjectID) + " to status " + std::to_string(bStatus));
}

void OnPetMapEnterReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (!payload || totalSize < 8) return;
    DWORD dwObjectID = *(DWORD*)payload;
    DWORD dwMapID = *(DWORD*)(payload + 4);

    DWORD pMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    CMapInstance* mapInst = nullptr;
    if (g_MapInstances.count(pMapID)) mapInst = g_MapInstances[pMapID];
    if (!mapInst) return;

    std::lock_guard<std::mutex> lock(mapInst->GetMutex());
    sServerObject* pPet = mapInst->GetPlayer(dwObjectID);
    sServerObject* pOwner = mapInst->GetPlayer(charID + 400000000);
    if (pPet && pOwner) {
        int oldX = pPet->wPosX;
        int oldY = pPet->wPosY;
        pPet->wPosX = pOwner->wPosX;
        pPet->wPosY = pOwner->wPosY;
        pPet->fPosX = (float)pPet->wPosX;
        pPet->fPosY = (float)pPet->wPosY;
        pPet->bIsMoving = false;
        mapInst->UpdatePlayerGrid(dwObjectID, oldX, oldY, pPet->wPosX, pPet->wPosY);
        LOG("[PetMapEnter] Synced pet " + std::to_string(dwObjectID) + " position to master (" + std::to_string(pPet->wPosX) + "," + std::to_string(pPet->wPosY) + ")");
    }
}

void OnPetSackListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (!payload || totalSize < 9) return;
    DWORD dwOwnerID = *(DWORD*)payload;
    DWORD dwPetID = *(DWORD*)(payload + 4);
    BYTE bSackID = payload[8];

    std::vector<BYTE> ackBuf;
    ackBuf.resize(4, 0);
    ackBuf.push_back(dwPetID & 0xFF); ackBuf.push_back((dwPetID >> 8) & 0xFF);
    ackBuf.push_back((dwPetID >> 16) & 0xFF); ackBuf.push_back((dwPetID >> 24) & 0xFF);
    ackBuf.push_back(bSackID);
    ackBuf.push_back(0); // bNumItem = 0

    PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
    head->id = 0x355E; // CS_NC_PETSACKLIST_ACK
    head->payloadSize = (WORD)(ackBuf.size() - 4);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), (int)ackBuf.size(), 0);
    LOG("[PetSack] Sent PETSACKLIST_ACK for Pet " + std::to_string(dwPetID) + " Sack=" + std::to_string(bSackID));
}


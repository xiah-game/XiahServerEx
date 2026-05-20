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
        
        if (pObj && pObj->bShopStatus == 1) {
            SendSystemWarningChat(clientSocket, "[Shop] Setup is active. Player movement is blocked!");
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
        needsUpdate = (dx*dx + dy*dy >= 40*40 || pObj->wLastUpdateX == 0);
        
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
            pObj->bMoveState = *(BYTE*)(payload + 16);
            char dbg[256]; sprintf(dbg, "[MoveTrack] Player %u START/SYNC: pos(%d,%d) -> des(%d,%d) speed=%d bIsMoving=true",
                pObj->dwObjectID, pObj->wPosX, pObj->wPosY, pObj->wMoveDesX, pObj->wMoveDesY, pObj->wWalkSpeed);
            LOG(std::string(dbg));
        } else if (headerId == 0x430F) { // ENDMOVE
            pObj->bIsMoving = false;
            char dbg[256]; sprintf(dbg, "[MoveTrack] Player %u ENDMOVE: pos(%d,%d) bIsMoving=false", pObj->dwObjectID, pObj->wPosX, pObj->wPosY);
            LOG(std::string(dbg));
        }
    }
    
    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); // bResult
    
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
                if (o->dwObjectID != dwMoveID) {
                    int ox = o->wPosX - pX;
                    int oy = o->wPosY - pY;
                    int old_ox = o->wPosX - lastUX;
                    int old_oy = o->wPosY - lastUY;
                    bool inNew = (ox*ox + oy*oy <= 150*150);
                    bool inOld = (old_ox*old_ox + old_oy*old_oy <= 150*150);
                    if (inNew && (!inOld || lastUX == 0)) {
                        pushDWord(o->dwObjectID); pushByte(o->bObjectType); pushWord(o->wPosX); pushWord(o->wPosY); pushByte(o->bHeight);
                        
                        // Tell the OTHER player about ME, because I just entered their AOI!
                        DWORD otherCharID = (o->dwObjectID >= 400000000 && o->dwObjectID < 800000000) ? o->dwObjectID - 400000000 : (o->dwObjectID >= 800000000 ? o->dwObjectID - 400000000 : o->dwObjectID);
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

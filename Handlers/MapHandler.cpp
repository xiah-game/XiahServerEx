#include "MapHandler.h"
#include "../DB/CharacterDB.h"
#include "../DB/GameDataDB.h"
#include <unordered_set>
#include "../GameObjects/MugongManager.h"
#include "../GameObjects/PlayerManager.h"
#include "../GameObjects/PlayerManager.h"
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

void OnMapLoadingSequenceReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize, WORD headerId) {
    if (headerId == 0x3A55) { 
        std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); 
        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3A56; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    } else if (headerId == 0x3A34) { 
        std::vector<BYTE> ackBuf; ackBuf.resize(4); 
        ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); 
        ackBuf.push_back(0); ackBuf.push_back(0); 
        ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); 
        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3A35; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    } else if (headerId == 0x3203) { 
        std::vector<BYTE> ackBuf; ackBuf.resize(4); 
        ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); 
        ackBuf.push_back(0); 
        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3204; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    } else if (headerId == 0x3903) { 
        std::vector<BYTE> ackBuf; ackBuf.resize(4); 
        ackBuf.push_back(1); ackBuf.push_back(1); ackBuf.push_back(1); ackBuf.push_back(1); 
        ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); 
        ackBuf.push_back(0); ackBuf.push_back(0); 
        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x3904; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
        LOG("[MapHandler] Game loading sequence fulfilled! Client is rendering the 3D map!");
    }
}

void OnMapEnterReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    DWORD dwObjectID = *(DWORD*)(payload);
    LOG("[MapHandler] Received CS_NV_MAPENTER_REQ! Preparing to spawn character in 3D Space! ObjectID/CharID: " + std::to_string(dwObjectID));
    DWORD dwMapID = *(DWORD*)(payload + 4);
    DWORD dwActualCharID = dwObjectID - 400000000;
    
    SessionMgr::GetInstance().SetMapID(clientSocket, dwMapID);
    SessionMgr::GetInstance().SetCharID(clientSocket, dwActualCharID);
    
    WORD wCurX = 398, wCurY = 618; BYTE bCurH = 1;
    { int px = wCurX, py = wCurY, ph = bCurH;
      CharacterDB::GetInstance().GetCharPosition(dwActualCharID, px, py, ph);
      wCurX = px; wCurY = py; bCurH = ph; }

    // Create or get the existing object
    CMapInstance* mapInst = nullptr;
    if (g_MapInstances.count(dwMapID)) {
        mapInst = g_MapInstances[dwMapID];
    } else {
        LOG("[MapHandler] WARNING: MapID " + std::to_string(dwMapID) + " does not exist!");
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwObjectID);
        if (!pObj) {
            sServerObject newObj;
            newObj.dwObjectID = dwObjectID;
            newObj.wPosX = wCurX;
            newObj.wPosY = wCurY;
            mapInst->AddPlayer(newObj);
            pObj = mapInst->GetPlayer(dwObjectID);
        }
        pObj->dwObjectID = dwObjectID;
        pObj->dwMapID = dwMapID;
        pObj->wPosX = wCurX;
        pObj->wPosY = wCurY;
        pObj->wLastUpdateX = wCurX; // FIX: Prevent duplicate AoI spawn on first move
        pObj->wLastUpdateY = wCurY;
        pObj->bHeight = bCurH;
        pObj->bObjectType = 1; // player

        // Initialize HP/IP from DB so RecalculateStats has valid initial values
        DWORD dbHpCur = 0, dbHpMax = 0;
        WORD dbIpCur = 0, dbIpMax = 0;
        CharacterDB::CharPower cpMap;
        if (CharacterDB::GetInstance().GetCharData(dwActualCharID, cpMap)) {
            dbHpCur = cpMap.dwHpCur; dbHpMax = cpMap.dwHpMax;
            dbIpCur = cpMap.wIpCur; dbIpMax = cpMap.wIpMax;
        }
        pObj->dwHpMax = (dbHpMax > 0) ? dbHpMax : 60000;
        pObj->dwHpCur = (dbHpCur > 0) ? dbHpCur : pObj->dwHpMax;
        pObj->wIpMax = (dbIpMax > 0) ? dbIpMax : 100;
        pObj->wIpCur = (dbIpCur > 0) ? dbIpCur : pObj->wIpMax;
        LOG("[MapHandler] Initialized player HP/IP from DB: HpCur=" + std::to_string(pObj->dwHpCur) + "/" + std::to_string(pObj->dwHpMax) + " IpCur=" + std::to_string(pObj->wIpCur) + "/" + std::to_string(pObj->wIpMax));
    }
    
    // Fetch szNickName and bCharType from CHAR_VISUAL
    {
        std::string vizName;
        BYTE vizType = 0;
        if (CharacterDB::GetInstance().GetCharVisual(dwActualCharID, vizName, vizType)) {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwObjectID);
            if (pObj) {
                pObj->szName = vizName;
                pObj->bPropType = vizType;
            }
        }
    }

    // Fetch Player's learned Mugongs BEFORE RecalcStats (passive bonuses need this data)
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwObjectID);
        if (pObj) {
            MugongManager::GetInstance()->LoadPlayerMugongs(dwActualCharID, pObj->learnedMugongs);
        }
    }
    LOG("[MapHandler] Player Learned Mugongs Loaded.");

    // Use PlayerManager to recalculate all stats (including passive inner skill bonuses)
    PlayerManager::GetInstance().RecalculateStats(dwActualCharID, false);

    // Retrieve the WalkSpeed calculated by PlayerManager
    BYTE bWalkSpeed = 11;
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(dwObjectID);
        if (pObj) {
            bWalkSpeed = pObj->wWalkSpeed & 0xFF;
        }
    }

    std::vector<BYTE> ackBuf; ackBuf.resize(4);
    ackBuf.push_back(0); // bResult
    ackBuf.push_back(dwMapID & 0xFF); ackBuf.push_back((dwMapID >> 8) & 0xFF); ackBuf.push_back((dwMapID >> 16) & 0xFF); ackBuf.push_back(dwMapID >> 24);
    ackBuf.push_back(dwObjectID & 0xFF); ackBuf.push_back((dwObjectID >> 8) & 0xFF); ackBuf.push_back((dwObjectID >> 16) & 0xFF); ackBuf.push_back(dwObjectID >> 24);
    ackBuf.push_back(1); // bObjectType = 1 (PC)
    ackBuf.push_back(wCurX & 0xFF); ackBuf.push_back(wCurX >> 8);
    ackBuf.push_back(wCurY & 0xFF); ackBuf.push_back(wCurY >> 8);
    ackBuf.push_back(bCurH);
    ackBuf.push_back(0); ackBuf.push_back(0); // wDirection
    ackBuf.push_back(0); // bState
    ackBuf.push_back(bWalkSpeed); // bSpeed
    ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); // dwMunpaBattleID
    ackBuf.push_back(0); // bBattleType
    ackBuf.push_back(0); // bBattleTeam
    ackBuf.push_back(0); // bHpPortionCnt
    ackBuf.push_back(0); // bIpPortionCnt
    
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x4306; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[MapHandler] MAP ENTER ACK Sent! Player Spawned at " + std::to_string(wCurX) + ", " + std::to_string(wCurY) + " with Speed " + std::to_string(bWalkSpeed));

    // Send CS_IF_CHARINFO_ACK (0x3B02) immediately after MapEnterAck
    SendCharStatusInfoAck(clientSocket, dwActualCharID, 0x3B02);

    // Sync active drops in player's AOI upon entering
    DropManager::GetInstance()->SendActiveDropsInAOI(clientSocket, dwMapID, wCurX, wCurY);

    // Spawn Map Objects immediately upon entering within AoI
    std::vector<BYTE> aoiBuf; aoiBuf.resize(4); aoiBuf.push_back(0); // bResult
    auto pushDWord = [&](DWORD d) { aoiBuf.push_back(d & 0xFF); aoiBuf.push_back((d>>8)&0xFF); aoiBuf.push_back((d>>16)&0xFF); aoiBuf.push_back((d>>24)&0xFF); };
    auto pushWord = [&](WORD w) { aoiBuf.push_back(w & 0xFF); aoiBuf.push_back((w>>8)&0xFF); };
    auto pushByte = [&](BYTE b) { aoiBuf.push_back(b); };
    WORD numObjects = 0;
    
    {
        std::lock_guard<std::mutex> lockMonsters(mapInst->GetMutex());
        auto monsters = mapInst->GetMonstersInAOI(wCurX, wCurY);
        auto players = mapInst->GetPlayersInAOI(wCurX, wCurY);
        for (auto* obj : monsters) { 
            int ox = obj->wPosX - wCurX;
            int oy = obj->wPosY - wCurY;
            if (ox*ox + oy*oy <= 150*150) numObjects++;
        }
        for (auto* obj : players) {
            if (obj->dwObjectID != dwObjectID) {
                int ox = obj->wPosX - wCurX;
                int oy = obj->wPosY - wCurY;
                if (ox*ox + oy*oy <= 150*150) numObjects++;
            }
        }
        aoiBuf.push_back(numObjects & 0xFF); aoiBuf.push_back(numObjects >> 8);
        for (auto* obj : monsters) {
            int ox = obj->wPosX - wCurX;
            int oy = obj->wPosY - wCurY;
            if (ox*ox + oy*oy <= 150*150) {
                pushDWord(obj->dwObjectID);
                pushByte(obj->bObjectType); // 3=Monster, 5=NPC
                pushWord(obj->wPosX);
                pushWord(obj->wPosY);
                pushByte(obj->bHeight);

            }
        }
        for (auto* obj : players) {
            if (obj->dwObjectID != dwObjectID) {
                int ox = obj->wPosX - wCurX;
                int oy = obj->wPosY - wCurY;
                if (ox*ox + oy*oy <= 150*150) {
                    pushDWord(obj->dwObjectID);
                    pushByte(obj->bObjectType); // 1=Player
                    pushWord(obj->wPosX);
                    pushWord(obj->wPosY);
                    pushByte(obj->bHeight);
                }
            }
        }
    } // Unlock mapInst->GetMutex() before sending and broadcasting

    PACKET_HEADER* aoiHead = (PACKET_HEADER*)aoiBuf.data(); aoiHead->id = 0x4312; aoiHead->payloadSize = aoiBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(aoiBuf.data(), 0x42); SafeSend(clientSocket, (const char*)aoiBuf.data(), aoiBuf.size(), 0);
    LOG("[MapHandler] Broadcasted AoI Objects: " + std::to_string(numObjects) + " entities!");

    // Broadcast entering player to others in AOI so they can spawn this player!
    std::vector<BYTE> enterBuf; enterBuf.resize(4); enterBuf.push_back(0); // bResult
    WORD enterNum = 1;
    enterBuf.push_back(enterNum & 0xFF); enterBuf.push_back((enterNum >> 8) & 0xFF);
    auto pushDWordEnter = [&](DWORD d) { enterBuf.push_back(d & 0xFF); enterBuf.push_back((d>>8)&0xFF); enterBuf.push_back((d>>16)&0xFF); enterBuf.push_back((d>>24)&0xFF); };
    auto pushWordEnter = [&](WORD w) { enterBuf.push_back(w & 0xFF); enterBuf.push_back((w>>8)&0xFF); };
    pushDWordEnter(dwObjectID);
    enterBuf.push_back(1); // 1 = Player
    pushWordEnter(wCurX);
    pushWordEnter(wCurY);
    enterBuf.push_back(bCurH);
    PACKET_HEADER* enterHead = (PACKET_HEADER*)enterBuf.data(); enterHead->id = 0x4312; enterHead->payloadSize = enterBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(enterBuf.data(), 0x42);
    mapInst->BroadcastPacketAOI(wCurX, wCurY, enterBuf, clientSocket);
    

    
    // Removed UpdatePlayerStatsAndSend because sending it here delays CharInfo 
    // past MugongListAck, causing Init_WindowOutSide to wipe out the Mugong UI!
}

void OnMapInfoReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 4) return;
    DWORD reqMapID = *(DWORD*)payload;

    std::vector<BYTE> ackBuf;
    ackBuf.push_back(0); // bResult
    
    // dwMapID
    ackBuf.push_back(reqMapID & 0xFF);
    ackBuf.push_back((reqMapID >> 8) & 0xFF);
    ackBuf.push_back((reqMapID >> 16) & 0xFF);
    ackBuf.push_back((reqMapID >> 24) & 0xFF);

    std::string mapName = "Map";
    WORD wWidth = 2048, wHeight = 2048;
    BYTE bType = 1;

    GameDataDB::MapInfo mi;
    if (GameDataDB::GetInstance().GetMapInfo(reqMapID, mi)) {
        mapName = mi.szName; wWidth = mi.wWidth; wHeight = mi.wHeight; bType = mi.bType;
    }

    WORD nameLen = mapName.length();
    ackBuf.push_back(nameLen & 0xFF);
    ackBuf.push_back((nameLen >> 8) & 0xFF);
    for (char c : mapName) ackBuf.push_back(c);

    ackBuf.push_back(wWidth & 0xFF); ackBuf.push_back((wWidth >> 8) & 0xFF);
    ackBuf.push_back(wHeight & 0xFF); ackBuf.push_back((wHeight >> 8) & 0xFF);
    ackBuf.push_back(bType);

    std::vector<GameDataDB::LinkMapEntry> links;
    GameDataDB::GetInstance().GetLinkMapList(reqMapID, links);

    // bNumLinkMap
    BYTE bNumLinkMap = links.size();
    ackBuf.push_back(bNumLinkMap);

    auto pushDWord = [&](DWORD d) { ackBuf.push_back(d & 0xFF); ackBuf.push_back((d>>8)&0xFF); ackBuf.push_back((d>>16)&0xFF); ackBuf.push_back((d>>24)&0xFF); };
    auto pushWord = [&](WORD w) { ackBuf.push_back(w & 0xFF); ackBuf.push_back((w>>8)&0xFF); };
    auto pushByte = [&](BYTE b) { ackBuf.push_back(b); };

    for (auto& lm : links) {
        pushDWord(lm.dwLinkMapID);
        pushWord(lm.wPortalPosX);
        pushWord(lm.wPortalPosY);
        pushWord(lm.wPortalWidth);
        pushWord(lm.wPortalHeight);
        pushByte(lm.bLinkType);
    }

    std::vector<BYTE> finalBuf(4 + ackBuf.size());
    PACKET_HEADER* head = (PACKET_HEADER*)finalBuf.data();
    head->id = 0x4438; // CS_IT_MAPINFO_ACK
    head->payloadSize = ackBuf.size();
    memcpy(finalBuf.data() + 4, ackBuf.data(), ackBuf.size());

    EncryptPacket(finalBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)finalBuf.data(), finalBuf.size(), 0);

    LOG("[MapHandler] Sent MapInfoAck for MapID: " + std::to_string(reqMapID) + ", LinkMaps: " + std::to_string(links.size()));
}

void OnImReadyReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    LOG("[MapHandler] Received CS_IT_IMREADY_REQ (0x4435), sending ACK (0x4436)");
    std::vector<BYTE> ackBuf(4);
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    ackHead->id = 0x4436; // CS_IT_IMREADY_ACK
    ackHead->payloadSize = 0;
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    
    if (totalSize >= 8) {
        DWORD dwObjectID = *(DWORD*)(payload);
        DWORD dwMapID = *(DWORD*)(payload + 4);
        
        // Find player coordinates
        int pX = 0, pY = 0;
        BYTE pHeight = 0;
        CMapInstance* mapInst = nullptr;
        if (g_MapInstances.count(dwMapID)) mapInst = g_MapInstances[dwMapID];
        if (!mapInst) return;

        {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwObjectID);
            if (pObj) {
                pX = pObj->wPosX;
                pY = pObj->wPosY;
                pHeight = pObj->bHeight;
            }
        }
        
        std::vector<BYTE> objListBuf;
        objListBuf.resize(4);
        objListBuf.push_back(0); // bResult
        
        WORD wNumObject = 0;
        {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            LOG("[MapHandler] OnImReadyReq loop: Players size = " + std::to_string(mapInst->GetPlayers().size()) + ", My ObjID: " + std::to_string(dwObjectID) + ", MapID: " + std::to_string(dwMapID));
            auto players = mapInst->GetPlayersInAOI(pX, pY);
            for (auto* o : players) {
                LOG("[MapHandler] Player in AOI: ObjID: " + std::to_string(o->dwObjectID) + ", MapID: " + std::to_string(o->dwMapID) + ", Pos: " + std::to_string(o->wPosX) + "," + std::to_string(o->wPosY));
                if (o->dwObjectID != dwObjectID) {
                    int dx = o->wPosX - pX;
                    int dy = o->wPosY - pY;
                    LOG("[MapHandler] Distance: " + std::to_string(dx*dx + dy*dy));
                    if (dx*dx + dy*dy <= 150*150) wNumObject++;
                }
            }
        }
        
        objListBuf.push_back(wNumObject & 0xFF);
        objListBuf.push_back((wNumObject >> 8) & 0xFF);
        
        auto pushDWord = [&](DWORD d) { objListBuf.push_back(d & 0xFF); objListBuf.push_back((d>>8)&0xFF); objListBuf.push_back((d>>16)&0xFF); objListBuf.push_back((d>>24)&0xFF); };
        auto pushWord = [&](WORD w) { objListBuf.push_back(w & 0xFF); objListBuf.push_back((w>>8)&0xFF); };
        auto pushByte = [&](BYTE b) { objListBuf.push_back(b); };
        {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            auto players = mapInst->GetPlayersInAOI(pX, pY);
            for (auto* o : players) {
                if (o->dwObjectID != dwObjectID) {
                    int dx = o->wPosX - pX;
                    int dy = o->wPosY - pY;
                    if (dx*dx + dy*dy <= 150*150) {
                        pushDWord(o->dwObjectID);
                        pushByte(o->bObjectType); // 1=PC
                        pushWord(o->wPosX);
                        pushWord(o->wPosY);
                        pushByte(o->bHeight);
                    }
                }
            }
        }
        
        PACKET_HEADER* olh = (PACKET_HEADER*)objListBuf.data();
        olh->id = 0x4312; // CS_NV_MAPOBJECTLIST_ACK
        olh->payloadSize = objListBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(objListBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)objListBuf.data(), objListBuf.size(), 0);
        
        LOG("[MapHandler] Sent MAPOBJECTLIST_ACK (0x4312) with " + std::to_string(wNumObject) + " objects.");

        // Sync active drops in player's AOI
        DropManager::GetInstance()->SendActiveDropsInAOI(clientSocket, dwMapID, pX, pY);
        
        // Broadcast the new player to everyone else in the map
        std::vector<BYTE> bcastBuf(4);
        bcastBuf.push_back(0); // bResult
        bcastBuf.push_back(1); bcastBuf.push_back(0); // wNumObject = 1
        bcastBuf.push_back(dwObjectID & 0xFF); bcastBuf.push_back((dwObjectID>>8)&0xFF); bcastBuf.push_back((dwObjectID>>16)&0xFF); bcastBuf.push_back(dwObjectID>>24);
        bcastBuf.push_back(1); // bObjectType = 1 (PC)
        bcastBuf.push_back(pX & 0xFF); bcastBuf.push_back(pX >> 8);
        bcastBuf.push_back(pY & 0xFF); bcastBuf.push_back(pY >> 8);
        bcastBuf.push_back(pHeight);
        
        PACKET_HEADER* bcastHead = (PACKET_HEADER*)bcastBuf.data();
        bcastHead->id = 0x4312;
        bcastHead->payloadSize = bcastBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(bcastBuf.data(), 0x42);
        
        {
            SessionMgr::GetInstance().ForEachSocketInMap(dwMapID, [&](SOCKET s, DWORD sCharID) {
                if (s != clientSocket) {
                    DWORD sObjID = sCharID + 400000000;
                    
                    bool inRange = false;
                    {
                        std::lock_guard<std::mutex> lockA(mapInst->GetMutex());
                        sServerObject* sObj = mapInst->GetPlayer(sObjID);
                        if (sObj) {
                            int sX = sObj->wPosX;
                            int sY = sObj->wPosY;
                            int ddx = sX - pX;
                            int ddy = sY - pY;
                            if (ddx*ddx + ddy*ddy <= 150*150) inRange = true;
                        }
                    }
                    
                    if (inRange) {
                        SafeSend(s, (const char*)bcastBuf.data(), bcastBuf.size(), 0);
                    }
                }
            });
        }
    }
}

void OnCharStatusInfoReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    DWORD dwActualCharID = SessionMgr::GetInstance().GetCharID(clientSocket);
    LOG("[MapHandler] Received CS_IT_CHARSTATUSINFO_REQ (0x442B) for charID: " + std::to_string(dwActualCharID));
    if (dwActualCharID != 0) {
        UpdatePlayerStatsAndSend(clientSocket, dwActualCharID);
    }
}

// Handler for CS_IT_CHARINFO_REQ (0x440F) �?single character info request
// Client sends this via ValidateObject when it sees a movement packet for an unknown player.
// We respond with CHARINFOLIST_ACK (0x4412) format containing 1 player, since that format is proven to work.
void OnCharInfoReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 8) return;
    DWORD reqObjectID = *(DWORD*)(payload);
    DWORD reqMapID = *(DWORD*)(payload + 4);
    
    LOG("[MapHandler] OnCharInfoReq: client requesting info for ObjectID=" + std::to_string(reqObjectID) + " map=" + std::to_string(reqMapID));
    
    if (!g_MapInstances.count(reqMapID)) return;
    CMapInstance* mapInst = g_MapInstances[reqMapID];
    
    sServerObject objCopy;
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        sServerObject* pObj = mapInst->GetPlayer(reqObjectID);
        if (pObj) { objCopy = *pObj; found = true; }
    }
    if (!found) {
        LOG("[MapHandler] OnCharInfoReq: ObjectID=" + std::to_string(reqObjectID) + " NOT FOUND on map " + std::to_string(reqMapID));
        return;
    }
    
    // Use CHARINFOLIST_ACK (0x4412) format with count=1 �?this format is proven to work
    std::vector<BYTE> ackBuf; ackBuf.reserve(256);
    ackBuf.push_back(0); // bResult = success
    
    auto pushDWord = [&](DWORD d) { ackBuf.push_back(d & 0xFF); ackBuf.push_back((d>>8)&0xFF); ackBuf.push_back((d>>16)&0xFF); ackBuf.push_back((d>>24)&0xFF); };
    auto pushWord = [&](WORD w) { ackBuf.push_back(w & 0xFF); ackBuf.push_back((w>>8)&0xFF); };
    auto pushByte = [&](BYTE b) { ackBuf.push_back(b); };
    auto pushString = [&](const std::string& str) {
        WORD len = str.length();
        pushWord(len);
        for (char c : str) pushByte(c);
    };
    
    pushDWord(reqMapID);  // dwMapID (outer)
    pushWord(1);          // wObjectNum = 1
    
    // Single player entry (same format as OnCharInfoListReq)
    pushDWord(objCopy.dwObjectID);
    pushWord(objCopy.wPosX); pushWord(objCopy.wPosY); pushByte(objCopy.bHeight);
    if (objCopy.bIsMoving) {
        pushWord(objCopy.wMoveDesX); pushWord(objCopy.wMoveDesY); pushByte(objCopy.bMoveDesH);
    } else {
        pushWord(objCopy.wPosX); pushWord(objCopy.wPosY); pushByte(objCopy.bHeight);
    }
    pushByte(objCopy.wWalkSpeed & 0xFF);
    pushWord(0); // wDirection
    pushByte(0); // bState
    pushByte(objCopy.bPropType); // bCharType
    pushString(objCopy.szName);
    pushDWord(0); // dwFame
    pushByte(objCopy.bShopStatus); // bShopStatus
    pushString(objCopy.strShopName); // strShopName
    pushString(objCopy.strShopDescription); // strShopDescription
    pushByte(0); // bSemiPKStatus
    pushByte(0); // bCurFiveElm
    pushByte(0); // bFELevel
    pushByte(0); // bInstanceCnt
    pushByte(0); // bChangeItemSet
    pushDWord(0); // dwMunpaID
    pushDWord(0); // dwPartyID
    for(int i=0; i<9; i++) {
        pushWord(0); pushByte(0); pushByte(0); pushByte(0);
    }
    pushByte(0); // bRebirth
    pushByte(0); // bPoisonUnderCover
    pushDWord(0); // bwGMMark
    
    std::vector<BYTE> fullAck; fullAck.resize(4);
    fullAck.insert(fullAck.end(), ackBuf.begin(), ackBuf.end());
    PACKET_HEADER* ah = (PACKET_HEADER*)fullAck.data();
    ah->id = 0x4412;   // CS_IT_CHARINFOLIST_ACK (proven format!)
    ah->payloadSize = (WORD)ackBuf.size();
    EncryptPacket(fullAck.data(), 0x42);
    SafeSend(clientSocket, (const char*)fullAck.data(), fullAck.size(), 0);
    
    LOG("[MapHandler] Sent CHARINFOLIST_ACK (0x4412) for single ObjectID=" + std::to_string(reqObjectID) + " at (" + std::to_string(objCopy.wPosX) + "," + std::to_string(objCopy.wPosY) + ") moving=" + std::to_string(objCopy.bIsMoving) + " bCharType=" + std::to_string(objCopy.bPropType) + " name=" + objCopy.szName);
}


void OnCharInfoListReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    DWORD reqMapID = *(DWORD*)(payload);
    std::unordered_set<DWORD> reqIDs;
    if (totalSize >= 6) {
        WORD wCount = *(WORD*)(payload + 4);
        int actualCount = (totalSize - 6) / 4;
        int processCount = (wCount < actualCount) ? wCount : actualCount;
        for (int i = 0; i < processCount; i++) reqIDs.insert(*(DWORD*)(payload + 6 + i*4));
    }
    
    std::vector<BYTE> ackBuf; ackBuf.reserve(1024);
    ackBuf.push_back(0); // bResult
    ackBuf.push_back(reqMapID & 0xFF); ackBuf.push_back((reqMapID>>8)&0xFF); ackBuf.push_back((reqMapID>>16)&0xFF); ackBuf.push_back(reqMapID>>24); // dwMapID
    
    WORD count = 0;
    if (g_MapInstances.count(reqMapID)) {
        CMapInstance* mapInst = g_MapInstances[reqMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        for (auto& pair : mapInst->GetPlayers()) {
            if (reqIDs.count(pair.second.dwObjectID)) count++;
        }
    }
    ackBuf.push_back(count & 0xFF); ackBuf.push_back(count >> 8);
    
    auto pushDWord = [&](DWORD d) { ackBuf.push_back(d & 0xFF); ackBuf.push_back((d>>8)&0xFF); ackBuf.push_back((d>>16)&0xFF); ackBuf.push_back((d>>24)&0xFF); };
    auto pushWord = [&](WORD w) { ackBuf.push_back(w & 0xFF); ackBuf.push_back((w>>8)&0xFF); };
    auto pushByte = [&](BYTE b) { ackBuf.push_back(b); };
    auto pushString = [&](const std::string& str) {
        WORD len = str.length();
        pushWord(len);
        for (char c : str) pushByte(c);
    };

    if (g_MapInstances.count(reqMapID)) {
        CMapInstance* mapInst = g_MapInstances[reqMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        for (auto& pair : mapInst->GetPlayers()) {
            auto& o = pair.second;
            if (reqIDs.count(o.dwObjectID)) {
                pushDWord(o.dwObjectID);
                pushWord(o.wPosX); pushWord(o.wPosY); pushByte(o.bHeight);
                pushWord(o.wPosX); pushWord(o.wPosY); pushByte(o.bHeight); // Dest
                pushByte(o.wWalkSpeed & 0xFF);
                pushWord(0); // wDirection
                pushByte(0); // bState
                pushByte(o.bPropType); // bCharType
                pushString(o.szName);
                pushDWord(0); // dwFame
                
                pushByte(o.bShopStatus); // bShopStatus
                pushString(o.strShopName); // strShopName
                pushString(o.strShopDescription); // strShopDescription
                pushByte(0); // bSemiPKStatus
                pushByte(0); // bCurFiveElm
                pushByte(0); // bFELevel
                pushByte(0); // bInstanceCnt
                
                pushByte(0); // bChangeItemSet
                pushDWord(0); // dwMunpaID
                pushDWord(0); // dwPartyID
                
                // Visuals (interleaved as expected by client)
                for(int i=0; i<9; i++) {
                    pushWord(0); // wVisualID
                    pushByte(0); // bRarity
                    pushByte(0); // bStxType
                    pushByte(0); // bNeedCharType
                }
                
                pushByte(0); // bRebirth
                pushByte(0); // bPoisonUnderCover
                pushDWord(0); // bwGMMark
            }
        }
    }
    
    std::vector<BYTE> fullAck; fullAck.resize(4);
    fullAck.insert(fullAck.end(), ackBuf.begin(), ackBuf.end());
    PACKET_HEADER* ah = (PACKET_HEADER*)fullAck.data(); ah->id = 0x4412; ah->payloadSize = (WORD)ackBuf.size();
    EncryptPacket(fullAck.data(), 0x42); 
    SafeSend(clientSocket, (const char*)fullAck.data(), fullAck.size(), 0);
    LOG("[MapHandler] Sent PC Info List for Map " + std::to_string(reqMapID) + " count: " + std::to_string(count));

    // For any players that are already moving, queue a STARTMOVE_ACK so the client animates them immediately
    if (g_MapInstances.count(reqMapID)) {
        CMapInstance* mapInst = g_MapInstances[reqMapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        for (auto& pair : mapInst->GetPlayers()) {
            auto& o = pair.second;
            if (reqIDs.count(o.dwObjectID) && o.bIsMoving) {
                std::vector<BYTE> moveBuf; moveBuf.resize(4);
                moveBuf.push_back(0); // bResult
                DWORD mid = o.dwObjectID;
                moveBuf.push_back(mid & 0xFF); moveBuf.push_back((mid>>8)&0xFF); moveBuf.push_back((mid>>16)&0xFF); moveBuf.push_back((mid>>24)&0xFF);
                moveBuf.push_back(o.wPosX & 0xFF); moveBuf.push_back(o.wPosX >> 8);
                moveBuf.push_back(o.wPosY & 0xFF); moveBuf.push_back(o.wPosY >> 8);
                moveBuf.push_back(o.bHeight);
                moveBuf.push_back(o.wMoveDesX & 0xFF); moveBuf.push_back(o.wMoveDesX >> 8);
                moveBuf.push_back(o.wMoveDesY & 0xFF); moveBuf.push_back(o.wMoveDesY >> 8);
                moveBuf.push_back(o.bMoveDesH);
                moveBuf.push_back(o.wMoveDirection & 0xFF); moveBuf.push_back(o.wMoveDirection >> 8);
                moveBuf.push_back(o.bMoveState);
                BYTE speed = (o.wWalkSpeed & 0xFF) ? (o.wWalkSpeed & 0xFF) : 11;
                moveBuf.push_back(speed);
                moveBuf.push_back(0); // bFastMove
                
                PACKET_HEADER* mh = (PACKET_HEADER*)moveBuf.data();
                mh->id = 0x430C; // CS_NV_STARTMOVE_ACK
                mh->payloadSize = moveBuf.size() - sizeof(PACKET_HEADER);
                EncryptPacket(moveBuf.data(), 0x42);
                SafeSend(clientSocket, (const char*)moveBuf.data(), moveBuf.size(), 0);
            }
        }
    }
}


void OnMapMoveReq(SOCKET clientSocket, DWORD charID, BYTE* payload, WORD totalSize) {
    if (totalSize < 8) return;
    
    DWORD dwReqMapID = *(DWORD*)(payload);
    WORD wReqPosX = *(WORD*)(payload + 4);
    WORD wReqPosY = *(WORD*)(payload + 6);
    
    DWORD dwObjectID = charID + 400000000;
    DWORD oldMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    
    if (g_MapInstances.count(oldMapID)) {
        std::lock_guard<std::mutex> lock(g_MapInstances[oldMapID]->GetMutex());
        sServerObject* pObj = g_MapInstances[oldMapID]->GetPlayer(dwObjectID);
        if (pObj && pObj->bShopStatus == 1) {
            SendSystemWarningChat(clientSocket, "[Shop] Setup is active. Map transition and teleportation blocked!");
            return;
        }
    }
    
    LOG("[MapHandler] OnMapMoveReq: charID=" + std::to_string(charID) + 
        " reqMap=" + std::to_string(dwReqMapID) + " pos=(" + std::to_string(wReqPosX) + "," + std::to_string(wReqPosY) + ")");
    
    // Portal transition: client sends current mapID with pos=(0,0)
    // Server must find which portal zone the player is in via LINKMAPLIST
    DWORD destMapID = dwReqMapID;
    WORD startX = 1024, startY = 1024;
    bool posResolved = false;
    
    if (wReqPosX == 0 && wReqPosY == 0 && dwReqMapID == oldMapID) {
        // Get player's current position
        WORD playerX = 0, playerY = 0;
        if (g_MapInstances.count(oldMapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[oldMapID]->GetMutex());
            sServerObject* pObj = g_MapInstances[oldMapID]->GetPlayer(dwObjectID);
            if (pObj) { playerX = pObj->wPosX; playerY = pObj->wPosY; }
        }
        
        LOG("[MapHandler] Portal check: player at (" + std::to_string(playerX) + "," + std::to_string(playerY) + ") on map " + std::to_string(oldMapID));
        
        // Check LINKMAPLIST for portal zones �?includes wStartPosX/wStartPosY for destination spawn
        bool foundPortal = false;
        std::vector<GameDataDB::LinkMapEntry> portalLinks;
        GameDataDB::GetInstance().GetLinkMapList(oldMapID, portalLinks);
        for (auto& pl : portalLinks) {
            if (foundPortal) break;
            int px = pl.wPortalPosX, py = pl.wPortalPosY, pw = pl.wPortalWidth, ph = pl.wPortalHeight;
            int spx = pl.wStartPosX, spy = pl.wStartPosY;
            int linkMap = pl.dwLinkMapID;
            
            LOG("[MapHandler] Portal zone: (" + std::to_string(px) + "," + std::to_string(py) + " " + std::to_string(pw) + "x" + std::to_string(ph) + ") -> Map " + std::to_string(linkMap) + " spawn(" + std::to_string(spx) + "," + std::to_string(spy) + ")");
            
            if (playerX >= px && playerX <= px + pw && playerY >= py && playerY <= py + ph) {
                destMapID = linkMap;
                startX = spx;
                startY = spy;
                foundPortal = true;
                posResolved = true;
                LOG("[MapHandler] Portal HIT! -> Map " + std::to_string(linkMap) + " at (" + std::to_string(spx) + "," + std::to_string(spy) + ")");
            }
        }
        
        if (!foundPortal) {
            LOG("[MapHandler] OnMapMoveReq: No portal found at player position (" + std::to_string(playerX) + "," + std::to_string(playerY) + ")");
            return;
        }
    }
    
    // Fallback: death respawn or explicit pos �?use LOCATION table
    if (!posResolved) {
        if (wReqPosX != 0 || wReqPosY != 0) {
            startX = wReqPosX;
            startY = wReqPosY;
        } else {
            int spX = startX, spY = startY;
            CharacterDB::GetInstance().GetSpawnPosition(destMapID, spX, spY);
            startX = spX; startY = spY;
        }
    }
    
    LOG("[MapHandler] OnMapMoveReq: Moving to map " + std::to_string(destMapID) + " at (" + std::to_string(startX) + "," + std::to_string(startY) + ")");
    
    // Move player between map instances (or within same map)
    sServerObject objToMove;
    bool found = false;
    if (g_MapInstances.count(oldMapID)) {
        std::lock_guard<std::mutex> lock(g_MapInstances[oldMapID]->GetMutex());
        sServerObject* pObj = g_MapInstances[oldMapID]->GetPlayer(dwObjectID);
        if (pObj) {
            objToMove = *pObj;
            found = true;
            g_MapInstances[oldMapID]->RemovePlayer(dwObjectID);
        }
    }
    
    if (found) {
        // Restore full HP/IP
        objToMove.dwHpCur = objToMove.dwHpMax;
        objToMove.wIpCur = objToMove.wIpMax;
        objToMove.dwDeadTime = 0; // Clear death state
        objToMove.dwInvulnerableUntil = GetTickCount() + 15000; // 15s respawn protection
        objToMove.dwMapID = destMapID;
        objToMove.wPosX = startX;
        objToMove.wPosY = startY;
        objToMove.fPosX = (float)startX;
        objToMove.fPosY = (float)startY;
        objToMove.bIsMoving = false;
        
        if (g_MapInstances.count(destMapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[destMapID]->GetMutex());
            g_MapInstances[destMapID]->AddPlayer(objToMove);
        }
    }
    
    // Update session and DB
    SessionMgr::GetInstance().SetMapID(clientSocket, destMapID);
    CharacterDB::GetInstance().SavePosition(charID, startX, startY, destMapID);
    
    // Send CS_NV_MAPMOVE_ACK (0x4308) to client
    auto pushDWord = [&](std::vector<BYTE>& buf, DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back((d>>24)&0xFF); };
    auto pushWord = [&](std::vector<BYTE>& buf, WORD w) { buf.push_back(w&0xFF); buf.push_back((w>>8)&0xFF); };
    
    std::vector<BYTE> mvBuf(4);
    mvBuf.push_back(0); // bResult = success
    pushDWord(mvBuf, destMapID);
    pushWord(mvBuf, startX);
    pushWord(mvBuf, startY);
    mvBuf.push_back(0); // bLinkMapType
    
    PACKET_HEADER* mvHead = (PACKET_HEADER*)mvBuf.data();
    mvHead->id = 0x4308; // CS_NV_MAPMOVE_ACK
    mvHead->payloadSize = mvBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(mvBuf.data(), 0x42);
    SafeSend(clientSocket, (char*)mvBuf.data(), mvBuf.size(), 0);
    
    LOG("[MapHandler] OnMapMoveReq: Sent MAPMOVE_ACK. Player " + std::to_string(charID) + 
        " moved to map " + std::to_string(destMapID) + " at (" + std::to_string(startX) + "," + std::to_string(startY) + ")");
}

#include "UnitServer.h"
#include "ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "DBHelper.h"
#include "Network/PacketRouter.h"
#include "Network/SessionMgr.h"
#include "GameObjects/PlayerManager.h"
#include "GameObjects/MapInstance.h"
#include <thread>
#include <cmath>
#include <mutex>
#include <set>
#include <mstcpip.h>

std::vector<SOCKET> g_UnitSockets;
std::map<SOCKET, DWORD> g_SocketToMap;
std::map<SOCKET, DWORD> g_SocketToChar;
std::mutex g_SocketsMutex;

void SendCharStatusInfoAck(SOCKET clientSocket, DWORD dwCharID, WORD opCode);

void UpdatePlayerStatsAndSend(SOCKET clientSocket, DWORD dwCharID) {
    PlayerManager::GetInstance().RecalculateStats(dwCharID, true);
}

void SendCharStatusInfoAck(SOCKET clientSocket, DWORD dwCharID, WORD opCode) {
    if (!clientSocket || dwCharID == 0) return;

    DWORD dwObjectID = (dwCharID < 800000000) ? (dwCharID + 400000000) : dwCharID;
    
    sServerObject playerObj;
    playerObj.wWepAtk = 0; playerObj.wWepDef = 0; playerObj.wWepMag = 0; playerObj.wWalkSpeed = 24;
    
    {
        DWORD mapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        if (g_MapInstances.count(mapID)) {
            CMapInstance* pMap = g_MapInstances[mapID];
            std::lock_guard<std::mutex> lock(pMap->GetMutex());
            sServerObject* pObj = pMap->GetPlayer(dwObjectID);
            if (pObj) {
                playerObj = *pObj;
            }
        }
    }

    std::vector<BYTE> ackBuf; ackBuf.resize(4);
    
    // Query using dwCharID
    std::string q = "SELECT P.wLevel, P.wStr, P.wSus, P.wDex, P.wVit, P.wIpMax, P.wIpCur, P.dwHpMax, P.dwHpCur, P.dwExp, P.dwTotalSp, P.wRemainSp, P.dwTotalTp, P.wRemainTp, P.dwMoney, P.dwFame FROM CHAR_DATA P WHERE P.dwCharID = " + std::to_string(dwCharID);
    
    auto rowCallback = [&](SQLHSTMT hStmt) {
        LOG("[UnitServer] DB Query returned a row for dwCharID: " + std::to_string(dwCharID));
        LONG lLevel=0, lStr=0, lSus=0, lDex=0, lVit=0, lIpMax=0, lIpCur=0, lRemainSp=0, lRemainTp=0;
        DWORD dwHpMax=0, dwHpCur=0, dwTotalSp=0, dwTotalTp=0, dwMoney=0, dwFame=0;
        long long int dwExp=0, levelExp=0, nextLevelExp=0;
        SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &lLevel, 0, &c); WORD wLevel = (WORD)lLevel;
        SQLGetData(hStmt, 2, SQL_C_SLONG, &lStr, 0, &c); WORD wStr = (WORD)lStr;
        SQLGetData(hStmt, 3, SQL_C_SLONG, &lSus, 0, &c); WORD wSus = (WORD)lSus;
        SQLGetData(hStmt, 4, SQL_C_SLONG, &lDex, 0, &c); WORD wDex = (WORD)lDex;
        SQLGetData(hStmt, 5, SQL_C_SLONG, &lVit, 0, &c); WORD wVit = (WORD)lVit;
        SQLGetData(hStmt, 6, SQL_C_SLONG, &lIpMax, 0, &c); WORD wIpMax = (WORD)lIpMax;
        SQLGetData(hStmt, 7, SQL_C_SLONG, &lIpCur, 0, &c); WORD wIpCur = (WORD)lIpCur;
        SQLGetData(hStmt, 8, SQL_C_ULONG, &dwHpMax, 0, &c);
        SQLGetData(hStmt, 9, SQL_C_ULONG, &dwHpCur, 0, &c);
        SQLGetData(hStmt, 10, SQL_C_SBIGINT, &dwExp, 0, &c);
        SQLGetData(hStmt, 11, SQL_C_ULONG, &dwTotalSp, 0, &c);
        SQLGetData(hStmt, 12, SQL_C_SLONG, &lRemainSp, 0, &c); WORD wRemainSp = (WORD)lRemainSp;
        SQLGetData(hStmt, 13, SQL_C_ULONG, &dwTotalTp, 0, &c);
        SQLGetData(hStmt, 14, SQL_C_SLONG, &lRemainTp, 0, &c); WORD wRemainTp = (WORD)lRemainTp;
        SQLGetData(hStmt, 15, SQL_C_ULONG, &dwMoney, 0, &c);
        SQLGetData(hStmt, 16, SQL_C_ULONG, &dwFame, 0, &c);
        
        // Calculate Max HP and Max IP dynamically based on EXACT official formulas for this class:
        // HP = (Stamina * bIncHp) + ((Level - 1) * bIncHpLevel) + EquipHP
        // IP = (Energy * bIncIp) + ((Level - 1) * bIncIpLevel) + EquipIP
        dwHpMax = (wSus * 8) + ((wLevel - 1) * 8) + playerObj.wEquipHp;
        wIpMax = (wVit * 0) + ((wLevel - 1) * 4) + playerObj.wEquipIp;
        
        // Clamp current HP/MP to the newly calculated maxes
        if (dwHpCur > dwHpMax || dwHpCur <= 8) dwHpCur = dwHpMax;
        if (wIpCur > wIpMax || wIpCur == 0) wIpCur = wIpMax;
        
        // Fetch from memory cache
        if (g_LevelTemplates.count(wLevel)) {
            levelExp = g_LevelTemplates[wLevel].begin()->second.dwNeedExp;
        }
        if (g_LevelTemplates.count(wLevel + 1)) {
            nextLevelExp = g_LevelTemplates[wLevel + 1].begin()->second.dwNeedExp;
        }
        
        if (nextLevelExp <= levelExp) nextLevelExp = levelExp + 1000;

        long long int tpExp = 0;
        long long int nextTpExp = 1000;
        
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
            pushDword(playerObj.dwTotalAtk); // dwTotalAtkPower
            pushDword(playerObj.dwTotalDef);  // dwTotalDefPower
            pushDword(playerObj.dwTotalHit);  // dwTotalAttackRating
            ackBuf.push_back(0); // bState
            ackBuf.push_back(playerObj.wWalkSpeed & 0xFF); // bWalkSpeed
            pushDword(dwHpCur); pushDword(dwHpMax); 
            pushWord(wIpCur); pushWord(wIpMax);
            pushWord(playerObj.wCritical); // wCritical
            pushWord(wStr); pushWord(wSus * 2); pushWord(wDex); // wBaseAtkPwr, wBaseDefPwr, wBaseAttackRating
            ackBuf.push_back(10); // bAttackSpeed
            pushWord(20); // wAttackRange
            ackBuf.push_back(playerObj.wPlusSpeed & 0xFF); // bPlusSpeed
            LOG("[UnitServer] CS_IF_CHARINFO_ACK: wCritical=" + std::to_string(playerObj.wCritical) + " bPlusSpeed=" + std::to_string(playerObj.wPlusSpeed) + " wWalkSpeed=" + std::to_string(playerObj.wWalkSpeed) + " dwTotalAtk=" + std::to_string(playerObj.dwTotalAtk) + " dwTotalDef=" + std::to_string(playerObj.dwTotalDef));
            pushWord(wRemainTp); pushDword(dwTotalTp);
            pushDword(dwFame);
            ackBuf.push_back(0); // bChangeItemSet
            BYTE bRebirth = 0; // Fetch from DB if needed, default to 0
            ackBuf.push_back(bRebirth);
            pushDword(0); // dwPremiumTP
            pushDword(0); // dwPremiumSP
            pushDword(0); // dwGameMasterMark
        } else if (opCode == CS_IT_CHARSTATUSINFO_ACK) {
            pushWord(wLevel); pushWord(wStr); pushWord(wSus); pushWord(wDex); pushWord(wVit);
            ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); ackBuf.push_back(0); // IncrStr, IncrSus, IncrDex, IncrVit
            pushWord(wIpMax); pushWord(wIpCur); pushDword(dwHpMax); pushDword(dwHpCur);
            pushInt64(dwExp); pushInt64(levelExp); pushInt64(nextLevelExp); pushInt64(tpExp); pushInt64(nextTpExp); // Exp Fields
            pushDword(dwTotalSp); pushWord(wRemainSp); pushDword(dwTotalTp); pushWord(wRemainTp);
            pushWord(wStr); pushDword(playerObj.dwTotalAtk); // BaseAtk, TotalAtk
            pushWord(wSus * 2); pushDword(playerObj.dwTotalDef); // BaseDef, TotalDef
            pushWord(wDex); pushDword(playerObj.dwTotalHit); // BaseAtkRat, TotalAtkRat
            pushWord(20); // wAttackRange
            ackBuf.push_back(playerObj.wWalkSpeed & 0xFF); ackBuf.push_back(playerObj.wWalkSpeed & 0xFF); ackBuf.push_back(playerObj.wPlusSpeed & 0xFF); // WalkSpeed, RunSpeed, PlusSpeed
            ackBuf.push_back(0); // bJumpLevel
            pushDword(0); // dwPkCnt
            pushDword(dwMoney); // dwMoney
            pushWord(playerObj.wCritical); // wCritical
            ackBuf.push_back(10); ackBuf.push_back(0); // bAttackSpeed, bState
            pushDword(dwFame);
            // Five Elm
            pushWord(0); // wFiveElmPoint
            pushDword(0); pushDword(0); pushDword(0); // Power, PowerMax, Gauge
            pushWord(0); pushWord(0); pushWord(0); pushWord(0); pushWord(0); // Exps
            ackBuf.push_back(0); // bRebirth
        }
    };
    
    LOG("[UnitServer] Querying DB for dwCharID: " + std::to_string(dwCharID) + " opCode: " + std::to_string(opCode));
    bool dbRet = DBHelper::GetInstance().ExecuteQuery(q, rowCallback);
    LOG("[UnitServer] ExecuteQuery returned " + std::to_string(dbRet) + ", ackBuf size: " + std::to_string(ackBuf.size()));
    
    if (ackBuf.size() > 4) {
        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
        ackHead->id = opCode;
        ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
        
        std::string hexDump = "";
        for (size_t i = 0; i < ackBuf.size(); ++i) {
            char buf[10];
            sprintf(buf, "%02X ", ackBuf[i]);
            hexDump += buf;
        }
        LOG("[UnitServer] Sending opCode 0x" + std::to_string(opCode) + " size " + std::to_string(ackBuf.size()) + "\nDump: " + hexDump);

        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    }
}

void BroadcastPacket(const std::vector<BYTE>& packet) {
    std::lock_guard<std::mutex> lock(g_SocketsMutex);
    for (SOCKET s : g_UnitSockets) {
        SafeSend(s, (const char*)packet.data(), packet.size(), 0);
    }
}

#include "MonsterAI.h"

void RunUnitSvr() {
    InitPacketHandlers();
    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) return;

    sockaddr_in serverAddr;
    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family = AF_INET;
    if (g_Config.internalIP == "0.0.0.0") {
        serverAddr.sin_addr.s_addr = INADDR_ANY;
    } else {
        serverAddr.sin_addr.s_addr = inet_addr(g_Config.internalIP.c_str());
    }
    serverAddr.sin_port = htons(g_Config.unitPort);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::string err = "[UnitSvr " + std::to_string(g_Config.unitPort) + "] Bind failed! Error: " + std::to_string(WSAGetLastError()) + "\nPlease close the official UnitSvr.exe and restart!";
        LOG(err);
        MessageBoxA(NULL, err.c_str(), "Port Conflict", MB_ICONERROR);
        return;
    }
    listen(listenSocket, SOMAXCONN);
    LOG("[UnitSvr " + std::to_string(g_Config.unitPort) + "] Online and listening...");

    std::thread(MonsterAIThread).detach();

    while (true) {
        SOCKET clientSocket = accept(listenSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) continue;
        // Enable TCP keepalive to prevent idle timeout on remote servers
        BOOL bKeepAlive = TRUE;
        setsockopt(clientSocket, SOL_SOCKET, SO_KEEPALIVE, (const char*)&bKeepAlive, sizeof(bKeepAlive));
        // Set keepalive interval to 5 seconds
        DWORD dwKeepAliveTime = 5000;    // 5 sec before first probe
        DWORD dwKeepAliveInterval = 5000; // 5 sec between probes
        struct tcp_keepalive ka = { 1, dwKeepAliveTime, dwKeepAliveInterval };
        DWORD dwBytesReturned = 0;
        WSAIoctl(clientSocket, SIO_KEEPALIVE_VALS, &ka, sizeof(ka), NULL, 0, &dwBytesReturned, NULL, NULL);
        LOG("[UnitSvr " + std::to_string(g_Config.unitPort) + "] Game Client arrived!");

        {
            std::lock_guard<std::mutex> lock(g_SocketsMutex);
            g_UnitSockets.push_back(clientSocket);
        }

        std::thread([clientSocket]() {
            srand((unsigned int)time(NULL) ^ (unsigned int)std::hash<std::thread::id>{}(std::this_thread::get_id()));
            try {
                std::vector<BYTE> initBuf(5);
                PACKET_HEADER* initHead = (PACKET_HEADER*)initBuf.data();
                initHead->id = 0; initHead->payloadSize = initBuf.size() - sizeof(PACKET_HEADER); initBuf[4] = 0x42;
                SafeSend(clientSocket, (const char*)initBuf.data(), initBuf.size(), 0);

                std::string clientAccountName = "dustwj"; // Default fallback

                while (true) {
                    char headerBuf[4];
                    int bytesReceived = 0;
                    while (bytesReceived < 4) {
                        int chunk = recv(clientSocket, headerBuf + bytesReceived, 4 - bytesReceived, 0);
                        if (chunk <= 0) break;
                        bytesReceived += chunk;
                    }
                    if (bytesReceived < 4) break; 

                    if (headerBuf[0] == 0 && headerBuf[1] == 0 && headerBuf[2] == 0 && headerBuf[3] == 0) {
                        char dummy[2];
                        recv(clientSocket, dummy, 2, 0);
                        continue;
                    }

                    WORD wPayloadSize = *(WORD*)(headerBuf + 2);
                    WORD wTotalSize = wPayloadSize + 4;
                    if (wTotalSize < 4 || wTotalSize > 4000) {
                        char hex[128];
                        sprintf(hex, "Dropped spam packet, payload size: %d, HEX: %02X %02X %02X %02X", wPayloadSize, (BYTE)headerBuf[0], (BYTE)headerBuf[1], (BYTE)headerBuf[2], (BYTE)headerBuf[3]);
                        LOG("[UnitSvr " + std::to_string(g_Config.unitPort) + "] " + std::string(hex));
                        break; 
                    }

                    char* payloadBuf = new char[wPayloadSize];
                    bytesReceived = 0;
                    while (bytesReceived < wPayloadSize) {
                        int chunk = recv(clientSocket, payloadBuf + bytesReceived, wPayloadSize - bytesReceived, 0);
                        if (chunk <= 0) break;
                        bytesReceived += chunk;
                    }
                    if (bytesReceived < wPayloadSize) {
                        delete[] payloadBuf;
                        break;
                    }

                    std::vector<BYTE> fullPacket(wTotalSize);
                    memcpy(fullPacket.data(), headerBuf, 4);
                    memcpy(fullPacket.data() + 4, payloadBuf, wPayloadSize);
                    delete[] payloadBuf;

                    std::string rawDump = "[UnitServer] Raw Recv (" + std::to_string(wTotalSize) + " bytes): ";
                    for (size_t i = 0; i < fullPacket.size(); i++) {
                        char h[10]; sprintf(h, "%02X ", fullPacket[i]);
                        rawDump += h;
                    }
                    LOG(rawDump);

                    DecryptPacket(fullPacket.data(), 0x42);
                    PACKET_HEADER* header = (PACKET_HEADER*)fullPacket.data();

                    if (RoutePacket(clientSocket, header, fullPacket)) {
                        // Handled by PacketRouter
                    } else if (header->id == 0x4437) {
                        std::vector<BYTE> ackBuf; ackBuf.resize(4); ackBuf.push_back(0); 
                        DWORD mapId = 6; ackBuf.push_back(mapId&0xFF); ackBuf.push_back((mapId>>8)&0xFF); ackBuf.push_back((mapId>>16)&0xFF); ackBuf.push_back(mapId>>24);
                        ackBuf.push_back(0); ackBuf.push_back(0); 
                        ackBuf.push_back(0); ackBuf.push_back(8); 
                        ackBuf.push_back(0); ackBuf.push_back(8); 
                        ackBuf.push_back(0); ackBuf.push_back(0); 
                        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data(); ackHead->id = 0x4438; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                        EncryptPacket(ackBuf.data(), 0x42); SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);

                    } else if ((header->id >= 0x4000 && header->id <= 0x4010) || header->id == 0x442B) {
                        // COMBAT PACKET INTERCEPTOR
                        std::string hexStr = "";
                        BYTE* payload = (BYTE*)fullPacket.data() + sizeof(PACKET_HEADER);
                        for(int i=0; i<wPayloadSize; i++) {
                            char buf[4]; sprintf(buf, "%02X ", payload[i]);
                            hexStr += buf;
                        }
                        LOG("[COMBAT INTERCEPT] ID: 0x" + std::to_string(header->id) + " (" + std::to_string(header->id) + ") Size: " + std::to_string(wPayloadSize) + " Hex: " + hexStr);
                    } else {
                        char dHex[128];
                        sprintf(dHex, "UNKNOWN PACKET! ID: 0x%04X, Size: %d", header->id, wPayloadSize);
                        LOG(std::string(dHex));
                    }
                }
            } catch (...) {}
            
            // Save player data on disconnect
            DWORD disconCharID = SessionMgr::GetInstance().GetCharID(clientSocket);
            if (disconCharID > 0) {
                PlayerManager::GetInstance().SavePlayer(disconCharID);
            }

            {
                std::lock_guard<std::mutex> lock(g_SocketsMutex);
                if (g_SocketToChar.count(clientSocket)) {
                    DWORD charID = g_SocketToChar[clientSocket];
                    DWORD mapID = g_SocketToMap[clientSocket];
                    if (g_MapInstances.count(mapID)) {
                        CMapInstance* pMap = g_MapInstances[mapID];
                        std::lock_guard<std::mutex> lockP(pMap->GetMutex());
                        pMap->RemovePlayer(charID + 400000000); // Remove player from map instance
                    }
                }
                g_UnitSockets.erase(std::remove(g_UnitSockets.begin(), g_UnitSockets.end(), clientSocket), g_UnitSockets.end());
                g_SocketToMap.erase(clientSocket);
                g_SocketToChar.erase(clientSocket);
            }
            closesocket(clientSocket);
            LOG("[UnitSvr " + std::to_string(g_Config.unitPort) + "] Client disconnected.");
        }).detach();
    }
}


bool GrantExpToPlayer(DWORD dwCharID, DWORD dwIncrExp) {
    if (dwIncrExp == 0) return false;
    
    SOCKET clientSocket = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
    if (clientSocket == INVALID_SOCKET) return false;
    
    // Query from CHAR_POWER (the real table, not CHAR_DATA view)
    // Need bCharType from CHAR_BASIC for LEVELTEMPLATE lookup
    std::string q = "SELECT P.dwExp, P.wLevel, P.dwTotalTp, P.wRemainTp, P.wRemainSp, P.dwTotalSp, B.bCharType "
                    "FROM CHAR_POWER P INNER JOIN CHAR_BASIC B ON P.dwCharID = B.dwCharID "
                    "WHERE P.dwCharID = " + std::to_string(dwCharID);
    
    long long int oldExp = 0, newExp = 0;
    WORD wLevel = 0;
    DWORD dwTotalTp = 0;
    WORD wRemainTp = 0, wRemainSp = 0;
    DWORD dwTotalSp = 0;
    BYTE bCharType = 1;
    BYTE bLevelUp = 0, bTpUp = 0;
    bool queryOk = false;
    
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_SBIGINT, &oldExp, 0, &c);
        SQLGetData(hStmt, 2, SQL_C_USHORT, &wLevel, 0, &c);
        SQLGetData(hStmt, 3, SQL_C_ULONG, &dwTotalTp, 0, &c);
        SQLGetData(hStmt, 4, SQL_C_USHORT, &wRemainTp, 0, &c);
        SQLGetData(hStmt, 5, SQL_C_USHORT, &wRemainSp, 0, &c);
        SQLGetData(hStmt, 6, SQL_C_ULONG, &dwTotalSp, 0, &c);
        SQLGetData(hStmt, 7, SQL_C_UTINYINT, &bCharType, 0, &c);
        queryOk = true;
    });
    
    if (!queryOk) return false;
    
    newExp = oldExp + dwIncrExp;
    
    LOG("[GrantExp] charID=" + std::to_string(dwCharID) + " charType=" + std::to_string(bCharType)
        + " oldExp=" + std::to_string(oldExp) + " +incr=" + std::to_string(dwIncrExp) + " =newExp=" + std::to_string(newExp)
        + " curLvl=" + std::to_string(wLevel) + " remainTp=" + std::to_string(wRemainTp) + " remainSp=" + std::to_string(wRemainSp));
    
    // --- Level-up detection ---
    // Lookup LEVELTEMPLATE by [wLevel][bCharType], fallback to first bCharType
    auto getLevelExp = [&](WORD lvl) -> long long int {
        if (!g_LevelTemplates.count(lvl)) return 0;
        auto& charMap = g_LevelTemplates[lvl];
        if (charMap.count(bCharType)) return charMap[bCharType].dwNeedExp;
        return charMap.begin()->second.dwNeedExp;
    };
    auto getLevelSp = [&](WORD lvl) -> BYTE {
        if (!g_LevelTemplates.count(lvl)) return 0;
        auto& charMap = g_LevelTemplates[lvl];
        if (charMap.count(bCharType)) return charMap[bCharType].bSp;
        return charMap.begin()->second.bSp;
    };
    
    long long int levelExp = getLevelExp(wLevel);       // EXP threshold for current level
    long long int nextLevelExp = getLevelExp(wLevel + 1); // EXP threshold for next level
    if (nextLevelExp <= levelExp) nextLevelExp = levelExp + 1000; // Safety
    
    LOG("[GrantExp] levelExp[" + std::to_string(wLevel) + "]=" + std::to_string(levelExp)
        + " nextLevelExp[" + std::to_string(wLevel+1) + "]=" + std::to_string(nextLevelExp)
        + " needLvlUp=" + std::to_string(newExp >= nextLevelExp));
    
    long long int i64NextLevelUpExp = nextLevelExp;
    
    // Check level-up
    if (newExp >= nextLevelExp && g_LevelTemplates.count(wLevel + 1)) {
        bLevelUp = 1;
        BYTE spGain = getLevelSp(wLevel + 1); // SP granted by reaching this new level
        wLevel++;
        wRemainSp += spGain;
        dwTotalSp += spGain;
        // Update thresholds for the new level
        levelExp = getLevelExp(wLevel);
        nextLevelExp = getLevelExp(wLevel + 1);
        if (nextLevelExp <= levelExp) nextLevelExp = levelExp + 1000;
        i64NextLevelUpExp = nextLevelExp;
        LOG("[GrantExp] LEVEL UP! newLvl=" + std::to_string(wLevel) + " spGain=" + std::to_string(spGain)
            + " newRemainSp=" + std::to_string(wRemainSp) + " newLevelExp=" + std::to_string(levelExp)
            + " newNextLevelExp=" + std::to_string(nextLevelExp));
    }
    
    // --- TP segment detection (6 segments per level → 6 TP per level) ---
    // The TP bar divides each level's EXP range into 6 equal segments.
    // When EXP crosses a segment boundary, bTpUp=1 and wRemainTp++.
    long long int diff = nextLevelExp - levelExp;
    long long int segSize = diff / 6;
    if (segSize <= 0) segSize = 1;
    
    // Calculate which segment the OLD exp was in
    long long int oldOffset = oldExp - levelExp;
    if (oldOffset < 0) oldOffset = 0;
    long long int oldSegIndex = oldOffset / segSize;
    if (oldSegIndex > 5) oldSegIndex = 5;
    
    // Calculate which segment the NEW exp is in
    long long int newOffset = newExp - levelExp;
    if (newOffset < 0) newOffset = 0;
    long long int newSegIndex = newOffset / segSize;
    if (newSegIndex > 5) newSegIndex = 5;
    
    // If we crossed segment boundaries, grant TP
    // (level-up resets segment tracking, so we only check within current level)
    long long int i64NextTpUpExp = 0;
    if (!bLevelUp && newSegIndex > oldSegIndex) {
        // Crossed (newSegIndex - oldSegIndex) segment boundaries
        WORD tpGained = (WORD)(newSegIndex - oldSegIndex);
        bTpUp = 1;
        wRemainTp += tpGained;
        dwTotalTp += tpGained;
    }
    
    // If level-up happened, the old level's remaining TP segments are granted,
    // then start fresh from new level's segment 0
    if (bLevelUp) {
        // Grant any remaining segments from old level that weren't yet reached
        WORD oldLevelRemaining = (WORD)(6 - oldSegIndex);
        if (oldLevelRemaining > 0 && oldLevelRemaining <= 6) {
            bTpUp = 1;
            wRemainTp += oldLevelRemaining;
            dwTotalTp += oldLevelRemaining;
        }
        // Reset segment tracking for new level (segment 0)
        newSegIndex = 0;
        if (newOffset > 0) {
            // If newExp is already partway into the new level
            long long int newLevelOffset = newExp - levelExp;
            if (newLevelOffset > 0) {
                long long int newLevelDiff = nextLevelExp - levelExp;
                long long int newSegSz = newLevelDiff / 6;
                if (newSegSz > 0) {
                    newSegIndex = newLevelOffset / newSegSz;
                    if (newSegIndex > 5) newSegIndex = 5;
                    if (newSegIndex > 0) {
                        bTpUp = 1;
                        wRemainTp += (WORD)newSegIndex;
                        dwTotalTp += (WORD)newSegIndex;
                    }
                }
            }
        }
    }
    
    // Compute TP segment boundaries for the client
    long long int curSegBase = levelExp + (newSegIndex * segSize);
    long long int curSegNext = curSegBase + segSize;
    if (newSegIndex == 5) curSegNext = nextLevelExp;
    i64NextTpUpExp = curSegNext;
    
    // --- Update CHAR_POWER (the real table) ---
    std::string uQ = "UPDATE CHAR_POWER SET dwExp = " + std::to_string(newExp);
    if (bLevelUp) uQ += ", wLevel = " + std::to_string(wLevel);
    if (bTpUp) {
        uQ += ", wRemainTp = " + std::to_string(wRemainTp);
        uQ += ", dwTotalTp = " + std::to_string(dwTotalTp);
    }
    if (bLevelUp) {
        uQ += ", wRemainSp = " + std::to_string(wRemainSp);
        uQ += ", dwTotalSp = " + std::to_string(dwTotalSp);
    }
    uQ += " WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(uQ);
    
    // --- Build 0x3B10 (CS_IF_CHAREXP_ACK) packet ---
    std::vector<BYTE> buf; buf.reserve(64);
    buf.resize(4); 
    
    auto pushDWord = [&](DWORD d) { buf.push_back(d&0xFF); buf.push_back((d>>8)&0xFF); buf.push_back((d>>16)&0xFF); buf.push_back(d>>24); };
    auto pushInt64 = [&](long long int d) {
        pushDWord((DWORD)(d & 0xFFFFFFFF));
        pushDWord((DWORD)((d >> 32) & 0xFFFFFFFF));
    };
    auto pushWord = [&](WORD w) { buf.push_back(w&0xFF); buf.push_back(w>>8); };
    
    pushDWord(dwIncrExp);        // dwIncrExp
    pushInt64(newExp);           // n64Exp (current total)
    buf.push_back(bLevelUp);    // bLevelUp
    buf.push_back(bTpUp);       // bTpUp
    buf.push_back(0);           // bFiveElmLevelUp
    pushInt64(i64NextLevelUpExp); // i64NextLevelUpExp
    pushInt64(i64NextTpUpExp);   // i64NextTpUpExp
    pushWord(0);                // wFiveElmPoint
    pushDWord(0);               // dwFiveElmPower
    pushDWord(0);               // dwFiveElmPowerMax
    pushDWord(0);               // dwFiveElmGauge
    pushDWord(0);               // dwEventExp
    pushWord(0);                // wFiveElmExp
    
    PACKET_HEADER* head = (PACKET_HEADER*)buf.data();
    head->id = 0x3B10;
    head->payloadSize = buf.size() - sizeof(PACKET_HEADER); 
    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), buf.size(), 0);
    
    // NOTE: Do NOT call SendCharStatusInfoAck here!
    // Caller must call it AFTER releasing the map mutex.
    
    LOG("[UnitServer] Granted " + std::to_string(dwIncrExp) + " EXP to player " + std::to_string(dwCharID) 
        + " | LvlUp=" + std::to_string(bLevelUp) + " TpUp=" + std::to_string(bTpUp) 
        + " NewExp=" + std::to_string(newExp) + " Level=" + std::to_string(wLevel));
    return bLevelUp || bTpUp;
}



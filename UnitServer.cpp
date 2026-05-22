#include "UnitServer.h"
#include "ServerCore.h"
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
#include "Handlers/TradeHandler.h"
#include "../XiahClient/csprotocol.h"
#include <ctime>

static uint64_t CalculateFNV1aHMAC(const BYTE* data, size_t len, DWORD key) {
    uint64_t hash = 14695981039346656037ULL;
    hash ^= key;
    hash *= 1099511628211ULL;
    for (size_t i = 0; i < len; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

// All business logic (SendCharStatusInfoAck, UpdatePlayerStatsAndSend, 
// GrantExpToPlayer, BroadcastPacketToMap) has been migrated to:
//   - GameObjects/PlayerManager.cpp
//   - GameObjects/ExpSystem.cpp

void BroadcastPacket(const std::vector<BYTE>& packet) {
    SessionMgr::GetInstance().BroadcastToAll(packet);
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

        SessionMgr::GetInstance().AddConnection(clientSocket);

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

                    // --- Phase B: Security Anti-Replay & HMAC Signature Interceptor ---
                    if (header->id != 0) { // Skip handshake init packet
                        WORD totalPayloadSize = header->payloadSize;
                        if (totalPayloadSize < 16) {
                            LOG("[UnitServer] SECURITY ALERT: Packet too short for security fields! wPayloadSize=" + std::to_string(totalPayloadSize) + " Kicking client.");
                            closesocket(clientSocket);
                            break;
                        }
                        
                        WORD originalPayloadSize = totalPayloadSize - 16;
                        BYTE* payloadStart = fullPacket.data() + sizeof(PACKET_HEADER);
                        
                        uint32_t seq = *(uint32_t*)(payloadStart + originalPayloadSize);
                        uint32_t timestamp = *(uint32_t*)(payloadStart + originalPayloadSize + 4);
                        uint64_t clientHmac = *(uint64_t*)(payloadStart + originalPayloadSize + 8);
                        
                        // Get Ticket ID (session secret key)
                        DWORD ticketId = 0;
                        if (header->id == CS_IT_LOGINCHECK_REQ) {
                            WORD nameLen = *(WORD*)(payloadStart);
                            if (nameLen > 0 && nameLen <= 256 && 2 + nameLen + 4 <= originalPayloadSize) {
                                ticketId = *(DWORD*)(payloadStart + 2 + nameLen);
                            }
                        } else {
                            ticketId = SessionMgr::GetInstance().GetTicketId(clientSocket);
                        }
                        
                        if (ticketId == 0) {
                            LOG("[UnitServer] SECURITY ALERT: Unauthorized packet before login! Kicking client.");
                            closesocket(clientSocket);
                            break;
                        }
                        
                        // Validate timestamp (prevent long-term replay, 300s tolerance for clock drift)
                        uint32_t now = (uint32_t)time(nullptr);
                        if (timestamp > now + 300 || now > timestamp + 300) {
                            LOG("[UnitServer] SECURITY ALERT: Packet timestamp expired! now=" + std::to_string(now) + " ts=" + std::to_string(timestamp) + " Kicking client.");
                            closesocket(clientSocket);
                            break;
                        }
                        
                        // Validate sequence anti-replay using sliding window
                        if (!SessionMgr::GetInstance().AcceptSequence(clientSocket, seq)) {
                            LOG("[UnitServer] SECURITY ALERT: Replay packet detected! seq=" + std::to_string(seq) + " Kicking client.");
                            closesocket(clientSocket);
                            break;
                        }
                        
                        // Validate FNV-1a HMAC signature (HMAC spans up to start of HMAC field: wTotalSize - 8)
                        // Note: The client calculated the HMAC when the header's payloadSize field was totalPayloadSize - 8 (excluding only the HMAC field itself).
                        // We must temporarily adjust the server buffer's header payloadSize to match it!
                        *(WORD*)(fullPacket.data() + 2) = totalPayloadSize - 8;
                        uint64_t serverHmac = CalculateFNV1aHMAC(fullPacket.data(), wTotalSize - 8, ticketId);
                        *(WORD*)(fullPacket.data() + 2) = totalPayloadSize; // Restore for downstream safety

                        if (serverHmac != clientHmac) {
                            LOG("[UnitServer] SECURITY ALERT: HMAC Signature mismatch! Client=" + std::to_string(clientHmac) + " Server=" + std::to_string(serverHmac) + " Key=" + std::to_string(ticketId) + " Kicking client.");
                            closesocket(clientSocket);
                            break;
                        }
                        
                        // Strip security footer from payloadSize so handlers see original, unpolluted data
                        header->payloadSize = originalPayloadSize;
                    }

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
                DWORD charID = SessionMgr::GetInstance().GetCharID(clientSocket);
                DWORD mapID = SessionMgr::GetInstance().GetMapID(clientSocket);
                if (charID > 0 && g_MapInstances.count(mapID)) {
                    CMapInstance* pMap = g_MapInstances[mapID];
                    std::lock_guard<std::mutex> lockP(pMap->GetMutex());
                    pMap->RemovePlayer(charID + 400000000);
                }
                TradeManager::GetInstance().OnPlayerDisconnect(charID);
                SessionMgr::GetInstance().RemoveConnection(clientSocket);
            }
            closesocket(clientSocket);
            LOG("[UnitSvr " + std::to_string(g_Config.unitPort) + "] Client disconnected.");
        }).detach();
    }
}

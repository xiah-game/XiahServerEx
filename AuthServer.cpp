#include "AuthServer.h"
#include "ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "DBHelper.h"
#include "DB/CharacterDB.h"
#include "Network/AuthCenter.h"
#include <thread>

void RunAuthSvr() {
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
    serverAddr.sin_port = htons(g_Config.authPort);

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        std::string err = "[AuthSvr " + std::to_string(g_Config.authPort) + "] Bind failed! Error: " + std::to_string(WSAGetLastError()) + "\nPlease close the official AuthSvr.exe and restart!";
        LOG(err);
        MessageBoxA(NULL, err.c_str(), "Port Conflict", MB_ICONERROR);
        return;
    }
    listen(listenSocket, SOMAXCONN);
    LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] Online and listening...");

    while (true) {
        sockaddr_in clientAddr;
        int addrLen = sizeof(clientAddr);
        SOCKET clientSocket = accept(listenSocket, (sockaddr*)&clientAddr, &addrLen);
        if (clientSocket == INVALID_SOCKET) continue;
        std::string clientIp = inet_ntoa(clientAddr.sin_addr);
        LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] Client connected from IP: " + clientIp);

        std::thread([clientSocket, clientIp]() {
            try {
                std::vector<BYTE> initBuf(5);
                PACKET_HEADER* initHead = (PACKET_HEADER*)initBuf.data();
                initHead->id = 0; initHead->payloadSize = initBuf.size() - sizeof(PACKET_HEADER); initBuf[4] = 0x00;
                SafeSend(clientSocket, (const char*)initBuf.data(), initBuf.size(), 0);

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
                        LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] " + std::string(hex));
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

                    DecryptPacket(fullPacket.data(), 0);
                    PACKET_HEADER* header = (PACKET_HEADER*)fullPacket.data();

                    if (header->id == CS_IT_LOGIN_REQ) {
                        BYTE* payload = (BYTE*)fullPacket.data() + sizeof(PACKET_HEADER);
                        BYTE* endPtr = (BYTE*)fullPacket.data() + wTotalSize;
                        
                        std::string username = "";
                        std::string password = "";
                        if (payload + 2 <= endPtr) {
                            WORD userLen = *(WORD*)payload; payload += 2;
                            if (userLen > 0 && payload + userLen <= endPtr) {
                                username = std::string((char*)payload, userLen - 1);
                                payload += userLen;
                            }
                        }
                        if (payload + 2 <= endPtr) {
                            WORD passLen = *(WORD*)payload; payload += 2;
                            if (passLen > 0 && payload + passLen <= endPtr) {
                                password = std::string((char*)payload, passLen - 1);
                                payload += passLen;
                            }
                        }
                        
                        LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] Received CS_IT_LOGIN_REQ for user: " + username);
                        
                        // 解析客户端版本号（int, 4字节，紧跟在 password sString 之后）
                        int clientVersion = 0;
                        if (payload + 4 <= endPtr) {
                            clientVersion = *(int*)payload;
                            payload += 4;
                        }
                        LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] Client version: " + std::to_string(clientVersion) + " (required: " + std::to_string(g_Config.requiredClientVersion) + ")");

                        // 版本校验：低于服务端要求的最低版本号时直接拒绝登录
                        if (clientVersion < g_Config.requiredClientVersion) {
                            LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] VERSION MISMATCH! Client v" + std::to_string(clientVersion) + " < Required v" + std::to_string(g_Config.requiredClientVersion) + " -> Rejecting");
                            
                            std::vector<BYTE> verFailBuf; verFailBuf.resize(4);
                            verFailBuf.push_back(7); // bResult = 7 (VERSION_MISMATCH, 客户端显示为登录失败)
                            DWORD dwDummyKey = 0;
                            verFailBuf.push_back((BYTE)(dwDummyKey & 0xFF)); verFailBuf.push_back((BYTE)((dwDummyKey >> 8) & 0xFF)); verFailBuf.push_back((BYTE)((dwDummyKey >> 16) & 0xFF)); verFailBuf.push_back((BYTE)(dwDummyKey >> 24));
                            verFailBuf.push_back(18); // bAge
                            PACKET_HEADER* verHead = (PACKET_HEADER*)verFailBuf.data();
                            verHead->id = CS_IT_LOGIN_ACK; verHead->payloadSize = verFailBuf.size() - sizeof(PACKET_HEADER);
                            EncryptPacket(verFailBuf.data(), 0);
                            SafeSend(clientSocket, (const char*)verFailBuf.data(), verFailBuf.size(), 0);
                            LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] Sent VERSION_MISMATCH response. Please update client.");
                            continue;
                        }

                        BYTE bLoginResult = 1; // 1 = LOGINFAIL
                        DWORD accountId = 0;
                        if (!username.empty() && !password.empty()) {
                            bool accountExists = false;
                            bool passMatch = false;
                            accountId = CharacterDB::GetInstance().AuthenticateUser(username, password, g_Config.dbAccount, accountExists, passMatch);
                            
                            if (!accountExists) {
                                bLoginResult = 1; // LOGINFAIL (Account doesn't exist)
                            } else if (!passMatch) {
                                bLoginResult = 5; // WRONGPASSWORD
                                LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] Debug: Password mismatch for user: " + username);
                            } else {
                                bLoginResult = 0; // SUCCESS
                            }
                        }
                        
                        std::vector<BYTE> mergedBuf;
                        
                        std::vector<BYTE> ackBuf; ackBuf.resize(4); 
                        ackBuf.push_back(bLoginResult); // bResult
                        
                        DWORD dwKey = 46;
                        if (bLoginResult == 0) {
                            AuthTicket ticket = AuthCenter::Get().Issue(username, accountId, 0xFF, clientIp);
                            dwKey = ticket.ticketId;
                        }

                        ackBuf.push_back((BYTE)(dwKey & 0xFF)); ackBuf.push_back((BYTE)((dwKey >> 8) & 0xFF)); ackBuf.push_back((BYTE)((dwKey >> 16) & 0xFF)); ackBuf.push_back((BYTE)(dwKey >> 24));
                        ackBuf.push_back(18); // bAge = 18 (Bypass age restriction)
                        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
                        ackHead->id = CS_IT_LOGIN_ACK; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
                        EncryptPacket(ackBuf.data(), 0);
                        
                        mergedBuf.insert(mergedBuf.end(), ackBuf.begin(), ackBuf.end());

                        if (bLoginResult == 0) {
                            std::vector<BYTE> wBuf; wBuf.resize(4);
                            auto pushStr = [&](const std::string& s) { WORD len = (WORD)(s.length() + 1); wBuf.push_back((BYTE)(len & 0xFF)); wBuf.push_back((BYTE)(len >> 8)); for (char c : s) wBuf.push_back((BYTE)c); wBuf.push_back(0); };
                            auto pushByte = [&](BYTE b) { wBuf.push_back(b); };
                            auto pushWord = [&](WORD w) { wBuf.push_back((BYTE)(w & 0xFF)); wBuf.push_back((BYTE)(w >> 8)); };
                            auto pushDWord = [&](DWORD d) { wBuf.push_back((BYTE)(d & 0xFF)); wBuf.push_back((BYTE)((d >> 8) & 0xFF)); wBuf.push_back((BYTE)((d >> 16) & 0xFF)); wBuf.push_back((BYTE)(d >> 24)); };
                            
                            pushByte(1); // bWorldID
                            pushStr(g_Config.realmName); // szWorldName
                            pushStr("Xiah Server"); // szWorldDes
                            pushByte((BYTE)g_Config.cachedChannels.size()); // bCount

                            for (const auto& c : g_Config.cachedChannels) {
                                pushByte((BYTE)c.id); // bChannelID
                                pushStr(c.name); // szChannelName
                                pushStr(c.desc); // szChannelDes
                                pushByte((BYTE)c.age); // bAge (Age Requirement)
                                pushWord((WORD)c.capacity); // wMaxUser
                                pushStr(g_Config.externalIP); // szUnitAddress
                                pushDWord(g_Config.channelPorts.at(c.id)); // dwUnitPort
                            }
                            
                            PACKET_HEADER* wHead = (PACKET_HEADER*)wBuf.data();
                            wHead->id = CS_IT_WORLDLIST_ACK;
                            wHead->payloadSize = (WORD)(wBuf.size() - sizeof(PACKET_HEADER));
                            EncryptPacket(wBuf.data(), 0);

                            mergedBuf.insert(mergedBuf.end(), wBuf.begin(), wBuf.end());
                        }

                        SafeSend(clientSocket, (const char*)mergedBuf.data(), mergedBuf.size(), 0);
                        std::string resStr = "FAIL (Unknown)";
                        if (bLoginResult == 0) resStr = "SUCCESS";
                        else if (bLoginResult == 1) resStr = "FAIL (Account does not exist)";
                        else if (bLoginResult == 5) resStr = "FAIL (Wrong Password)";
                        LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] Sent Login Result: " + resStr);
                    } else {
                        char dHex[128];
                        sprintf(dHex, "UNKNOWN PACKET! ID: 0x%04X, Size: %d", header->id, wPayloadSize);
                        LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] " + std::string(dHex));
                    }
                }
            } catch (...) {}
            closesocket(clientSocket);
            LOG("[AuthSvr " + std::to_string(g_Config.authPort) + "] Client disconnected.");
        }).detach();
    }
}

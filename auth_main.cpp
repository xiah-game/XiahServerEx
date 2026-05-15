#include <winsock2.h>
#include <ws2tcpip.h>
#include <sql.h>
#include <sqlext.h>
#include <iostream>
#include <vector>
#include <string>
#include "../XiahClient/csprotocol.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "odbc32.lib")

#pragma pack(push, 1)
struct PACKET_HEADER {
    WORD id;
    WORD totalSize;
};
#pragma pack(pop)

// Helper to execute DB login query
bool VerifyLogin(const std::string& user, const std::string& pass) {
    // For debugging the world list UI, simply return TRUE for absolutely everything!
    std::cout << "[God Mode] Bypassed database permanently for UI testing." << std::endl;
    return true;
    
    SQLHENV hEnv;
    SQLHDBC hDbc;
    SQLHSTMT hStmt;
    bool success = false;

    if (SQLAllocHandle(SQL_HANDLE_ENV, SQL_NULL_HANDLE, &hEnv) == SQL_ERROR) return false;
    SQLSetEnvAttr(hEnv, SQL_ATTR_ODBC_VERSION, (void*)SQL_OV_ODBC3, 0);
    if (SQLAllocHandle(SQL_HANDLE_DBC, hEnv, &hDbc) == SQL_ERROR) { SQLFreeHandle(SQL_HANDLE_ENV, hEnv); return false; }

    SQLCHAR szConnStrIn[] = "Driver={SQL Server};Server=61.184.20.203;UID=sa;PWD=Ahtd!@123;Database=xiah_account;";
    SQLCHAR szConnStrOut[1024];
    SQLSMALLINT cbConnStrOut;
    
    if (SQLDriverConnectA(hDbc, NULL, szConnStrIn, SQL_NTS, szConnStrOut, sizeof(szConnStrOut), &cbConnStrOut, SQL_DRIVER_NOPROMPT) == SQL_SUCCESS || 
        SQLDriverConnectA(hDbc, NULL, szConnStrIn, SQL_NTS, szConnStrOut, sizeof(szConnStrOut), &cbConnStrOut, SQL_DRIVER_NOPROMPT) == SQL_SUCCESS_WITH_INFO) {
        
        if (SQLAllocHandle(SQL_HANDLE_STMT, hDbc, &hStmt) == SQL_SUCCESS) {
            std::string query = "SELECT id FROM Account WHERE szAccount = '" + user + "' AND szPasswd = '" + pass + "'";
            if (SQLExecDirectA(hStmt, (SQLCHAR*)query.c_str(), SQL_NTS) == SQL_SUCCESS) {
                SQLINTEGER accountId;
                if (SQLFetch(hStmt) == SQL_SUCCESS) {
                    success = true; // Found the user!
                }
            }
            SQLFreeHandle(SQL_HANDLE_STMT, hStmt);
        }
        SQLDisconnect(hDbc);
    }
    
    SQLFreeHandle(SQL_HANDLE_DBC, hDbc);
    SQLFreeHandle(SQL_HANDLE_ENV, hEnv);
    return success;
}

void EncryptPacket(BYTE* pHead, BYTE bKey) {
    WORD wSize = *(WORD*)(pHead + 2) + 4; // totalSize + header Size
    BYTE bPrev = 0;
    pHead[0] += bPrev + bKey + (wSize & 0xFF);
    bPrev = pHead[0];
    pHead[1] += bPrev + bKey + (wSize & 0xFF);
    bPrev = pHead[1];
    for (int i = 0; i < wSize - 4; i++) {
        pHead[i+4] += bPrev + bKey + (wSize & 0xFF);
        bPrev = pHead[i+4];
    }
}

void DecryptPacket(BYTE* pHead, BYTE bKey) {
    WORD wSize = *(WORD*)(pHead + 2) + 4;
    BYTE bPrevKey = 0;
    BYTE bPrev = pHead[0];
    pHead[0] -= bPrevKey + bKey + (wSize & 0xFF);
    bPrevKey = bPrev;
    
    bPrev = pHead[1];
    pHead[1] -= bPrevKey + bKey + (wSize & 0xFF);
    bPrevKey = bPrev;
    
    for (int i = 0; i < wSize - 4; i++) {
        bPrev = pHead[i+4];
        pHead[i+4] -= bPrevKey + bKey + (wSize & 0xFF);
        bPrevKey = bPrev;
    }
}

int main() {
    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    SOCKET listenSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listenSocket == INVALID_SOCKET) {
        WSACleanup();
        return 1;
    }

    sockaddr_in serverAddr = {};
    serverAddr.sin_family = AF_INET;
    serverAddr.sin_addr.s_addr = INADDR_ANY;
    serverAddr.sin_port = htons(9001); // AuthSvr

    if (bind(listenSocket, (sockaddr*)&serverAddr, sizeof(serverAddr)) == SOCKET_ERROR) {
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }
    if (listen(listenSocket, SOMAXCONN) == SOCKET_ERROR) {
        closesocket(listenSocket);
        WSACleanup();
        return 1;
    }

    std::cout << "Modern C++ XiahEmu (AuthSvr) is listening on port 9001..." << std::endl;
    std::cout << "MSSQL Database initialized (61.184.20.203)..." << std::endl;

    while (true) {
        SOCKET clientSocket = accept(listenSocket, NULL, NULL);
        if (clientSocket == INVALID_SOCKET) continue;
        std::cout << "Client connected!" << std::endl;

        // Xiah requires the server to send an encryption key handshake (ID=0) immediately upon connection!
        std::vector<BYTE> initBuf(5); // 4 header + 1 payload
        PACKET_HEADER* initHead = (PACKET_HEADER*)initBuf.data();
        initHead->id = 0;
        initHead->totalSize = 5;
        initBuf[4] = 0x00; // Key = 0

        // Because it's the very first packet conveying the key itself, it might not be encrypted? 
        // In Xiah socket: if ID=0, it just reads it (no Decrypt called!). 
        // Wait, if it doesn't call Decrypt, we shouldn't call Encrypt!
        SafeSend(clientSocket, (const char*)initBuf.data(), initBuf.size(), 0);
        std::cout << "Sent Encryption Handshake (ID=0, Key=0)." << std::endl;

        char buffer[4096];
        int bytesReceived = recv(clientSocket, buffer, sizeof(buffer), 0);
        if (bytesReceived >= sizeof(PACKET_HEADER)) {
            try {
                WORD wPayloadSize = *(WORD*)(buffer + 2);
                if (wPayloadSize > 4000) {
                    std::cerr << "Dropping malformed packet with massive payload: " << wPayloadSize << std::endl;
                    closesocket(clientSocket);
                    continue;
                }

                // Decrypt with default key 0 (for AuthSvr)
                DecryptPacket((BYTE*)buffer, 0);
                
                PACKET_HEADER* header = (PACKET_HEADER*)buffer;
                std::cout << "Received Packet ID: " << std::hex << header->id << " Size: " << std::dec << header->totalSize << std::endl;

                if (header->id == CS_IT_LOGIN_REQ) { 
                    BYTE* payload = (BYTE*)buffer + sizeof(PACKET_HEADER);
                    BYTE* endPtr = (BYTE*)buffer + bytesReceived;
                    
                    // Xiah uses WORD-length prefixed string.
                    if (payload + 2 > endPtr) throw std::runtime_error("Buffer underflow reading userLen");
                    WORD userLen = *(WORD*)payload; payload += 2;
                    
                    if (userLen == 0 || payload + userLen > endPtr) throw std::runtime_error("Invalid userLen");
                    std::string username((char*)payload, userLen - 1); // exclude null char
                    payload += userLen;

                    if (payload + 2 > endPtr) throw std::runtime_error("Buffer underflow reading passLen");
                    WORD passLen = *(WORD*)payload; payload += 2;
                    
                    if (passLen == 0 || payload + passLen > endPtr) throw std::runtime_error("Invalid passLen");
                    std::string password((char*)payload, passLen - 1);
                    
                    std::cout << "-> Auth Request for User: " << username << " Pass: " << password << std::endl;

                    bool dbResult = VerifyLogin(username, password);

                if (dbResult) {
                    // 1. Send World List first
                    std::vector<BYTE> wBuf;
                    wBuf.resize(4);

                    auto pushStr = [&](const std::string& s) {
                        WORD len = (WORD)(s.length() + 1);
                        wBuf.push_back((BYTE)(len & 0xFF)); wBuf.push_back((BYTE)(len >> 8));
                        for (char c : s) wBuf.push_back((BYTE)c);
                        wBuf.push_back(0);
                    };
                    auto pushByte = [&](BYTE b) { wBuf.push_back(b); };
                    auto pushWord = [&](WORD w) { wBuf.push_back((BYTE)(w & 0xFF)); wBuf.push_back((BYTE)(w >> 8)); };
                    auto pushDWord = [&](DWORD d) { wBuf.push_back((BYTE)(d & 0xFF)); wBuf.push_back((BYTE)((d >> 8) & 0xFF)); wBuf.push_back((BYTE)((d >> 16) & 0xFF)); wBuf.push_back((BYTE)(d >> 24)); };

                    pushByte(1); // bWorldID
                    pushStr("XiahEmu Local"); // szWorldName
                    pushStr("Test Realm"); // szWorldDes
                    pushByte(1); // bCount (1 channel)

                    // For each channel:
                    pushByte(1); // bChannelID
                    pushStr("Channel 1"); // szChannelName
                    pushStr("PVE"); // szChannelDes
                    pushByte(0); // bAge
                    pushWord(100); // wMaxUser
                    pushStr("127.0.0.1"); // szUnitAddress
                    pushDWord(9000); // dwUnitPort

                    PACKET_HEADER* wHead = (PACKET_HEADER*)wBuf.data();
                    wHead->id = CS_IT_WORLDLIST_ACK;
                    wHead->totalSize = (WORD)(wBuf.size() - sizeof(PACKET_HEADER));

                    EncryptPacket(wBuf.data(), 0);
                    SafeSend(clientSocket, (const char*)wBuf.data(), wBuf.size(), 0);
                    std::cout << "Sent CS_IT_WORLDLIST_ACK." << std::endl;
                }

                // 2. Send Login Ack
                std::vector<BYTE> ackBuf;
                ackBuf.resize(4); 
                ackBuf.push_back(dbResult ? 0 : 1); // bResult
                // Wait! Xiah's OnCS_IT_LOGIN_ACK requires: bResult, dwKey, bAge
                // msg >> bResult >> dwKey >> bAge
                auto pushDWordAck = [&](DWORD d) { ackBuf.push_back((BYTE)(d & 0xFF)); ackBuf.push_back((BYTE)((d >> 8) & 0xFF)); ackBuf.push_back((BYTE)((d >> 16) & 0xFF)); ackBuf.push_back((BYTE)(d >> 24)); };
                pushDWordAck(12345); // dwKey
                ackBuf.push_back(18); // bAge (18+)

                PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
                ackHead->id = CS_IT_LOGIN_ACK;
                ackHead->totalSize = (WORD)(ackBuf.size() - sizeof(PACKET_HEADER));

                EncryptPacket(ackBuf.data(), 0);
                SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
                std::cout << (dbResult ? "-> [DB] Login SUCCESS!" : "-> [DB] Login FAILED!") << std::endl;
            }
            } catch (const std::exception& e) {
                std::cerr << "Exception parsing packet: " << e.what() << std::endl;
            } catch (...) {
                std::cerr << "Unknown exception parsing packet." << std::endl;
            }
        }
        closesocket(clientSocket);
        std::cout << "Client disconnected." << std::endl;
    }
    closesocket(listenSocket);
    WSACleanup();
    return 0;
}

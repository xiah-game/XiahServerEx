#include "LoginHandler.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/PlayerManager.h"
#include "SlotHandler.h"
#include "../GameObjects/PlayerManager.h"

void OnLoginCheckReq(SOCKET clientSocket, std::string& clientAccountName, BYTE* payload, WORD totalSize) {
    LOG("[LoginHandler] Received CS_IT_LOGINCHECK_REQ!");
    WORD nameLen = *(WORD*)(payload);
    if (nameLen > 1 && nameLen <= 256) {
        clientAccountName = std::string((char*)payload + 2, nameLen - 1);
        LOG("[LoginHandler] Login Account: " + clientAccountName);
        SessionMgr::GetInstance().SetAccount(clientSocket, clientAccountName);
    }

    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); 
    ackBuf.push_back(1); 
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    ackHead->id = CS_IT_LOGINCHECK_ACK; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[LoginHandler] Sent CS_IT_LOGINCHECK_ACK!");
}

void OnCharacterListReq(SOCKET clientSocket, const std::string& clientAccountName, BYTE* payload, WORD totalSize) {
    LOG("[LoginHandler] Received CS_IT_CHARACTERLIST_REQ!");
    std::vector<BYTE> charBuf; charBuf.resize(4); 

    auto pushStr = [&](const std::string& s) { WORD len = (WORD)(s.length() + 1); charBuf.push_back((BYTE)(len & 0xFF)); charBuf.push_back((BYTE)(len >> 8)); for (char c : s) charBuf.push_back((BYTE)c); charBuf.push_back(0); };
    auto pushByte = [&](BYTE b) { charBuf.push_back(b); };
    auto pushWord = [&](WORD w) { charBuf.push_back((BYTE)(w & 0xFF)); charBuf.push_back((BYTE)(w >> 8)); };
    auto pushDWord = [&](DWORD d) { charBuf.push_back((BYTE)(d & 0xFF)); charBuf.push_back((BYTE)((d >> 8) & 0xFF)); charBuf.push_back((BYTE)((d >> 16) & 0xFF)); charBuf.push_back((BYTE)(d >> 24)); };

    std::string q = "SELECT TOP 3 dwCharID, szNickName, bCharType, dwBirthDate, dwMapID, wLevel, dwHpCur, dwHpMax, wIpCur, wIpMax, wVit, wStr, wSus, wDex, bRebirth FROM CHAR_VISUAL WHERE szAccount = '" + clientAccountName + "' ORDER BY dwCharID ASC";
    struct CItem { WORD wVis=0; BYTE bRar=0; BYTE bStx=0; };
    struct CData { 
        int dwCharID; char szNickName[256]; char bCharType; int dwBirthDate; int dwMapID; short wLevel; 
        int dwHpCur; int dwHpMax; short wIpCur; short wIpMax; short wVit; short wStr; short wSus; short wDex; char bRebirth;
        CItem items[9];
    };
    std::vector<CData> chars;

    auto listCallback = [&](SQLHSTMT hStmt) {
        CData d; SQLLEN cb[15];
        SQLGetData(hStmt, 1, SQL_C_SLONG, &d.dwCharID, 0, &cb[0]);
        SQLGetData(hStmt, 2, SQL_C_CHAR, d.szNickName, sizeof(d.szNickName), &cb[1]);
        SQLGetData(hStmt, 3, SQL_C_STINYINT, &d.bCharType, 0, &cb[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &d.dwBirthDate, 0, &cb[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &d.dwMapID, 0, &cb[4]);
        SQLGetData(hStmt, 6, SQL_C_SSHORT, &d.wLevel, 0, &cb[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &d.dwHpCur, 0, &cb[6]);
        SQLGetData(hStmt, 8, SQL_C_SLONG, &d.dwHpMax, 0, &cb[7]);
        SQLGetData(hStmt, 9, SQL_C_SSHORT, &d.wIpCur, 0, &cb[8]);
        SQLGetData(hStmt, 10, SQL_C_SSHORT, &d.wIpMax, 0, &cb[9]);
        SQLGetData(hStmt, 11, SQL_C_SSHORT, &d.wVit, 0, &cb[10]);
        SQLGetData(hStmt, 12, SQL_C_SSHORT, &d.wStr, 0, &cb[11]);
        SQLGetData(hStmt, 13, SQL_C_SSHORT, &d.wSus, 0, &cb[12]);
        SQLGetData(hStmt, 14, SQL_C_SSHORT, &d.wDex, 0, &cb[13]);
        SQLGetData(hStmt, 15, SQL_C_STINYINT, &d.bRebirth, 0, &cb[14]);
        chars.push_back(d);
    };

    if (DBHelper::GetInstance().ExecuteQuery(q, listCallback)) {
        for (auto& ch : chars) {
            std::string qe = "SELECT bSackPos, wVisualID FROM vCHAR_EQUIPITEM WHERE szAccount = '" + clientAccountName + "' AND dwCharID = " + std::to_string(ch.dwCharID);
            auto equipCallback = [&](SQLHSTMT hStmt) {
                char pos = 0; short vis = 0; SQLLEN c1, c2;
                SQLGetData(hStmt, 1, SQL_C_STINYINT, &pos, 0, &c1);
                if (c1 == SQL_NULL_DATA) pos = 0;
                SQLGetData(hStmt, 2, SQL_C_SSHORT, &vis, 0, &c2);
                if (c2 == SQL_NULL_DATA) vis = 0;
                if (pos >= 0 && pos < 9) ch.items[pos].wVis = vis;
            };
            DBHelper::GetInstance().ExecuteQuery(qe, equipCallback);

            std::string qd = "SELECT bSackPos, bStxType, bRarity FROM vCHAR_EQUIPITEMDATA WHERE szAccount = '" + clientAccountName + "' AND dwCharID = " + std::to_string(ch.dwCharID);
            auto dataCallback = [&](SQLHSTMT hStmt) {
                char pos = 0, stx = 0, rar = 0; SQLLEN c1, c2, c3;
                SQLGetData(hStmt, 1, SQL_C_STINYINT, &pos, 0, &c1);
                SQLGetData(hStmt, 2, SQL_C_STINYINT, &stx, 0, &c2);
                if (c2 == SQL_NULL_DATA) stx = 0;
                SQLGetData(hStmt, 3, SQL_C_STINYINT, &rar, 0, &c3);
                if (c3 == SQL_NULL_DATA) rar = 0;
                if (pos >= 0 && pos < 9) { ch.items[pos].bStx = stx; ch.items[pos].bRar = rar; }
            };
            DBHelper::GetInstance().ExecuteQuery(qd, dataCallback);
        }
        
        LOG("[LoginHandler] DB Character Count: " + std::to_string(chars.size()));
        charBuf.push_back((BYTE)chars.size()); // bCount
        for (auto& ch : chars) {
            pushDWord(ch.dwMapID); 
            pushDWord(400000000 + ch.dwCharID); // ObjectID
            pushStr(std::string(ch.szNickName)); 
            pushByte(ch.bCharType); 
            pushWord(ch.wLevel); 
            pushDWord(ch.dwHpCur); 
            pushDWord(ch.dwHpMax); 
            pushWord(ch.wIpCur); 
            pushWord(ch.wIpMax); 
            pushWord(ch.wVit); 
            pushWord(ch.wStr); 
            pushWord(ch.wSus); 
            pushWord(ch.wDex); 
            pushDWord(ch.dwBirthDate); 
            pushStr(""); // MunpaName
            pushStr(""); // OrderName

            for (int i = 0; i < 9; i++) {
                pushWord(ch.items[i].wVis); 
                pushByte(ch.items[i].bRar); 
                pushByte(ch.items[i].bStx); 
            }
            pushStr(""); // szMapName
            pushByte(1); // m_bAuction (Must be 1, otherwise client Start Game button ignores clicks)
            pushByte(ch.bRebirth); // Rebirth
        }
    } else {
        LOG("[LoginHandler] DB Fetch Failed! Pushed 0 chars.");
        charBuf.push_back(0); 
    }

    PACKET_HEADER* charHead = (PACKET_HEADER*)charBuf.data();
    charHead->id = CS_IT_CHARACTERLIST_ACK; 
    charHead->payloadSize = (WORD)(charBuf.size() - sizeof(PACKET_HEADER));
    EncryptPacket(charBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)charBuf.data(), charBuf.size(), 0);
    LOG("[LoginHandler] Pushed DB Character List successfully!");
}

void OnStartGameReq(SOCKET clientSocket, std::string& clientAccountName, BYTE* payload, WORD totalSize) {
    DWORD dwObjectID = *(DWORD*)(payload);
    DWORD dwCharID = dwObjectID - 400000000;
    LOG("[LoginHandler] Received CS_NV_STARTGAME_REQ! dwCharID: " + std::to_string(dwCharID));

    SessionMgr::GetInstance().SetCharID(clientSocket, dwCharID);

    DWORD dwMapID = 6;
    std::string q = "SELECT dwMapID FROM CHAR_STATUS WHERE dwCharID = " + std::to_string(dwCharID);
    auto mapCallback = [&](SQLHSTMT hStmt) {
        int map; SQLLEN cbMap;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &map, 0, &cbMap);
        if (cbMap != SQL_NULL_DATA) dwMapID = map;
        LOG("[LoginHandler] Found real Map ID: " + std::to_string(dwMapID) + " for Character: " + std::to_string(dwCharID));
    };
    DBHelper::GetInstance().ExecuteQuery(q, mapCallback);

    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); // Success
    ackBuf.push_back((BYTE)(dwMapID & 0xFF)); ackBuf.push_back((BYTE)((dwMapID >> 8) & 0xFF)); ackBuf.push_back((BYTE)((dwMapID >> 16) & 0xFF)); ackBuf.push_back((BYTE)(dwMapID >> 24));
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    ackHead->id = CS_NV_STARTGAME_ACK; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[LoginHandler] Sent CS_NV_STARTGAME_ACK!");

    UpdatePlayerStatsAndSend(clientSocket, dwCharID);
}

void OnEndGameReq(SOCKET clientSocket, DWORD dwCharID, BYTE* payload, WORD totalSize) {
    LOG("[LoginHandler] Received CS_NV_ENDGAME_REQ for dwCharID: " + std::to_string(dwCharID));

    // Save player data
    PlayerManager::GetInstance().SavePlayer(dwCharID);

    // Send ACK back to client
    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); // Success
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    ackHead->id = CS_NV_ENDGAME_ACK; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[LoginHandler] Sent CS_NV_ENDGAME_ACK!");
}


#include "LoginHandler.h"
#include "../DB/CharacterDB.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/PlayerManager.h"
#include "SlotHandler.h"
#include "../Network/AuthCenter.h"
#include "../ServerCore.h"
#include "../GameObjects/MapInstance.h"
#include "TradeHandler.h"

void OnLoginCheckReq(SOCKET clientSocket, std::string& clientAccountName, BYTE* payload, WORD totalSize) {
    LOG("[LoginHandler] Received CS_IT_LOGINCHECK_REQ!");
    
    WORD nameLen = *(WORD*)(payload);
    if (nameLen < 1 || nameLen > 256 || 2 + nameLen + 4 + 1 + 2 > totalSize) {
        LOG("[LoginHandler] Bad packet length!");
        closesocket(clientSocket);
        return;
    }

    // Extract dwKey (ticket ID) and byChannelID from client request payload
    DWORD dwKey = *(DWORD*)(payload + 2 + nameLen);
    BYTE byChannelID = *(BYTE*)(payload + 2 + nameLen + 4);
    WORD wProtocolVersion = *(WORD*)(payload + 2 + nameLen + 4 + 1);

    // Retrieve client IP
    sockaddr_in peerAddr;
    int addrLen = sizeof(peerAddr);
    std::string peerIp = "127.0.0.1";
    if (getpeername(clientSocket, (sockaddr*)&peerAddr, &addrLen) == 0) {
        peerIp = inet_ntoa(peerAddr.sin_addr);
    }

    LOG("[LoginHandler] Ticket verification: Account payload='" + std::string((char*)payload + 2, nameLen - 1) + 
        "' Ticket=" + std::to_string(dwKey) + " Channel=" + std::to_string(byChannelID) + 
        " ProtocolVer=" + std::to_string(wProtocolVersion) + " IP=" + peerIp);

    AuthTicket ticket;
    if (!AuthCenter::Get().Consume(dwKey, peerIp, byChannelID, ticket)) {
        LOG("[LoginHandler] TICKET VERIFICATION FAILED! Key: " + std::to_string(dwKey) + " -> Rejecting client");
        
        // Respond with login failure ACK
        std::vector<BYTE> ackBuf; ackBuf.resize(4); 
        ackBuf.push_back(1); // 1 = LOGINFAIL
        ackBuf.push_back(0); // Underage
        PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
        ackHead->id = CS_IT_LOGINCHECK_ACK; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
        return;
    }

    // Overwrite the account name with the one secured by AuthTicket to prevent fake account spoofing!
    clientAccountName = ticket.account;
    LOG("[LoginHandler] Ticket verified successfully! Authenticated Account: " + clientAccountName);

    // Detect and kick duplicate sessions to completely prevent WPE replay exploits and handle dirty reconnects gracefully
    SOCKET oldSocket = SessionMgr::GetInstance().GetSocketByAccount(clientAccountName);
    if (oldSocket != INVALID_SOCKET && oldSocket != clientSocket) {
        LOG("[LoginHandler] WARNING: Account '" + clientAccountName + "' is already online! Kicking the older connection.");
        closesocket(oldSocket);
    }

    SessionMgr::GetInstance().SetAccount(clientSocket, clientAccountName);
    SessionMgr::GetInstance().SetTicketId(clientSocket, ticket.ticketId);

    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); // 0 = SUCCESS
    ackBuf.push_back(1); // 1 = Adult
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

    std::vector<CharacterDB::CharSelectEntry> chars;

    if (CharacterDB::GetInstance().GetCharSelectList(clientAccountName, chars)) {
        
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
            pushDWord(ch.wIpCur); 
            pushDWord(ch.wIpMax); 
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
    LOG("[LoginHandler] Received CS_NV_STARTGAME_REQ! dwCharID: " + std::to_string(dwCharID) + " Account: " + clientAccountName);

    // Verify character ownership
    if (!CharacterDB::GetInstance().AccountOwnsCharacter(dwCharID, clientAccountName)) {
        LOG("[LoginHandler] SECURITY ALERT: Account '" + clientAccountName + "' tried to load unauthorized character ID " + std::to_string(dwCharID) + "! Kicking socket.");
        closesocket(clientSocket);
        return;
    }

    SessionMgr::GetInstance().SetCharID(clientSocket, dwCharID);

    DWORD dwMapID = CharacterDB::GetInstance().GetCharMapID(dwCharID);
    LOG("[LoginHandler] Found real Map ID: " + std::to_string(dwMapID) + " for Character: " + std::to_string(dwCharID));

    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); // Success
    ackBuf.push_back((BYTE)(dwMapID & 0xFF)); ackBuf.push_back((BYTE)((dwMapID >> 8) & 0xFF)); ackBuf.push_back((BYTE)((dwMapID >> 16) & 0xFF)); ackBuf.push_back((BYTE)(dwMapID >> 24));
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    ackHead->id = CS_NV_STARTGAME_ACK; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[LoginHandler] Sent CS_NV_STARTGAME_ACK!");

    // During StartGame login handshake, send 0x4414 (CS_IT_CHARSTATUSINFO_ACK) which safely initializes intro frames without requiring g_pMainChar
    SendCharStatusInfoAck(clientSocket, dwCharID, 0x4414);
}

void OnEndGameReq(SOCKET clientSocket, DWORD dwCharID, BYTE* payload, WORD totalSize) {
    LOG("[LoginHandler] Received CS_NV_ENDGAME_REQ for dwCharID: " + std::to_string(dwCharID));

    // Save player data
    PlayerManager::GetInstance().SavePlayer(dwCharID);

    // 清理玩家地图及相关内存状态，防止切换角色时旧角色残留在游戏世界中
    DWORD mapID = SessionMgr::GetInstance().GetMapID(clientSocket);
    if (dwCharID > 0 && g_MapInstances.count(mapID)) {
        CMapInstance* pMap = g_MapInstances[mapID];
        DWORD dwObjectID = dwCharID + 400000000;
        
        // 1. 向该玩家视野内（AOI）的所有其他玩家广播玩家离开，避免显示残影
        WORD wPosX = 0, wPosY = 0;
        {
            std::lock_guard<std::mutex> lockP(pMap->GetMutex());
            sServerObject* pObj = pMap->GetPlayer(dwObjectID);
            if (pObj) {
                wPosX = pObj->wPosX;
                wPosY = pObj->wPosY;
            }
        }
        
        if (wPosX > 0 && wPosY > 0) {
            std::vector<BYTE> leaveBuf;
            leaveBuf.resize(4);
            leaveBuf.push_back(0); // bResult = 0 (success)
            leaveBuf.push_back(dwObjectID & 0xFF);
            leaveBuf.push_back((dwObjectID >> 8) & 0xFF);
            leaveBuf.push_back((dwObjectID >> 16) & 0xFF);
            leaveBuf.push_back(dwObjectID >> 24);
            leaveBuf.push_back(1); // bObjectType = 1 (PC)
            leaveBuf.push_back(mapID & 0xFF);
            leaveBuf.push_back((mapID >> 8) & 0xFF);
            leaveBuf.push_back((mapID >> 16) & 0xFF);
            leaveBuf.push_back(mapID >> 24);
            leaveBuf.push_back(0); // bType = 0 (normal leave)
            
            PACKET_HEADER* leaveHead = (PACKET_HEADER*)leaveBuf.data();
            leaveHead->id = 0x3506; // CS_NV_MAPLEAVE_ACK / MAPLEAVE
            leaveHead->payloadSize = leaveBuf.size() - sizeof(PACKET_HEADER);
            EncryptPacket(leaveBuf.data(), 0x42);
            
            pMap->BroadcastPacketAOI(wPosX, wPosY, leaveBuf);
        }

        // 2. 从地图实例中彻底移除玩家实体
        {
            std::lock_guard<std::mutex> lockP(pMap->GetMutex());
            pMap->RemovePlayer(dwObjectID);
        }
    }

    // 3. 清理交易模块中该玩家的离线/小退状态
    if (dwCharID > 0) {
        TradeManager::GetInstance().OnPlayerDisconnect(dwCharID);
    }

    // 4. 重置该客户端 Session 关联的角色 ID 和地图 ID 状态，防止同一个 Socket 后续重新登录其他角色发生数据混淆
    SessionMgr::GetInstance().SetCharID(clientSocket, 0);
    SessionMgr::GetInstance().SetMapID(clientSocket, 0);

    BYTE bEnd = 1;
    BYTE bChChange = 0;
    if (totalSize >= 2) {
        bEnd = payload[0];
        bChChange = payload[1];
    } else if (totalSize >= 1) {
        bEnd = payload[0];
    }

    LOG("[LoginHandler] EndGame payload details: bEnd=" + std::to_string(bEnd) + " bChChange=" + std::to_string(bChChange));

    // Send ACK back to client
    std::vector<BYTE> ackBuf; ackBuf.resize(4); 
    ackBuf.push_back(0); // Success (bStartOption)
    ackBuf.push_back(bChChange); // Echo bChChange to client so it knows whether to exit or switch channel
    
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    ackHead->id = CS_NV_ENDGAME_ACK; ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    LOG("[LoginHandler] Sent CS_NV_ENDGAME_ACK! bChChange=" + std::to_string(bChChange));
}


#include "MunpaHandler.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../DB/CharacterDB.h"
#include "../GameObjects/PlayerManager.h"
#include "../GameObjects/MapInstance.h"
#include "../ServerCore.h"
#include "../DBHelper.h"
#include <iostream>
#include <vector>
#include <string>
#include <mutex>
#include <time.h>

// Helpers for reading from raw payload safely
template<typename T>
T ReadVal(BYTE*& p, BYTE* end) {
    if (p + sizeof(T) > end) return 0;
    T val = *(T*)p;
    p += sizeof(T);
    return val;
}

std::string ReadString(BYTE*& p, BYTE* end) {
    if (p + 2 > end) return "";
    WORD len = *(WORD*)p;
    p += 2;
    if (p + len > end) return "";
    std::string s((char*)p, len);
    p += len;
    return s;
}

// Helpers for serializing packet fields
void pushDWord(std::vector<BYTE>& buf, DWORD d) {
    buf.push_back((BYTE)(d & 0xFF));
    buf.push_back((BYTE)((d >> 8) & 0xFF));
    buf.push_back((BYTE)((d >> 16) & 0xFF));
    buf.push_back((BYTE)((d >> 24) & 0xFF));
}

void pushWord(std::vector<BYTE>& buf, WORD w) {
    buf.push_back((BYTE)(w & 0xFF));
    buf.push_back((BYTE)((w >> 8) & 0xFF));
}

void pushString(std::vector<BYTE>& buf, const std::string& str) {
    WORD len = (WORD)str.length();
    pushWord(buf, len);
    for (char c : str) buf.push_back((BYTE)c);
}

// Generic packet sender
void SendPacketToSocket(SOCKET s, WORD opCode, const std::vector<BYTE>& payload) {
    if (s == INVALID_SOCKET) return;
    std::vector<BYTE> ackBuf;
    ackBuf.resize(4); // header placeholder
    ackBuf.insert(ackBuf.end(), payload.begin(), payload.end());
    PACKET_HEADER* ackHead = (PACKET_HEADER*)ackBuf.data();
    ackHead->id = opCode;
    ackHead->payloadSize = ackBuf.size() - sizeof(PACKET_HEADER);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
}

// Broadcast to all online members of a guild
void BroadcastToMunpa(DWORD dwMunpaID, WORD opCode, const std::vector<BYTE>& payload) {
    if (dwMunpaID == 0) return;
    std::vector<DWORD> members;
    std::string q = "SELECT dwCharID FROM CHAR_BASIC WHERE dwMunpaID = " + std::to_string(dwMunpaID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        DWORD dwCharID = 0; SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_ULONG, &dwCharID, 0, &c);
        if (dwCharID > 0) members.push_back(dwCharID);
    });
    for (DWORD dwCharID : members) {
        SOCKET s = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
        if (s != INVALID_SOCKET) {
            SendPacketToSocket(s, opCode, payload);
        }
    }
}

// Update player's in-memory cached guild details on map
void UpdatePlayerInMemoryGuild(DWORD dwCharID, DWORD dwMunpaID, DWORD dwMunpaOrder, const std::string& szMunpaName, const std::string& szMunpaNickName, DWORD dwMarkID) {
    SOCKET s = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
    if (s == INVALID_SOCKET) return;
    DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
    if (g_MapInstances.count(mapID)) {
        CMapInstance* pMap = g_MapInstances[mapID];
        std::lock_guard<std::mutex> lock(pMap->GetMutex());
        PlayerData* pObj = pMap->GetPlayer(dwCharID + 400000000);
        if (pObj) {
            pObj->dwMunpaID = dwMunpaID;
            pObj->dwMunpaOrder = dwMunpaOrder;
            pObj->szMunpaName = szMunpaName;
            pObj->szMunpaNickName = szMunpaNickName;
            pObj->dwMunpaMarkID = dwMarkID;
            LOG("[MunpaHandler] Updated online player caching: ID=" + std::to_string(dwCharID) + " MunpaID=" + std::to_string(dwMunpaID));
        }
    }
}

// Broadcast player visual update (CS_IT_CHARINFOLIST_ACK) to their map AOI
void BroadcastPlayerVisualUpdate(DWORD dwCharID) {
    SOCKET s = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
    if (s == INVALID_SOCKET) return;
    DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
    DWORD dwObjectID = dwCharID + 400000000;
    
    if (g_MapInstances.count(mapID)) {
        CMapInstance* mapInst = g_MapInstances[mapID];
        PlayerData objCopy;
        bool found = false;
        {
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            PlayerData* pObj = mapInst->GetPlayer(dwObjectID);
            if (pObj) { objCopy = *pObj; found = true; }
        }
        if (!found) return;

        std::vector<BYTE> ackBuf; ackBuf.reserve(256);
        ackBuf.push_back(0); // success
        
        auto pDW = [&](DWORD d) { ackBuf.push_back(d & 0xFF); ackBuf.push_back((d>>8)&0xFF); ackBuf.push_back((d>>16)&0xFF); ackBuf.push_back((d>>24)&0xFF); };
        auto pW = [&](WORD w) { ackBuf.push_back(w & 0xFF); ackBuf.push_back((w>>8)&0xFF); };
        auto pB = [&](BYTE b) { ackBuf.push_back(b); };
        auto pS = [&](const std::string& str) { pW((WORD)str.length()); for (char c : str) pB(c); };

        pDW(mapID); pW(1);
        pDW(objCopy.dwObjectID);
        pW(objCopy.wPosX); pW(objCopy.wPosY); pB(objCopy.bHeight);
        if (objCopy.bIsMoving) {
            pW(objCopy.wMoveDesX); pW(objCopy.wMoveDesY); pB(objCopy.bMoveDesH);
        } else {
            pW(objCopy.wPosX); pW(objCopy.wPosY); pB(objCopy.bHeight);
        }
        pB(objCopy.wWalkSpeed & 0xFF);
        pW(0); pB(0); pB(objCopy.bPropType);
        pS(objCopy.szName); pDW(0);
        pB(objCopy.bShopStatus); pS(objCopy.strShopName); pS(objCopy.strShopDescription);
        pB(0); pB(0); pB(0); pB(0); pB(0);
        pDW(objCopy.dwMunpaID); pDW(0);
        for(int i=0; i<9; i++) {
            pW(0); pB(0); pB(0); pB(0);
        }
        pB(0); pB(0); pDW(0);

        std::vector<BYTE> fullAck; fullAck.resize(4);
        fullAck.insert(fullAck.end(), ackBuf.begin(), ackBuf.end());
        PACKET_HEADER* ah = (PACKET_HEADER*)fullAck.data();
        ah->id = 0x4412; // CS_IT_CHARINFOLIST_ACK
        ah->payloadSize = fullAck.size() - sizeof(PACKET_HEADER);
        EncryptPacket(fullAck.data(), 0x42);

        // Broadcast to all players in AOI
        auto players = mapInst->GetPlayersInAOI(objCopy.wPosX, objCopy.wPosY);
        for (PlayerData* p : players) {
            SOCKET ts = SessionMgr::GetInstance().GetSocketByCharID(p->dwObjectID - 400000000);
            if (ts != INVALID_SOCKET) {
                SafeSend(ts, (const char*)fullAck.data(), fullAck.size(), 0);
            }
        }
    }
}

// 1. CS_RL_CREATEMUNPA_REQ (0x3A19)
void OnCreateMunpaReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    std::string szMunpaName = ReadString(p, end);
    
    LOG("[MunpaHandler] OnCreateMunpaReq: Name=" + szMunpaName + " CreatorCharID=" + std::to_string(charID));
    
    BYTE bResult = 99;
    DWORD dwMunpaID = 0;
    WORD wTotalTp = 0, wRemainTp = 0;
    
    bool ok = CharacterDB::GetInstance().ExecCreateMunpa(szMunpaName, charID, (DWORD)time(NULL), 1, bResult, dwMunpaID, wTotalTp, wRemainTp);
    
    std::vector<BYTE> ack;
    ack.push_back(bResult);
    if (ok && bResult == 0) {
        pushDWord(ack, dwMunpaID);
        pushString(ack, szMunpaName);
        
        // Update in-memory PlayerData of the creator
        UpdatePlayerInMemoryGuild(charID, dwMunpaID, 1, szMunpaName, "", 0);
        
        // Send character status sync to update TP count visually
        ::SendCharStatusInfoAck(s, charID, 0x4414);
        
        // Broadcast visual update
        BroadcastPlayerVisualUpdate(charID);
    }
    
    SendPacketToSocket(s, 0x3A1A, ack); // CS_RL_CREATEMUNPA_ACK
}

// 2. CS_RL_DELETEMUNPA_REQ (0x3A1B)
void OnDeleteMunpaReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    LOG("[MunpaHandler] OnDeleteMunpaReq: CharID=" + std::to_string(charID));
    
    DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
    std::string szMunpaName, szMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
    
    BYTE bResult = 99;
    if (dwMunpaID > 0 && dwMunpaOrder == 1) {
        // Collect all online guild member CharIDs before deleting to clear caching
        std::vector<DWORD> onlineMembers;
        std::string q = "SELECT dwCharID FROM CHAR_BASIC WHERE dwMunpaID = " + std::to_string(dwMunpaID);
        DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
            DWORD memberID = 0; SQLLEN c;
            SQLGetData(hStmt, 1, SQL_C_ULONG, &memberID, 0, &c);
            if (memberID > 0) onlineMembers.push_back(memberID);
        });

        bool ok = CharacterDB::GetInstance().ExecDeleteMunpa(dwMunpaID, bResult);
        if (ok && bResult == 0) {
            // Clear memory caches for all online members
            for (DWORD mid : onlineMembers) {
                UpdatePlayerInMemoryGuild(mid, 0, 0, "", "", 0);
                BroadcastPlayerVisualUpdate(mid);
            }
        }
    } else {
        bResult = 4; // Not leader or not in Munpa
    }
    
    std::vector<BYTE> ack;
    ack.push_back(bResult);
    SendPacketToSocket(s, 0x3A1C, ack); // CS_RL_DELETEMUNPA_ACK
}

// 3. CS_RL_ASKMUNWON_REQ (0x3A2A)
void OnAskMunwonReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    
    BYTE bResult = ReadVal<BYTE>(p, end);
    DWORD dwAskID = ReadVal<DWORD>(p, end);
    DWORD dwAskedID = ReadVal<DWORD>(p, end);
    
    LOG("[MunpaHandler] OnAskMunwonReq: bResult=" + std::to_string(bResult) + " AskID=" + std::to_string(dwAskID) + " AskedID=" + std::to_string(dwAskedID));
    
    if (bResult == 1) { // ACT_ASKMUNWON_REQUEST
        DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
        std::string szMunpaName, szMunpaNick;
        CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
        
        if (dwMunpaID > 0 && dwMunpaOrder <= 3) {
            DWORD targetCharID = dwAskedID - 400000000;
            SOCKET targetSocket = SessionMgr::GetInstance().GetSocketByCharID(targetCharID);
            
            // Check if invitee is already in a clan
            DWORD targetMunpaID = 0, targetOrder = 0, targetMark = 0;
            std::string tMunpa, tNick;
            CharacterDB::GetInstance().GetCharMunpaInfo(targetCharID, targetMunpaID, targetOrder, tMunpa, tNick, targetMark);
            
            if (targetSocket != INVALID_SOCKET) {
                if (targetMunpaID > 0) {
                    // Already in clan
                    std::vector<BYTE> errAck;
                    errAck.push_back(4); // ERR_ASKMUNWON_ALREADY_JOIN
                    errAck.push_back(dwAskID & 0xFF); errAck.push_back((dwAskID>>8)&0xFF); errAck.push_back((dwAskID>>16)&0xFF); errAck.push_back(dwAskID>>24);
                    errAck.push_back(dwAskedID & 0xFF); errAck.push_back((dwAskedID>>8)&0xFF); errAck.push_back((dwAskedID>>16)&0xFF); errAck.push_back(dwAskedID>>24);
                    SendPacketToSocket(s, 0x3A2B, errAck);
                } else {
                    // Forward invite to invitee
                    std::vector<BYTE> askAck;
                    askAck.push_back(1); // ACT_ASKMUNWON_REQUEST
                    pushDWord(askAck, dwAskID);
                    pushDWord(askAck, dwAskedID);
                    SendPacketToSocket(targetSocket, 0x3A2B, askAck);
                }
            }
        }
    } else if (bResult == 2) { // ACT_ASKMUNWON_CANCEL (Rejection)
        DWORD targetCharID = dwAskID - 400000000;
        SOCKET targetSocket = SessionMgr::GetInstance().GetSocketByCharID(targetCharID);
        if (targetSocket != INVALID_SOCKET) {
            std::vector<BYTE> ack;
            ack.push_back(2); // ACT_ASKMUNWON_CANCEL
            pushDWord(ack, dwAskID);
            pushDWord(ack, dwAskedID);
            SendPacketToSocket(targetSocket, 0x3A2B, ack);
        }
    } else if (bResult == 0) { // ACT_ASKMUNWON_OK (Accept)
        DWORD inviterCharID = dwAskID - 400000000;
        DWORD inviteeCharID = dwAskedID - 400000000;
        
        DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
        std::string szMunpaName, szMunpaNick;
        CharacterDB::GetInstance().GetCharMunpaInfo(inviterCharID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
        
        if (dwMunpaID > 0) {
            BYTE dbRes = 99;
            bool ok = CharacterDB::GetInstance().ExecAddMunwon(dwMunpaID, 6, inviteeCharID, dbRes);
            if (ok && dbRes == 0) {
                // Fetch invitee visual name
                std::string vizName; BYTE vizType = 0;
                CharacterDB::GetInstance().GetCharVisual(inviteeCharID, vizName, vizType);
                
                // Update invitee memory
                UpdatePlayerInMemoryGuild(inviteeCharID, dwMunpaID, 6, szMunpaName, "", dwMarkID);
                BroadcastPlayerVisualUpdate(inviteeCharID);
                
                // Send CS_RL_ADDMUNWON_ACK (0x3A2C) to all guild members
                std::vector<BYTE> addAck;
                pushDWord(addAck, dwMunpaID);
                pushDWord(addAck, 6);
                pushString(addAck, "门众");
                pushDWord(addAck, inviteeCharID);
                pushString(addAck, vizName);
                
                BroadcastToMunpa(dwMunpaID, 0x3A2C, addAck);
            }
        }
    }
}

// 4. CS_RL_DELMUNWON_REQ (0x3A2D)
void OnDelMunwonReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    
    DWORD dwCharID = ReadVal<DWORD>(p, end);
    DWORD dwOrderID = ReadVal<DWORD>(p, end);
    
    LOG("[MunpaHandler] OnDelMunwonReq: TargetCharID=" + std::to_string(dwCharID) + " TargetOrder=" + std::to_string(dwOrderID));
    
    DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
    std::string szMunpaName, szMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
    
    if (dwMunpaID == 0) return;
    
    bool authorized = false;
    if (dwCharID == charID) {
        // Voluntary leave: Sect Master cannot leave this way
        if (dwMunpaOrder != 1) authorized = true;
    } else {
        // Kick: Sender must be master/vice master and higher order than target
        if (dwMunpaOrder <= 2 && dwMunpaOrder < dwOrderID) authorized = true;
    }
    
    if (authorized) {
        // Delete member from DB
        std::string q = "UPDATE CHAR_BASIC SET dwMunpaID = 0, dwMunpaOrder = 0, dateSecede = GETDATE() WHERE dwCharID = " + std::to_string(dwCharID);
        DBHelper::GetInstance().ExecuteUpdate(q);
        
        // Update in-memory player caching
        UpdatePlayerInMemoryGuild(dwCharID, 0, 0, "", "", 0);
        BroadcastPlayerVisualUpdate(dwCharID);
        
        // Broadcast CS_RL_DELMUNWON_ACK (0x3A2E) to members of the guild
        std::vector<BYTE> delAck;
        delAck.push_back(0); // success
        pushDWord(delAck, dwMunpaID);
        pushDWord(delAck, dwOrderID);
        pushDWord(delAck, dwCharID);
        
        BroadcastToMunpa(dwMunpaID, 0x3A2E, delAck);
        
        // Send to kicked player too if they are online so their UI cleans up
        SOCKET ks = SessionMgr::GetInstance().GetSocketByCharID(dwCharID);
        if (ks != INVALID_SOCKET) {
            SendPacketToSocket(ks, 0x3A2E, delAck);
        }
    }
}

// 5. CS_RL_CHANGEMUNWONORDER_REQ (0x3A2F)
void OnChangeMunwonOrderReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    
    DWORD dwCharID = ReadVal<DWORD>(p, end);
    DWORD dwOldOrderID = ReadVal<DWORD>(p, end);
    DWORD dwNewOrderID = ReadVal<DWORD>(p, end);
    
    LOG("[MunpaHandler] OnChangeMunwonOrderReq: TargetCharID=" + std::to_string(dwCharID) + " OldOrder=" + std::to_string(dwOldOrderID) + " NewOrder=" + std::to_string(dwNewOrderID));
    
    DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
    std::string szMunpaName, szMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
    
    if (dwMunpaID > 0 && dwMunpaOrder <= 2) { // master or vice master
        BYTE bResult = 99;
        bool ok = CharacterDB::GetInstance().ExecChangeMunwon(dwMunpaID, dwCharID, dwOldOrderID, dwNewOrderID, bResult);
        if (ok && bResult == 0) {
            // Find rank name from ORDERTEMPLATE
            std::string rankName = "门众";
            std::string q = "SELECT szOrderName FROM MUNPA_ORDERTEMPLATE WHERE dwOrderID = " + std::to_string(dwNewOrderID);
            DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
                char buf[64] = {0}; SQLLEN c;
                SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &c);
                rankName = buf;
            });
            
            // Update in-memory
            UpdatePlayerInMemoryGuild(dwCharID, dwMunpaID, dwNewOrderID, szMunpaName, szMunpaNick, dwMarkID);
            
            std::vector<BYTE> ack;
            ack.push_back(0); // success
            pushDWord(ack, dwMunpaID);
            pushDWord(ack, dwNewOrderID);
            pushString(ack, rankName);
            pushDWord(ack, dwCharID);
            
            BroadcastToMunpa(dwMunpaID, 0x3A30, ack); // CS_RL_CHANGEMUNWONORDER_ACK
        }
    }
}

// 6. CS_RL_MUNWONLIST_REQ (0x3A34)
void OnMunwonListReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    LOG("[MunpaHandler] OnMunwonListReq: CharID=" + std::to_string(charID));
    
    DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
    std::string szMunpaName, szMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
    
    std::vector<BYTE> ack;
    if (dwMunpaID > 0) {
        pushDWord(ack, dwMunpaID);
        pushString(ack, szMunpaName);
        
        struct MemberEntry {
            DWORD dwOrderID;
            std::string szOrderName;
            DWORD dwCharID;
            std::string szCharName;
            std::string szMunpaNick;
            BYTE bCharType;
        };
        std::vector<MemberEntry> members;
        
        std::string q = "SELECT O.dwMunpaOrder, ISNULL(T.szOrderName, '门众'), O.dwCharID, O.szNickName, ISNULL(O.szMunpaNickName, ''), O.bCharType "
                        "FROM CHAR_BASIC O "
                        "LEFT JOIN MUNPA_ORDERTEMPLATE T ON O.dwMunpaOrder = T.dwOrderID "
                        "WHERE O.dwMunpaID = " + std::to_string(dwMunpaID) + " ORDER BY O.dwMunpaOrder ASC";
                        
        DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
            MemberEntry me; SQLLEN c;
            char orderName[64] = {0};
            char charName[64] = {0};
            char munpaNick[64] = {0};
            
            SQLGetData(hStmt, 1, SQL_C_ULONG, &me.dwOrderID, 0, &c);
            SQLGetData(hStmt, 2, SQL_C_CHAR, orderName, sizeof(orderName), &c);
            SQLGetData(hStmt, 3, SQL_C_ULONG, &me.dwCharID, 0, &c);
            SQLGetData(hStmt, 4, SQL_C_CHAR, charName, sizeof(charName), &c);
            SQLGetData(hStmt, 5, SQL_C_CHAR, munpaNick, sizeof(munpaNick), &c);
            SQLGetData(hStmt, 6, SQL_C_UTINYINT, &me.bCharType, 0, &c);
            
            me.szOrderName = orderName;
            me.szCharName = charName;
            me.szMunpaNick = munpaNick;
            members.push_back(me);
        });
        
        pushDWord(ack, (DWORD)members.size());
        for (const auto& me : members) {
            pushDWord(ack, me.dwOrderID);
            pushString(ack, me.szOrderName);
            pushDWord(ack, me.dwCharID);
            pushString(ack, me.szCharName);
            pushString(ack, me.szMunpaNick);
            
            // Check online state
            bool online = (SessionMgr::GetInstance().GetSocketByCharID(me.dwCharID) != INVALID_SOCKET);
            ack.push_back(online ? 1 : 0);
            ack.push_back(me.bCharType);
        }
    } else {
        pushDWord(ack, 0);
        pushString(ack, "");
        pushDWord(ack, 0);
    }
    
    SendPacketToSocket(s, 0x3A35, ack); // CS_RL_MUNWONLIST_ACK
}

// 7. CS_RL_MUNWONINFO_REQ (0x3A36)
void OnMunwonInfoReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    DWORD dwMunwonID = ReadVal<DWORD>(p, end);
    
    LOG("[MunpaHandler] OnMunwonInfoReq: MemberID=" + std::to_string(dwMunwonID));
    
    DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
    std::string szMunpaName, szMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(dwMunwonID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
    
    std::vector<BYTE> ack;
    if (dwMunpaID > 0) {
        pushDWord(ack, dwMunwonID);
        
        std::string charName = "";
        BYTE bCharType = 0;
        WORD wLevel = 1;
        std::string rankName = "门众";
        
        std::string q = "SELECT O.szNickName, O.bCharType, P.wLevel, ISNULL(T.szOrderName, '门众') "
                        "FROM CHAR_BASIC O "
                        "INNER JOIN CHAR_POWER P ON O.dwCharID = P.dwCharID "
                        "LEFT JOIN MUNPA_ORDERTEMPLATE T ON O.dwMunpaOrder = T.dwOrderID "
                        "WHERE O.dwCharID = " + std::to_string(dwMunwonID);
                        
        DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
            char name[64] = {0};
            char order[64] = {0};
            SQLLEN c;
            SQLGetData(hStmt, 1, SQL_C_CHAR, name, sizeof(name), &c);
            SQLGetData(hStmt, 2, SQL_C_UTINYINT, &bCharType, 0, &c);
            SQLGetData(hStmt, 3, SQL_C_USHORT, &wLevel, 0, &c);
            SQLGetData(hStmt, 4, SQL_C_CHAR, order, sizeof(order), &c);
            charName = name;
            rankName = order;
        });
        
        pushString(ack, charName);
        pushDWord(ack, dwMunpaOrder);
        pushString(ack, rankName);
        pushString(ack, szMunpaNick);
        
        bool online = (SessionMgr::GetInstance().GetSocketByCharID(dwMunwonID) != INVALID_SOCKET);
        ack.push_back(online ? 1 : 0);
        pushWord(ack, wLevel);
        ack.push_back(bCharType);
    }
    
    SendPacketToSocket(s, 0x3A37, ack); // CS_RL_MUNWONINFO_ACK
}

// 8. CS_RL_MUNPACHAT_REQ (0x3A38)
void OnMunpaChatReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    
    BYTE bType = ReadVal<BYTE>(p, end);
    std::string szMsg = ReadString(p, end);
    
    DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
    std::string szMunpaName, szMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
    
    if (dwMunpaID > 0) {
        std::vector<BYTE> chatAck;
        pushDWord(chatAck, charID + 400000000); // sender ObjectID
        chatAck.push_back(bType);
        pushString(chatAck, szMsg);
        
        BroadcastToMunpa(dwMunpaID, 0x3A39, chatAck); // CS_RL_MUNPACHAT_ACK
    }
}

// 9. CS_RL_MUNPAINFO_REQ (0x3A3A)
void OnMunpaInfoReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    BYTE bType = ReadVal<BYTE>(p, end);
    DWORD dwReqMunpaID = ReadVal<DWORD>(p, end);
    
    LOG("[MunpaHandler] OnMunpaInfoReq: Type=" + std::to_string(bType) + " ReqMunpaID=" + std::to_string(dwReqMunpaID));
    
    if (dwReqMunpaID == 0) {
        // Query sender's Munpa
        DWORD order = 0, mark = 0; std::string name, nick;
        CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwReqMunpaID, order, name, nick, mark);
    }
    
    std::vector<BYTE> ack;
    if (dwReqMunpaID > 0) {
        ack.push_back(1); // bResult = 1 (success)
        pushDWord(ack, dwReqMunpaID);
        
        std::string szName = "", szMunjuName = "", szEnemyName = "", szFriendName = "", szNotice = "";
        DWORD dwMunjuID = 0, dwStoneID = 0, dwFame = 0, dwRank = 0, dwTotalWar = 0, dwWinWar = 0, dwDrawWar = 0, dwLossWar = 0, dwMarkID = 0, dwTaxMoney = 0;
        BYTE bLevel = 1, bChannelID = 0, bWarStatus = 0;
        DWORD dwMapID = 0, dwCanBattleTime = 0, dwEnemyMunpaID = 0, dwFriendMunpaID = 0, dwBattleTime = 0;
        
        // Fetch MUNPA_BASIC fields
        std::string q = "SELECT szName, dwMunjuID, dwStoneID, bChannelID, dwMapID, dwFame, bLevel, dwRank, dwCanBattleTime, dwEnemyMunpaID, dwFriendMunpaID, dwBattleTime, bWarStatus, dwTotalWar, dwWinWar, dwDrawWar, dwLossWar, szNotice, dwMarkID, dwTaxMoney "
                        "FROM MUNPA_BASIC WHERE dwMunpaID = " + std::to_string(dwReqMunpaID);
                        
        DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
            char nameBuf[64] = {0};
            char noticeBuf[256] = {0};
            SQLLEN c;
            
            SQLGetData(hStmt, 1, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c);
            SQLGetData(hStmt, 2, SQL_C_ULONG, &dwMunjuID, 0, &c);
            SQLGetData(hStmt, 3, SQL_C_ULONG, &dwStoneID, 0, &c);
            SQLGetData(hStmt, 4, SQL_C_UTINYINT, &bChannelID, 0, &c);
            SQLGetData(hStmt, 5, SQL_C_ULONG, &dwMapID, 0, &c);
            SQLGetData(hStmt, 6, SQL_C_ULONG, &dwFame, 0, &c);
            SQLGetData(hStmt, 7, SQL_C_UTINYINT, &bLevel, 0, &c);
            SQLGetData(hStmt, 8, SQL_C_ULONG, &dwRank, 0, &c);
            SQLGetData(hStmt, 9, SQL_C_ULONG, &dwCanBattleTime, 0, &c);
            SQLGetData(hStmt, 10, SQL_C_ULONG, &dwEnemyMunpaID, 0, &c);
            SQLGetData(hStmt, 11, SQL_C_ULONG, &dwFriendMunpaID, 0, &c);
            SQLGetData(hStmt, 12, SQL_C_ULONG, &dwBattleTime, 0, &c);
            SQLGetData(hStmt, 13, SQL_C_UTINYINT, &bWarStatus, 0, &c);
            SQLGetData(hStmt, 14, SQL_C_ULONG, &dwTotalWar, 0, &c);
            SQLGetData(hStmt, 15, SQL_C_ULONG, &dwWinWar, 0, &c);
            SQLGetData(hStmt, 16, SQL_C_ULONG, &dwDrawWar, 0, &c);
            SQLGetData(hStmt, 17, SQL_C_ULONG, &dwLossWar, 0, &c);
            SQLGetData(hStmt, 18, SQL_C_CHAR, noticeBuf, sizeof(noticeBuf), &c);
            SQLGetData(hStmt, 19, SQL_C_ULONG, &dwMarkID, 0, &c);
            SQLGetData(hStmt, 20, SQL_C_ULONG, &dwTaxMoney, 0, &c);
            
            szName = nameBuf;
            szNotice = noticeBuf;
        });
        
        // Fetch Master Nickname
        if (dwMunjuID > 0) {
            std::string q2 = "SELECT szNickName FROM CHAR_BASIC WHERE dwCharID = " + std::to_string(dwMunjuID);
            DBHelper::GetInstance().ExecuteQuery(q2, [&](SQLHSTMT hStmt) {
                char buf[64] = {0}; SQLLEN c;
                SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &c);
                szMunjuName = buf;
            });
        }
        
        // Fetch Enemy Clan Name
        if (dwEnemyMunpaID > 0) {
            std::string q3 = "SELECT szName FROM MUNPA_BASIC WHERE dwMunpaID = " + std::to_string(dwEnemyMunpaID);
            DBHelper::GetInstance().ExecuteQuery(q3, [&](SQLHSTMT hStmt) {
                char buf[64] = {0}; SQLLEN c;
                SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &c);
                szEnemyName = buf;
            });
        }
        
        // Fetch Ally Clan Name
        if (dwFriendMunpaID > 0) {
            std::string q4 = "SELECT szName FROM MUNPA_BASIC WHERE dwMunpaID = " + std::to_string(dwFriendMunpaID);
            DBHelper::GetInstance().ExecuteQuery(q4, [&](SQLHSTMT hStmt) {
                char buf[64] = {0}; SQLLEN c;
                SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &c);
                szFriendName = buf;
            });
        }
        
        // Fetch Member Count
        DWORD dwMunwonNum = 0;
        std::string q5 = "SELECT COUNT(*) FROM CHAR_BASIC WHERE dwMunpaID = " + std::to_string(dwReqMunpaID);
        DBHelper::GetInstance().ExecuteQuery(q5, [&](SQLHSTMT hStmt) {
            int cnt = 0; SQLLEN c;
            SQLGetData(hStmt, 1, SQL_C_SLONG, &cnt, 0, &c);
            dwMunwonNum = (DWORD)cnt;
        });
        
        pushString(ack, szName);
        pushDWord(ack, dwMunjuID);
        pushString(ack, szMunjuName);
        pushDWord(ack, dwStoneID);
        ack.push_back(bChannelID);
        pushDWord(ack, dwMapID);
        pushDWord(ack, dwFame);
        ack.push_back(bLevel);
        pushDWord(ack, dwRank);
        pushDWord(ack, dwCanBattleTime);
        pushString(ack, szEnemyName);
        pushString(ack, szFriendName);
        pushDWord(ack, dwBattleTime);
        ack.push_back(bWarStatus);
        pushDWord(ack, dwTotalWar);
        pushDWord(ack, dwWinWar);
        pushDWord(ack, dwDrawWar);
        pushDWord(ack, dwLossWar);
        pushDWord(ack, dwMunwonNum);
        pushString(ack, szNotice);
        pushDWord(ack, dwMarkID);
        pushDWord(ack, dwTaxMoney);
    } else {
        ack.push_back(0); // bResult = 0 (fail/no clan)
    }
    
    SendPacketToSocket(s, 0x3A3B, ack); // CS_RL_MUNPAINFO_ACK
}

// 10. CS_RL_MUNPANICK_REQ (0x3A3C)
void OnMunpaNickReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    
    DWORD dwMunpaID = ReadVal<DWORD>(p, end);
    DWORD dwTargetCharID = ReadVal<DWORD>(p, end);
    std::string szMunpaNick = ReadString(p, end);
    
    LOG("[MunpaHandler] OnMunpaNickReq: TargetCharID=" + std::to_string(dwTargetCharID) + " Nick=" + szMunpaNick);
    
    DWORD dwMyMunpaID = 0, dwMyMunpaOrder = 0, dwMyMarkID = 0;
    std::string szMyMunpaName, szMyMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMyMunpaID, dwMyMunpaOrder, szMyMunpaName, szMyMunpaNick, dwMyMarkID);
    
    if (dwMyMunpaID > 0 && dwMyMunpaID == dwMunpaID && dwMyMunpaOrder <= 2) { // Master or Vice Master
        BYTE bResult = 99;
        bool ok = CharacterDB::GetInstance().ExecChangeMunpaNick(dwMunpaID, dwTargetCharID, szMunpaNick, bResult);
        if (ok && bResult == 0) {
            // Update target memory cached value
            UpdatePlayerInMemoryGuild(dwTargetCharID, dwMunpaID, 6, szMyMunpaName, szMunpaNick, dwMyMarkID);
            
            std::vector<BYTE> ack;
            ack.push_back(0); // success
            pushDWord(ack, dwMunpaID);
            pushDWord(ack, dwTargetCharID);
            pushString(ack, szMunpaNick);
            
            BroadcastToMunpa(dwMunpaID, 0x3A3D, ack); // CS_RL_MUNPANICK_ACK
        }
    }
}

// 11. CS_RL_MUNPANOTICE_REQ (0x3A5D)
void OnMunpaNoticeReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    std::string szNotice = ReadString(p, end);
    
    LOG("[MunpaHandler] OnMunpaNoticeReq: Notice=" + szNotice);
    
    DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
    std::string szMunpaName, szMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
    
    BYTE bResult = 4; // internal error
    if (dwMunpaID > 0) {
        if (dwMunpaOrder == 1) { // Only Sect Master
            std::string q = "UPDATE MUNPA_BASIC SET szNotice = '" + szNotice + "' WHERE dwMunpaID = " + std::to_string(dwMunpaID);
            if (DBHelper::GetInstance().ExecuteUpdate(q)) {
                bResult = 0; // ERR_MUNPANOTICE_SUCCESS
            }
        } else {
            bResult = 2; // ERR_MUNPANOTICE_ONLYMUNJU
        }
    }
    
    std::vector<BYTE> ack;
    ack.push_back(bResult);
    SendPacketToSocket(s, 0x3A5E, ack); // CS_RL_MUNPANOTICE_ACK
}

// 12. CS_RL_MUNPAMARKREG_REQ (0x3A5F)
void OnMunpaMarkRegReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    
    BYTE bRegType = ReadVal<BYTE>(p, end);
    DWORD dwMunpaID = ReadVal<DWORD>(p, end);
    std::string szMarkImage = ReadString(p, end);
    
    LOG("[MunpaHandler] OnMunpaMarkRegReq: RegType=" + std::to_string(bRegType) + " MunpaID=" + std::to_string(dwMunpaID));
    
    DWORD dwMyMunpaID = 0, dwMyMunpaOrder = 0, dwMyMarkID = 0;
    std::string szMyMunpaName, szMyMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMyMunpaID, dwMyMunpaOrder, szMyMunpaName, szMyMunpaNick, dwMyMarkID);
    
    BYTE bResult = 99;
    DWORD dwMarkID = 0;
    
    if (dwMyMunpaID > 0 && dwMyMunpaID == dwMunpaID && dwMyMunpaOrder == 1) { // leader
        bool ok = CharacterDB::GetInstance().ExecMunpaMarkReg(bRegType, dwMunpaID, szMarkImage, dwMarkID, bResult);
        if (ok && bResult == 0) {
            // Collect online members
            std::vector<DWORD> members;
            std::string q = "SELECT dwCharID FROM CHAR_BASIC WHERE dwMunpaID = " + std::to_string(dwMunpaID);
            DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
                DWORD mId = 0; SQLLEN c;
                SQLGetData(hStmt, 1, SQL_C_ULONG, &mId, 0, &c);
                if (mId > 0) members.push_back(mId);
            });
            
            // Sync memory cached dwMunpaMarkID and visual rendering for everyone online in guild
            for (DWORD mid : members) {
                UpdatePlayerInMemoryGuild(mid, dwMunpaID, 6, szMyMunpaName, szMyMunpaNick, dwMarkID);
                BroadcastPlayerVisualUpdate(mid);
            }
        }
    } else {
        bResult = 2; // No permission
    }
    
    std::vector<BYTE> ack;
    ack.push_back(bRegType);
    ack.push_back(bResult);
    if (bResult == 0) {
        pushDWord(ack, dwMarkID);
    }
    SendPacketToSocket(s, 0x3A60, ack); // CS_RL_MUNPAMARKREG_ACK
}

// 13. CS_RL_GAINMARKIMAGE_REQ (0x3A62)
void OnGainMarkImageReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload + 4;
    BYTE* end = payload + size;
    DWORD dwMarkID = ReadVal<DWORD>(p, end);
    
    LOG("[MunpaHandler] OnGainMarkImageReq: MarkID=" + std::to_string(dwMarkID));
    
    std::string szMarkImage = "";
    std::string q = "SELECT szMarkImage FROM MUNPA_MARK WHERE dwMarkID = " + std::to_string(dwMarkID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        char buf[2048] = {0}; SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &c);
        szMarkImage = buf;
    });
    
    std::vector<BYTE> ack;
    pushDWord(ack, dwMarkID);
    pushString(ack, szMarkImage);
    
    SendPacketToSocket(s, 0x3A63, ack); // CS_RL_GAINMARKIMAGE_ACK
}

// Register all Munpa handlers in the network routing engine
void RegisterMunpaHandlers() {
    RegisterHandler(0x3A19, [](SOCKET s, BYTE* p, WORD size) { OnCreateMunpaReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A1B, [](SOCKET s, BYTE* p, WORD size) { OnDeleteMunpaReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A2A, [](SOCKET s, BYTE* p, WORD size) { OnAskMunwonReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A2D, [](SOCKET s, BYTE* p, WORD size) { OnDelMunwonReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A2F, [](SOCKET s, BYTE* p, WORD size) { OnChangeMunwonOrderReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A34, [](SOCKET s, BYTE* p, WORD size) { OnMunwonListReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A36, [](SOCKET s, BYTE* p, WORD size) { OnMunwonInfoReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A38, [](SOCKET s, BYTE* p, WORD size) { OnMunpaChatReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A3A, [](SOCKET s, BYTE* p, WORD size) { OnMunpaInfoReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A3C, [](SOCKET s, BYTE* p, WORD size) { OnMunpaNickReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A5D, [](SOCKET s, BYTE* p, WORD size) { OnMunpaNoticeReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A5F, [](SOCKET s, BYTE* p, WORD size) { OnMunpaMarkRegReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A62, [](SOCKET s, BYTE* p, WORD size) { OnGainMarkImageReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    
    LOG("[MunpaHandler] 13 Sect/Munpa Handlers Registered successfully!");
}

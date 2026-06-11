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
    // 客户端 sString 的 len 包含 null terminator，构造 string 时用 strnlen 排除，
    // 防止 \0 嵌入 std::string 内部导致 SQL 拼接时引号截断
    size_t actualLen = strnlen((char*)p, len);
    std::string s((char*)p, actualLen);
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
    // 客户端 sString 格式: WORD len（含 null terminator）+ char data[len]（含 null terminator）
    WORD len = (WORD)(str.length() + 1);
    pushWord(buf, len);
    for (char c : str) buf.push_back((BYTE)c);
    buf.push_back(0); // null terminator
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
        auto pS = [&](const std::string& str) { pW((WORD)(str.length() + 1)); for (char c : str) pB(c); pB(0); };

        pDW(mapID); pW(1);
        pDW(objCopy.dwObjectID);
        pB(1); // bObjectType = 1 (PC)
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
            pW(objCopy.wVisualID[i]);
            pB(objCopy.bStxType[i]);
            pB(objCopy.bRarity[i]);
            pB(objCopy.bNeedCharType[i]);
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

// 前向声明：创建门派成功后需要主动推送成员列表
void OnMunwonListReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);

// 1. CS_RL_CREATEMUNPA_REQ (0x3A19)
void OnCreateMunpaReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload;
    BYTE* end = payload + size;
    std::string szMunpaName = ReadString(p, end);
    
    LOG("[MunpaHandler] OnCreateMunpaReq: Name=" + szMunpaName + " CreatorCharID=" + std::to_string(charID));
    
    BYTE bResult = 99;
    DWORD dwMunpaID = 0;
    WORD wTotalTp = 0, wRemainTp = 0;
    
    bool ok = CharacterDB::GetInstance().ExecCreateMunpa(szMunpaName, charID, (DWORD)time(NULL), 1, bResult, dwMunpaID, wTotalTp, wRemainTp);
    
    LOG("[MunpaHandler] ExecCreateMunpa result: ok=" + std::to_string(ok) + " bResult=" + std::to_string(bResult) + " dwMunpaID=" + std::to_string(dwMunpaID) + " wTotalTp=" + std::to_string(wTotalTp) + " wRemainTp=" + std::to_string(wRemainTp));
    
    std::vector<BYTE> ack;
    ack.push_back(bResult);
    if (ok && bResult == 0) {
        pushDWord(ack, dwMunpaID);
        pushString(ack, szMunpaName);
        
        // Update in-memory PlayerData of the creator
        UpdatePlayerInMemoryGuild(charID, dwMunpaID, 1, szMunpaName, "", 0);
        
        // Send character status sync to update TP count visually
        ::SendCharStatusInfoAck(s, charID, 0x4414);
        
        // 视觉广播暂时禁用——CHARINFOLIST_ACK 包格式与客户端不完全匹配会导致角色模型丢失
        // 创建门派后，客户端 SetClanInfo 已更新本地门派状态，其他玩家的门派标记会在 AOI 同步时自然更新
        // BroadcastPlayerVisualUpdate(charID);
    }
    
    LOG("[MunpaHandler] Sending CREATEMUNPA_ACK (0x3A1A) payload size=" + std::to_string(ack.size()));
    SendPacketToSocket(s, 0x3A1A, ack); // CS_RL_CREATEMUNPA_ACK
    LOG("[MunpaHandler] CREATEMUNPA_ACK sent successfully");
    
    // 创建成功后主动推送成员列表，填充 WINDOW_MUNPA 面板
    // 不推送 MUNPAINFO_ACK（会打开公告板窗口覆盖成员列表面板）
    if (ok && bResult == 0 && dwMunpaID > 0) {
        OnMunwonListReq(s, charID, nullptr, 0);
    }
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
    BYTE* p = payload;
    BYTE* end = payload + size;
    
    BYTE bResult = ReadVal<BYTE>(p, end);
    DWORD dwAskID = ReadVal<DWORD>(p, end);
    DWORD dwAskedID = ReadVal<DWORD>(p, end);
    
    LOG("[MunpaHandler] OnAskMunwonReq: bResult=" + std::to_string(bResult) + " AskID=" + std::to_string(dwAskID) + " AskedID=" + std::to_string(dwAskedID));
    
    if (bResult == 1) { // ACT_ASKMUNWON_REQUEST
        // 查发送者的门派信息
        DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
        std::string szMunpaName, szMunpaNick;
        CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
        
        // 查目标玩家的门派信息
        DWORD targetCharID = dwAskedID - 400000000;
        DWORD targetMunpaID = 0, targetOrder = 0, targetMark = 0;
        std::string tMunpa, tNick;
        CharacterDB::GetInstance().GetCharMunpaInfo(targetCharID, targetMunpaID, targetOrder, tMunpa, tNick, targetMark);
        
        if (dwMunpaID > 0 && dwMunpaOrder <= 3) {
            // ===== 场景1: 发送者是门主/副门主，邀请目标加入 =====
            SOCKET targetSocket = SessionMgr::GetInstance().GetSocketByCharID(targetCharID);
            
            if (targetSocket != INVALID_SOCKET) {
                if (targetMunpaID > 0) {
                    // 目标已有门派
                    std::vector<BYTE> errAck;
                    errAck.push_back(4); // ERR_ASKMUNWON_ALREADY_JOIN
                    pushDWord(errAck, dwAskID);
                    pushDWord(errAck, dwAskedID);
                    SendPacketToSocket(s, 0x3A2B, errAck);
                } else {
                    // 转发邀请给目标
                    std::vector<BYTE> askAck;
                    askAck.push_back(1); // ACT_ASKMUNWON_REQUEST
                    pushDWord(askAck, dwAskID);
                    pushDWord(askAck, dwAskedID);
                    SendPacketToSocket(targetSocket, 0x3A2B, askAck);
                }
            } else {
                // 目标不在线
                std::vector<BYTE> errAck;
                errAck.push_back(7); // 不在线
                pushDWord(errAck, dwAskID);
                pushDWord(errAck, dwAskedID);
                SendPacketToSocket(s, 0x3A2B, errAck);
            }
        } else if (dwMunpaID == 0 && targetMunpaID > 0) {
            // ===== 场景2: 发送者无门派，向有门派的目标申请加入 =====
            // 查目标门派的门主 CharID
            DWORD leaderCharID = 0;
            DBHelper::GetInstance().ExecuteQuery(
                "SELECT dwCharID FROM CHAR_BASIC WHERE dwMunpaID = " + std::to_string(targetMunpaID) + " AND dwMunpaOrder = 1",
                [&](SQLHSTMT hStmt) {
                    SQLLEN ind;
                    SQLGetData(hStmt, 1, SQL_C_ULONG, &leaderCharID, 0, &ind);
                });
            
            if (leaderCharID > 0) {
                SOCKET leaderSocket = SessionMgr::GetInstance().GetSocketByCharID(leaderCharID);
                if (leaderSocket != INVALID_SOCKET) {
                    // 将申请转发给门主（AskID=申请者, AskedID=申请者自己）
                    // 门主收到后弹出"XXX想加入门派"对话框
                    DWORD leaderObjID = leaderCharID + 400000000;
                    std::vector<BYTE> askAck;
                    askAck.push_back(1); // ACT_ASKMUNWON_REQUEST
                    pushDWord(askAck, dwAskID);         // 申请者 ObjectID
                    pushDWord(askAck, dwAskedID);       // 被选中目标 ObjectID（门主判断用）
                    SendPacketToSocket(leaderSocket, 0x3A2B, askAck);
                    
                    LOG("[MunpaHandler] 申请加入: 转发给门主 CharID=" + std::to_string(leaderCharID));
                } else {
                    // 门主不在线
                    std::vector<BYTE> errAck;
                    errAck.push_back(7); // 门主不在线
                    pushDWord(errAck, dwAskID);
                    pushDWord(errAck, dwAskedID);
                    SendPacketToSocket(s, 0x3A2B, errAck);
                }
            }
        } else if (dwMunpaID == 0 && targetMunpaID == 0) {
            // 双方都没有门派
            std::vector<BYTE> errAck;
            errAck.push_back(5); // ERR_ASKMUNWON_HASNOTMUNPA
            pushDWord(errAck, dwAskID);
            pushDWord(errAck, dwAskedID);
            SendPacketToSocket(s, 0x3A2B, errAck);
        }
    } else if (bResult == 3) { // ACT_ASKMUNWON_CANCEL (客户端定义: CANCEL=3)
        DWORD targetCharID = dwAskID - 400000000;
        SOCKET targetSocket = SessionMgr::GetInstance().GetSocketByCharID(targetCharID);
        if (targetSocket != INVALID_SOCKET) {
            std::vector<BYTE> ack;
            ack.push_back(3); // ACT_ASKMUNWON_CANCEL
            pushDWord(ack, dwAskID);
            pushDWord(ack, dwAskedID);
            SendPacketToSocket(targetSocket, 0x3A2B, ack);
        }
    } else if (bResult == 2) { // ACT_ASKMUNWON_OK (客户端定义: OK=2)
        // 两种场景: 
        // (1) 门主A邀请B→B接受: dwAskID=A(门主), dwAskedID=B(接受者=发送者charID)
        // (2) B申请加入A→A(门主)接受: dwAskID=B(申请者), dwAskedID=A(门主=发送者charID)
        // 通用策略：查两端的门派信息，有门派的提供 MunpaID，无门派的是被加入者
        
        DWORD charID_Ask = dwAskID - 400000000;
        DWORD charID_Asked = dwAskedID - 400000000;
        
        DWORD munpaID_Ask = 0, order_Ask = 0, mark_Ask = 0;
        std::string mName_Ask, mNick_Ask;
        CharacterDB::GetInstance().GetCharMunpaInfo(charID_Ask, munpaID_Ask, order_Ask, mName_Ask, mNick_Ask, mark_Ask);
        
        DWORD munpaID_Asked = 0, order_Asked = 0, mark_Asked = 0;
        std::string mName_Asked, mNick_Asked;
        CharacterDB::GetInstance().GetCharMunpaInfo(charID_Asked, munpaID_Asked, order_Asked, mName_Asked, mNick_Asked, mark_Asked);
        
        // 确定谁是门派方、谁是被加入者
        DWORD dwMunpaID = 0, dwMarkID = 0;
        DWORD inviteeCharID = 0; // 需要加入门派的角色
        std::string szMunpaName;
        
        if (munpaID_Ask > 0 && munpaID_Asked == 0) {
            // Ask方有门派（场景1: 门主邀请别人）
            dwMunpaID = munpaID_Ask;
            dwMarkID = mark_Ask;
            szMunpaName = mName_Ask;
            inviteeCharID = charID_Asked;
        } else if (munpaID_Asked > 0 && munpaID_Ask == 0) {
            // Asked方有门派（场景2: 玩家申请加入）
            dwMunpaID = munpaID_Asked;
            dwMarkID = mark_Asked;
            szMunpaName = mName_Asked;
            inviteeCharID = charID_Ask;
        } else {
            LOG("[MunpaHandler] ASKMUNWON_OK: 无法确定门派方和被加入方");
            return;
        }
        
        if (dwMunpaID > 0) {
            BYTE dbRes = 99;
            DWORD dwNewOrder = 6; // 门众
            bool ok = CharacterDB::GetInstance().ExecAddMunwon(dwMunpaID, dwNewOrder, inviteeCharID, dbRes);
            if (ok && dbRes == 0) {
                std::string vizName; BYTE vizType = 0;
                CharacterDB::GetInstance().GetCharVisual(inviteeCharID, vizName, vizType);
                
                // 从数据库读取职务名（GBK 编码，与客户端一致）
                std::string orderName;
                DBHelper::GetInstance().ExecuteQuery(
                    "SELECT szOrderName FROM MUNPA_ORDERTEMPLATE WHERE dwOrderID = " + std::to_string(dwNewOrder),
                    [&](SQLHSTMT hStmt) {
                        char buf[64] = {0}; SQLLEN ind;
                        SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &ind);
                        orderName = buf;
                    });
                
                UpdatePlayerInMemoryGuild(inviteeCharID, dwMunpaID, dwNewOrder, szMunpaName, "", dwMarkID);
                // 广播视觉更新，让周围玩家看到新成员头顶出现门派名
                BroadcastPlayerVisualUpdate(inviteeCharID);
                
                // Send CS_RL_ADDMUNWON_ACK (0x3A2D) to all guild members
                std::vector<BYTE> addAck;
                addAck.push_back(0); // bResult=0 (成功)
                pushDWord(addAck, dwMunpaID);
                pushDWord(addAck, dwNewOrder);
                pushString(addAck, orderName);
                pushDWord(addAck, inviteeCharID + 400000000); // 转为 800M ObjectID
                pushString(addAck, vizName);
                
                BroadcastToMunpa(dwMunpaID, 0x3A2D, addAck);
                
                // 向双方回发 0x3A2B 确认包
                std::vector<BYTE> okAck;
                okAck.push_back(2); // ACT_ASKMUNWON_OK
                pushDWord(okAck, dwAskID);
                pushDWord(okAck, dwAskedID);
                
                SOCKET askSocket = SessionMgr::GetInstance().GetSocketByCharID(charID_Ask);
                if (askSocket != INVALID_SOCKET)
                    SendPacketToSocket(askSocket, 0x3A2B, okAck);
                
                SOCKET askedSocket = SessionMgr::GetInstance().GetSocketByCharID(charID_Asked);
                if (askedSocket != INVALID_SOCKET && askedSocket != askSocket)
                    SendPacketToSocket(askedSocket, 0x3A2B, okAck);
                
                LOG("[MunpaHandler] 加入门派成功: invitee=" + std::to_string(inviteeCharID) + " munpaID=" + std::to_string(dwMunpaID));
            }
        }
    }
}

// 4. CS_RL_DELMUNWON_REQ (0x3A2E = OFFSET_CS_RL + 45)
void OnDelMunwonReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload;
    BYTE* end = payload + size;
    
    DWORD dwCharID = ReadVal<DWORD>(p, end);
    DWORD dwOrderID = ReadVal<DWORD>(p, end);
    
    // 客户端发送的是 800M ObjectID，转为 400M CharID
    DWORD dwTargetCharID = dwCharID;
    if (dwTargetCharID >= 800000000) dwTargetCharID -= 400000000;
    
    LOG("[MunpaHandler] OnDelMunwonReq: TargetCharID=" + std::to_string(dwCharID) + " TargetOrder=" + std::to_string(dwOrderID));
    
    DWORD dwMunpaID = 0, dwMunpaOrder = 0, dwMarkID = 0;
    std::string szMunpaName, szMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMunpaID, dwMunpaOrder, szMunpaName, szMunpaNick, dwMarkID);
    
    if (dwMunpaID == 0) return;
    
    bool authorized = false;
    if (dwTargetCharID == charID) {
        // Voluntary leave: Sect Master cannot leave this way
        if (dwMunpaOrder != 1) authorized = true;
    } else {
        // Kick: Sender must be master/vice master and higher order than target
        if (dwMunpaOrder <= 2 && dwMunpaOrder < dwOrderID) authorized = true;
    }
    
    if (authorized) {
        // Delete member from DB（需要 400M CharID）
        std::string q = "UPDATE CHAR_BASIC SET dwMunpaID = 0, dwMunpaOrder = 0, dateSecede = GETDATE() WHERE dwCharID = " + std::to_string(dwTargetCharID);
        DBHelper::GetInstance().ExecuteUpdate(q);
        
        // Update in-memory player caching
        UpdatePlayerInMemoryGuild(dwTargetCharID, 0, 0, "", "", 0);
        // 广播视觉更新，让周围玩家看到被踢者头顶门派名消失
        BroadcastPlayerVisualUpdate(dwTargetCharID);
        
        // Broadcast CS_RL_DELMUNWON_ACK (0x3A2E) to members of the guild
        // 注意：ACK 中 dwCharID 保留 800M ObjectID 格式，客户端按此比较
        std::vector<BYTE> delAck;
        delAck.push_back(0); // success
        pushDWord(delAck, dwMunpaID);
        pushDWord(delAck, dwOrderID);
        pushDWord(delAck, dwCharID); // 保留原始 800M ObjectID
        
        BroadcastToMunpa(dwMunpaID, 0x3A2F, delAck); // CS_RL_DELMUNWON_ACK = OFFSET_CS_RL + 46
        
        // Send to leaving/kicked player too so their UI cleans up
        SOCKET ks = SessionMgr::GetInstance().GetSocketByCharID(dwTargetCharID);
        if (ks != INVALID_SOCKET) {
            SendPacketToSocket(ks, 0x3A2F, delAck);
        }
        
        LOG("[MunpaHandler] 成员退出/被踢: CharID=" + std::to_string(dwTargetCharID) + " MunpaID=" + std::to_string(dwMunpaID));
    }
}

// 5. CS_RL_CHANGEMUNWONORDER_REQ (0x3A30 = OFFSET_CS_RL + 47)
void OnChangeMunwonOrderReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload;
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
            
            BroadcastToMunpa(dwMunpaID, 0x3A31, ack); // CS_RL_CHANGEMUNWONORDER_ACK = OFFSET_CS_RL + 48
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
            pushDWord(ack, me.dwCharID + 400000000); // 转为 ObjectID（800M 格式）供客户端匹配
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
    BYTE* p = payload;
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
    BYTE* p = payload;
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
    BYTE* p = payload;
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
    BYTE* p = payload;
    BYTE* end = payload + size;
    
    DWORD dwMunpaID = ReadVal<DWORD>(p, end);
    DWORD dwTargetObjID = ReadVal<DWORD>(p, end);  // 800M ObjectID
    std::string szMunpaNick = ReadString(p, end);
    
    // 转换为 400M CharID 用于数据库操作
    DWORD dwTargetCharID = dwTargetObjID;
    if (dwTargetCharID >= 800000000) dwTargetCharID -= 400000000;
    
    LOG("[MunpaHandler] OnMunpaNickReq: TargetCharID=" + std::to_string(dwTargetCharID) + " Nick=" + szMunpaNick);
    
    DWORD dwMyMunpaID = 0, dwMyMunpaOrder = 0, dwMyMarkID = 0;
    std::string szMyMunpaName, szMyMunpaNick;
    CharacterDB::GetInstance().GetCharMunpaInfo(charID, dwMyMunpaID, dwMyMunpaOrder, szMyMunpaName, szMyMunpaNick, dwMyMarkID);
    
    if (dwMyMunpaID > 0 && dwMyMunpaID == dwMunpaID && dwMyMunpaOrder <= 2) { // Master or Vice Master
        BYTE bResult = 99;
        bool ok = CharacterDB::GetInstance().ExecChangeMunpaNick(dwMunpaID, dwTargetCharID, szMunpaNick, bResult);
        LOG("[MunpaHandler] MunpaNick DB result: ok=" + std::to_string(ok) + " bResult=" + std::to_string(bResult));
        if (ok && bResult == 0) {
            // Update target memory cached value
            UpdatePlayerInMemoryGuild(dwTargetCharID, dwMunpaID, 0, szMyMunpaName, szMunpaNick, dwMyMarkID);
            
            std::vector<BYTE> ack;
            ack.push_back(0); // success
            pushDWord(ack, dwMunpaID);
            pushDWord(ack, dwTargetObjID); // 保留 800M ObjectID 给客户端
            pushString(ack, szMunpaNick);
            
            LOG("[MunpaHandler] MunpaNick 广播 ACK: opcode=0x3A3D munpaID=" + std::to_string(dwMunpaID));
            BroadcastToMunpa(dwMunpaID, 0x3A3D, ack); // CS_RL_MUNPANICK_ACK
        }
    } else {
        LOG("[MunpaHandler] MunpaNick 权限不足: MyMunpaID=" + std::to_string(dwMyMunpaID) + " ReqMunpaID=" + std::to_string(dwMunpaID) + " MyOrder=" + std::to_string(dwMyMunpaOrder));
    }
}

// 11. CS_RL_MUNPANOTICE_REQ (0x3A5D)
void OnMunpaNoticeReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    BYTE* p = payload;
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
                
                // 通过门派聊天频道广播公告内容（bType=9 = 门主公告）
                DWORD senderObjID = charID + 400000000; // 转为 800M ObjectID
                std::vector<BYTE> chatAck;
                pushDWord(chatAck, senderObjID);
                chatAck.push_back(9); // bType = 门主公告
                pushString(chatAck, szNotice);
                BroadcastToMunpa(dwMunpaID, 0x3A39, chatAck); // CS_RL_MUNPACHAT_ACK
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
    BYTE* p = payload;
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
    BYTE* p = payload;
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
    RegisterHandler(0x3A2E, [](SOCKET s, BYTE* p, WORD size) { OnDelMunwonReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A30, [](SOCKET s, BYTE* p, WORD size) { OnChangeMunwonOrderReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
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

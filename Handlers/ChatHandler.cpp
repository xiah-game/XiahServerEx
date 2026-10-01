#include "ChatHandler.h"
#include "PartyHandler.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../DB/CharacterDB.h"
#include "../DB/ItemDB.h"
#include "../DBHelper.h"
#include "ItemSerializer.h"
#include <cstring>
#include <cmath>
#include <set>
#include <algorithm>

// 外部声明：RebuildItemHandler.cpp 中已实现的物品刷新函数
void SendItemRefresh(SOCKET clientSocket, DWORD dwItemID, BYTE bSackID, BYTE bSackPos);

// =========================================================
// sString Helper Functions
// =========================================================
static std::string ReadSString(BYTE*& ptr, WORD& remaining) {
    if (remaining < 2) return "";
    WORD len = *(WORD*)ptr;
    ptr += 2;
    remaining -= 2;
    if (len == 0 || len > remaining) {
        if (len > remaining) ptr += remaining;
        remaining = 0;
        return "";
    }
    std::string s((char*)ptr, len);
    ptr += len;
    remaining -= len;
    while (!s.empty() && (s.back() == '\0' || s.back() == '\r' || s.back() == '\n')) {
        s.pop_back();
    }
    return s;
}

static void WriteSString(std::vector<BYTE>& buf, const std::string& s) {
    WORD len = (WORD)s.size();
    buf.push_back(len & 0xFF);
    buf.push_back((len >> 8) & 0xFF);
    buf.insert(buf.end(), s.begin(), s.end());
}

// =========================================================
// Build CS_CH_CHAT_ACK packet
// =========================================================
static std::vector<BYTE> BuildChatAck(
    DWORD dwSenderObjectID,
    BYTE bType,
    const std::string& content,
    const std::string& senderName,
    DWORD dwListenerObjectID = 0,
    const std::string& listenerName = "")
{
    std::vector<BYTE> buf;
    // Header: ID (2) + PayloadSize (2)
    buf.resize(4, 0);

    // sender (DWORD)
    DWORD val = dwSenderObjectID;
    buf.push_back(val & 0xFF); buf.push_back((val >> 8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF); buf.push_back((val >> 24) & 0xFF);

    // type (BYTE)
    buf.push_back(bType);

    // For types other than CT_BATTLE and CT_TIMEMESSAGE, append content + senderName
    if (bType != CT_BATTLE && bType != CT_TIMEMESSAGE) {
        WriteSString(buf, content);
        WriteSString(buf, senderName);
    }

    // For CT_WHISPER, append listener ObjectID + listenerName
    if (bType == CT_WHISPER) {
        val = dwListenerObjectID;
        buf.push_back(val & 0xFF); buf.push_back((val >> 8) & 0xFF);
        buf.push_back((val >> 16) & 0xFF); buf.push_back((val >> 24) & 0xFF);
        WriteSString(buf, listenerName);
    }

    // Fill header
    WORD packetID = CS_CH_CHAT_ACK_ID;
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);

    return buf;
}

// =========================================================
// 构建号角聊天 ACK 包（比普通聊天多 8 字节坐标数据）
// 客户端接收格式: sender(4) + type(1) + content(sString)
//   + senderName(sString) + dwMapID(4) + wPosX(2) + wPosY(2)
// =========================================================
static std::vector<BYTE> BuildSayItemAck(
    DWORD dwSenderObjectID,
    BYTE bType,
    const std::string& content,
    const std::string& senderName,
    DWORD dwMapID,
    WORD wPosX,
    WORD wPosY)
{
    std::vector<BYTE> buf;
    buf.resize(4, 0);

    // sender (DWORD)
    DWORD val = dwSenderObjectID;
    buf.push_back(val & 0xFF); buf.push_back((val >> 8) & 0xFF);
    buf.push_back((val >> 16) & 0xFF); buf.push_back((val >> 24) & 0xFF);

    // type (BYTE)
    buf.push_back(bType);

    // content + senderName (sString)
    WriteSString(buf, content);
    WriteSString(buf, senderName);

    // 号角专属尾部：dwMapID(4) + wPosX(2) + wPosY(2)
    buf.push_back(dwMapID & 0xFF); buf.push_back((dwMapID >> 8) & 0xFF);
    buf.push_back((dwMapID >> 16) & 0xFF); buf.push_back((dwMapID >> 24) & 0xFF);
    buf.push_back(wPosX & 0xFF); buf.push_back((wPosX >> 8) & 0xFF);
    buf.push_back(wPosY & 0xFF); buf.push_back((wPosY >> 8) & 0xFF);

    // Fill header
    WORD packetID = CS_CH_CHAT_ACK_ID;
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);

    return buf;
}

// =========================================================
// 号角聊天类型 → 所需的 nBasicData2 功能编号映射
// =========================================================
static int GetRequiredFunctionID(BYTE chatType) {
    switch (chatType) {
        case CT_SAYITEM_CELL:    return 2;  // 百里传音
        case CT_SAYITEM_MAP:     return 3;  // 千里传音
        case CT_SAYITEM_CHANNEL: return 4;  // 万里传音
        case CT_SAYITEM_GOLD:    return 8;  // 黄金号角
        default: return 0;
    }
}

static bool IsSayItemType(BYTE bType) {
    return bType == CT_SAYITEM_CELL || bType == CT_SAYITEM_MAP ||
           bType == CT_SAYITEM_CHANNEL || bType == CT_SAYITEM_GOLD;
}

// =========================================================
// Get player name from sServerObject
// =========================================================
static std::string GetPlayerName(DWORD dwCharID) {
    for (auto& pair : g_MapInstances) {
        CMapInstance* mapInst = pair.second;
        if (!mapInst) continue;
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        DWORD objectID = dwCharID + 400000000;
        sServerObject* player = mapInst->GetPlayer(objectID);
        if (player) {
            return player->szName;
        }
    }
    return "";
}

// =========================================================
// Find CharID by character name (search online players)
// =========================================================
static DWORD FindCharIDByName(const std::string& name) {
    for (auto& pair : g_MapInstances) {
        CMapInstance* mapInst = pair.second;
        if (!mapInst) continue;
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        auto& players = mapInst->GetPlayers();
        for (auto& pp : players) {
            if (pp.second.szName == name) {
                return pp.second.dwObjectID - 400000000;
            }
        }
    }
    return 0;
}

// =========================================================
// 字符编码转换工具：服务端内部为 UTF-8，客户端网络通信为 GBK (CP936)
// =========================================================
static std::string Utf8ToGbk(const std::string& utf8Str) {
    if (utf8Str.empty()) return "";
    int wlen = MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), (int)utf8Str.size(), NULL, 0);
    if (wlen <= 0) return utf8Str;
    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8Str.c_str(), (int)utf8Str.size(), &wstr[0], wlen);

    int glen = WideCharToMultiByte(936, 0, wstr.c_str(), wlen, NULL, 0, NULL, NULL);
    if (glen <= 0) return utf8Str;
    std::string gbkStr(glen, 0);
    WideCharToMultiByte(936, 0, wstr.c_str(), wlen, &gbkStr[0], glen, NULL, NULL);
    return gbkStr;
}

static std::string GbkToUtf8(const std::string& gbkStr) {
    if (gbkStr.empty()) return "";
    int wlen = MultiByteToWideChar(936, 0, gbkStr.c_str(), (int)gbkStr.size(), NULL, 0);
    if (wlen <= 0) return gbkStr;
    std::wstring wstr(wlen, 0);
    MultiByteToWideChar(936, 0, gbkStr.c_str(), (int)gbkStr.size(), &wstr[0], wlen);

    int ulen = WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen, NULL, 0, NULL, NULL);
    if (ulen <= 0) return gbkStr;
    std::string utf8Str(ulen, 0);
    WideCharToMultiByte(CP_UTF8, 0, wstr.c_str(), wlen, &utf8Str[0], ulen, NULL, NULL);
    return utf8Str;
}

// =========================================================
// 发送私有系统消息到玩家聊天框 (转为 GBK 发送给客户端)
// =========================================================
static void SendSystemChat(SOCKET clientSocket, const std::string& msgUtf8) {
    std::string msgGbk = Utf8ToGbk(msgUtf8);
    std::string senderGbk = Utf8ToGbk("[系统]");
    std::vector<BYTE> ackBuf = BuildChatAck(0, CT_NORMAL, msgGbk, senderGbk);
    EncryptPacket(ackBuf.data(), 0x42);
    SafeSend(clientSocket, (const char*)ackBuf.data(), (int)ackBuf.size(), 0);
}

// =========================================================
// 处理玩家输入的命令（如 /信息）
// =========================================================
static bool HandlePlayerCommand(SOCKET clientSocket, DWORD dwCharID, const std::string& rawCmd) {
    if (rawCmd.empty() || rawCmd[0] != '/') return false;

    // 去除两端可能多余的空格、换行、或客户端尾部多传的 '\0' 终结符
    std::string cleanCmd = rawCmd;
    while (!cleanCmd.empty() && ((unsigned char)cleanCmd.back() == '\0' || cleanCmd.back() == ' ' || cleanCmd.back() == '\r' || cleanCmd.back() == '\n' || cleanCmd.back() == '\t')) {
        cleanCmd.pop_back();
    }

    // 将客户端 GBK 输入转为 UTF-8 便于统一比对
    std::string utf8Cmd = GbkToUtf8(cleanCmd);

    LOG("[CMD] Received rawCmd len=" + std::to_string(rawCmd.size()) +
        " cleanCmd len=" + std::to_string(cleanCmd.size()) +
        " utf8Cmd=" + utf8Cmd);

    // 匹配 /信息 与 /info (支持全等匹配及前缀匹配，兼容 GBK 与 UTF-8)
    bool isInfo = (utf8Cmd == "/信息" || utf8Cmd.find("/信息") == 0 ||
                   utf8Cmd == "/info" || utf8Cmd.find("/info") == 0 ||
                   utf8Cmd == "/INFO" || utf8Cmd.find("/INFO") == 0 ||
                   cleanCmd == "/信息" || cleanCmd.find("/信息") == 0 ||
                   cleanCmd == "/\xd0\xc5\xcf\xa2" || cleanCmd.find("/\xd0\xc5\xcf\xa2") == 0 ||
                   cleanCmd == "/info" || cleanCmd.find("/info") == 0);

    if (isInfo) {
        // 1. 基础倍率
        double expMult = 1.0;
        int vipLevel = CharacterDB::GetInstance().GetVipLevel(dwCharID);
        if (vipLevel >= 1 && vipLevel <= 5) {
            expMult += vipLevel * 0.2;
        }

        // 经验卡加成
        int totalBonusPercent = 0;
        std::string qExpBuff = 
            "SELECT COALESCE(SUM(p.wValue), 0) "
            "FROM CHAR_PREMIUM c "
            "INNER JOIN ITEM_PREMIUM p ON c.wRefID = p.wRefID "
            "WHERE c.dwCharID = " + std::to_string(dwCharID) + " "
            "  AND c.dateEnd > GETDATE() "
            "  AND p.bType IN (0, 1, 8)";
        DBHelper::GetInstance().ExecuteQuery(qExpBuff, [&](SQLHSTMT hStmt) {
            SQLGetData(hStmt, 1, SQL_C_LONG, &totalBonusPercent, 0, NULL);
        });
        if (totalBonusPercent > 0) {
            double cardBonus = (totalBonusPercent / 100.0) - 1.0;
            if (cardBonus > 0.0) {
                expMult += cardBonus;
            }
        }

        // 称号经验加成
        int titleExpBonus = 0;
        std::string qTitleExp = 
            "SELECT COALESCE(SUM(t.wExpPerc), 0) "
            "FROM CHAR_TITLE ct "
            "INNER JOIN TITLE_TEMPLATE t ON ct.dwTitleID = t.dwTitleID "
            "WHERE ct.dwCharID = " + std::to_string(dwCharID) + " AND ct.bActive = 1";
        DBHelper::GetInstance().ExecuteQuery(qTitleExp, [&](SQLHSTMT hStmt) {
            SQLGetData(hStmt, 1, SQL_C_LONG, &titleExpBonus, 0, NULL);
        });
        if (titleExpBonus > 0) {
            expMult += (titleExpBonus / 100.0);
        }

        // 云虎符加成
        int expBonus = ItemDB::GetInstance().GetEquippedItemDataValue(dwCharID, 5, 18);
        if (expBonus > 0) {
            expMult += (expBonus / 100.0);
        }

        // 队伍经验加成
        double partyBonusRatio = 0.0;
        DWORD partyID = PartyManager::GetInstance().GetPartyID(dwCharID);
        if (partyID > 0) {
            DWORD mapID = SessionMgr::GetInstance().GetMapID(clientSocket);
            auto members = PartyManager::GetInstance().GetMembers(partyID);
            std::vector<DWORD> eligibleMembers;
            if (g_MapInstances.count(mapID)) {
                CMapInstance* mapInst = g_MapInstances[mapID];
                std::lock_guard<std::mutex> lock(mapInst->GetMutex());
                PlayerData* myPl = mapInst->GetPlayer(dwCharID + 400000000);
                if (myPl) {
                    for (const auto& m : members) {
                        PlayerData* pl = mapInst->GetPlayer(m.dwCharID + 400000000);
                        if (pl && pl->dwMapID == mapID) {
                            float dx = (float)pl->wPosX - (float)myPl->wPosX;
                            float dy = (float)pl->wPosY - (float)myPl->wPosY;
                            float dist = std::sqrt(dx * dx + dy * dy);
                            if (dist <= 150.0f) {
                                eligibleMembers.push_back(m.dwCharID);
                            }
                        }
                    }
                }
            }
            if (eligibleMembers.size() >= 2) {
                double sizeBonus = (std::min)(0.35, (eligibleMembers.size() - 1) * 0.05);
                std::set<BYTE> activeClasses;
                if (g_MapInstances.count(mapID)) {
                    CMapInstance* mapInst = g_MapInstances[mapID];
                    std::lock_guard<std::mutex> lock(mapInst->GetMutex());
                    for (DWORD memCharID : eligibleMembers) {
                        PlayerData* pl = mapInst->GetPlayer(memCharID + 400000000);
                        if (pl && pl->bPropType >= 1 && pl->bPropType <= 4) {
                            activeClasses.insert(pl->bPropType);
                        }
                    }
                }
                double classBonus = (activeClasses.size() == 4) ? 0.25 : 0.0;
                partyBonusRatio = sizeBonus + classBonus;
            }
        }

        // 2. 总暴率（掉落倍率）
        double dropMult = 1.0;
        if (vipLevel >= 1 && vipLevel <= 5) {
            dropMult += vipLevel * 0.2;
        }

        // 称号掉宝加成
        int titleDropBonus = 0;
        std::string qTitleDrop = 
            "SELECT COALESCE(SUM(t.wDropPerc), 0) "
            "FROM CHAR_TITLE ct "
            "INNER JOIN TITLE_TEMPLATE t ON ct.dwTitleID = t.dwTitleID "
            "WHERE ct.dwCharID = " + std::to_string(dwCharID) + " AND ct.bActive = 1";
        DBHelper::GetInstance().ExecuteQuery(qTitleDrop, [&](SQLHSTMT hStmt) {
            SQLGetData(hStmt, 1, SQL_C_LONG, &titleDropBonus, 0, NULL);
        });
        if (titleDropBonus > 0) {
            dropMult += (titleDropBonus / 100.0);
        }

        if (partyBonusRatio > 0.0) {
            expMult += partyBonusRatio;
        }

        int expPercent = (int)std::round(expMult * 100.0);
        int dropPercent = (int)std::round(dropMult * 100.0);

        std::string expMsg = "经验倍率【" + std::to_string(expPercent) + "%】";
        std::string dropMsg = "总暴率【" + std::to_string(dropPercent) + "%】";

        SendSystemChat(clientSocket, expMsg);
        SendSystemChat(clientSocket, dropMsg);
        return true;
    }

    // 未知命令提示
    SendSystemChat(clientSocket, "未知命令。输入 /信息 可查看当前经验倍率与总暴率。");
    return true;
}

// =========================================================
// OnChatReq - Main chat packet handler
// =========================================================
void OnChatReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize) {
    if (wSize < 5) return; // minimum: type(1) + listener(4)

    BYTE* ptr = pPayload;
    WORD remaining = wSize;

    // Read type (BYTE)
    BYTE bType = *ptr; ptr++; remaining--;

    // Read listener (DWORD)
    DWORD dwListener = *(DWORD*)ptr; ptr += 4; remaining -= 4;

    // Read content (sString)
    std::string content = ReadSString(ptr, remaining);

    // 命令拦截：如果内容以 / 开头，优先由命令处理器响应
    if (!content.empty() && content[0] == '/') {
        if (HandlePlayerCommand(clientSocket, dwCharID, content)) {
            return;
        }
    }

    // 号角聊天的尾部字段与普通聊天不同：
    // 普通聊天: szNickName(sString)
    // 号角聊天: bSackID(BYTE) + bSackPos(BYTE)
    std::string szNickName;
    BYTE hornSackID = 0, hornSackPos = 0;
    if (IsSayItemType(bType)) {
        if (remaining >= 2) {
            hornSackID = *ptr; ptr++; remaining--;
            hornSackPos = *ptr; ptr++; remaining--;
        }
    } else {
        szNickName = ReadSString(ptr, remaining);
    }

    // Sender info
    DWORD dwSenderObjectID = dwCharID + 400000000;
    std::string senderName = GetPlayerName(dwCharID);

    LOG("[CHAT] Type=" + std::to_string(bType) +
        " Sender=" + senderName +
        " (CharID=" + std::to_string(dwCharID) + ")" +
        " Content=" + content);

    switch (bType) {
    // =====================================================
    // CT_NORMAL (0) - Broadcast to all in same map
    // =====================================================
    case CT_NORMAL:
    {
        std::vector<BYTE> ackBuf = BuildChatAck(
            dwSenderObjectID, CT_NORMAL, content, senderName);

        DWORD mapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        auto it = g_MapInstances.find(mapID);
        if (it != g_MapInstances.end() && it->second) {
            CMapInstance* mapInst = it->second;
            // Encrypt before broadcast (BroadcastPacket sends raw)
            EncryptPacket(ackBuf.data(), 0x42);
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* player = mapInst->GetPlayer(dwSenderObjectID);
            if (player) {
                mapInst->BroadcastPacket(ackBuf);
            }
        }
        break;
    }

    // =====================================================
    // CT_WHISPER (1) - Private message to a specific player
    // =====================================================
    case CT_WHISPER:
    {
        DWORD dwTargetCharID = 0;
        std::string targetName;

        if (dwListener != 0) {
            dwTargetCharID = dwListener;
            targetName = GetPlayerName(dwTargetCharID);
        }
        else if (!szNickName.empty()) {
            dwTargetCharID = FindCharIDByName(szNickName);
            targetName = szNickName;
        }

        // Target not found: send ACK with dwListenerObjectID=0 to trigger client-side error display
        if (dwTargetCharID == 0) {
            LOG("[CHAT] Whisper target not found: " + szNickName);
            std::vector<BYTE> failAck = BuildChatAck(
                dwSenderObjectID, CT_WHISPER, content, senderName, 0, szNickName);
            EncryptPacket(failAck.data(), 0x42);
            SafeSend(clientSocket, (const char*)failAck.data(), (int)failAck.size(), 0);
            break;
        }

        DWORD dwTargetObjectID = dwTargetCharID + 400000000;

        // Check target online status
        SOCKET targetSocket = SessionMgr::GetInstance().GetSocketByCharID(dwTargetCharID);
        if (targetSocket == INVALID_SOCKET) {
            // Target exists but offline: send ACK with dwListenerObjectID=0
            LOG("[CHAT] Whisper target offline: " + targetName);
            std::vector<BYTE> failAck = BuildChatAck(
                dwSenderObjectID, CT_WHISPER, content, senderName, 0, targetName);
            EncryptPacket(failAck.data(), 0x42);
            SafeSend(clientSocket, (const char*)failAck.data(), (int)failAck.size(), 0);
            break;
        }

        // Build ACK with whisper fields
        std::vector<BYTE> ackBuf = BuildChatAck(
            dwSenderObjectID, CT_WHISPER, content, senderName,
            dwTargetObjectID, targetName);

        // Send to the target
        std::vector<BYTE> targetCopy = ackBuf;
        EncryptPacket(targetCopy.data(), 0x42);
        SafeSend(targetSocket, (const char*)targetCopy.data(), (int)targetCopy.size(), 0);

        // Also send back to the sender (so they see their own whisper)
        std::vector<BYTE> senderAck = ackBuf;
        EncryptPacket(senderAck.data(), 0x42);
        SafeSend(clientSocket, (const char*)senderAck.data(), (int)senderAck.size(), 0);

        break;
    }

    // =====================================================
    // CT_DAN (5) - Party/Group chat
    // =====================================================
    case CT_DAN:
    {
        DWORD dwPartyID = PartyManager::GetInstance().GetPartyID(dwCharID);
        if (dwPartyID == 0) {
            LOG("[CHAT] Player not in party, CharID=" + std::to_string(dwCharID));
            break;
        }

        auto members = PartyManager::GetInstance().GetMembers(dwPartyID);

        std::vector<BYTE> ackBuf = BuildChatAck(
            dwSenderObjectID, CT_DAN, content, senderName);

        // Send to all party members (including sender)
        for (auto& member : members) {
            SOCKET memberSocket = SessionMgr::GetInstance().GetSocketByCharID(member.dwCharID);
            if (memberSocket != INVALID_SOCKET) {
                std::vector<BYTE> sendBuf = ackBuf;
                EncryptPacket(sendBuf.data(), 0x42);
                SafeSend(memberSocket, (const char*)sendBuf.data(), (int)sendBuf.size(), 0);
            }
        }
        break;
    }

    // =====================================================
    // CT_SAYITEM_CELL/MAP/CHANNEL/GOLD — 号角物品聊天
    // =====================================================
    case CT_SAYITEM_CELL:
    case CT_SAYITEM_MAP:
    case CT_SAYITEM_CHANNEL:
    case CT_SAYITEM_GOLD:
    {
        // 1. 客户端发来的 sackPos 是页内相对偏移，需换算为 DB 中的绝对位置
        //    sackID=1 → 基址 20, sackID=2 → 基址 60, sackID=3 → 基址 100
        BYTE absolutePos = hornSackPos;
        if (hornSackID == 1) absolutePos = 20 + hornSackPos;
        else if (hornSackID == 2) absolutePos = 60 + hornSackPos;
        else if (hornSackID == 3) absolutePos = 100 + hornSackPos;

        DWORD dwHornItemID = ItemDB::GetInstance().GetItemAtSackPos(dwCharID, absolutePos);
        if (dwHornItemID == 0) {
            LOG("[CHAT-HORN] 物品未找到: charID=" + std::to_string(dwCharID) + " sackPos=" + std::to_string(hornSackPos));
            break;
        }

        // 2. 校验物品类型：bType=32 且功能编号匹配
        WORD hornRefID = ItemDB::GetInstance().GetItemRefID(dwHornItemID);
        if (hornRefID == 0 || !g_ItemTemplates.count(hornRefID)) {
            LOG("[CHAT-HORN] 物品模板不存在: refID=" + std::to_string(hornRefID));
            break;
        }
        sItemTemplate& hornTpl = g_ItemTemplates[hornRefID];
        if (hornTpl.bType != 32) {
            LOG("[CHAT-HORN] 物品类型错误: bType=" + std::to_string(hornTpl.bType) + " 期望32");
            break;
        }
        int requiredFunc = GetRequiredFunctionID(bType);
        // ITEM 实例的 nBasicData2 可能为 0（未从模板拷贝），fallback 到模板值
        int actualFunc = ItemDB::GetInstance().GetItemNBasicData(dwHornItemID, 2);
        if (actualFunc == 0) actualFunc = hornTpl.nBasicData2;
        if (actualFunc != requiredFunc) {
            LOG("[CHAT-HORN] 功能编号不匹配: actual=" + std::to_string(actualFunc) + " required=" + std::to_string(requiredFunc));
            break;
        }

        // 3. 检查并扣减耐久 (ITEM.nBasicData3)
        // 如果 ITEM 实例的 nBasicData3=0 但模板有值，先从模板初始化
        int curDur = ItemDB::GetInstance().GetItemNBasicData(dwHornItemID, 3);
        if (curDur == 0 && hornTpl.nBasicData3 > 0) {
            // 首次使用：从模板初始化耐久到 ITEM 实例
            DBHelper::GetInstance().ExecuteUpdate(
                "UPDATE ITEM SET nBasicData3 = " + std::to_string(hornTpl.nBasicData3) +
                " WHERE dwItemID = " + std::to_string(dwHornItemID));
            curDur = hornTpl.nBasicData3;
            LOG("[CHAT-HORN] 首次使用，从模板初始化耐久: " + std::to_string(curDur));
        }
        if (curDur <= 0) {
            LOG("[CHAT-HORN] 耐久已耗尽: itemID=" + std::to_string(dwHornItemID));
            break;
        }
        int newDur = ItemDB::GetInstance().DecrementNBasicData3(dwHornItemID);
        LOG("[CHAT-HORN] 耐久扣减: itemID=" + std::to_string(dwHornItemID) + " " + std::to_string(curDur) + "->" + std::to_string(newDur));

        // 4. 获取发送者位置信息
        DWORD senderMapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        WORD senderPosX = 0, senderPosY = 0;
        if (g_MapInstances.count(senderMapID)) {
            CMapInstance* mapInst = g_MapInstances[senderMapID];
            std::lock_guard<std::mutex> lock(mapInst->GetMutex());
            sServerObject* pObj = mapInst->GetPlayer(dwSenderObjectID);
            if (pObj) {
                senderPosX = pObj->wPosX;
                senderPosY = pObj->wPosY;
            }
        }

        // 5. 构建含坐标的号角 ACK 包
        // sender 字段客户端当作"频道号"显示（如 "1频道"），使用服务端配置的频道 ID
        DWORD channelID = g_Config.cachedChannels.empty() ? 1 : (DWORD)g_Config.cachedChannels[0].id;
        std::vector<BYTE> ackBuf = BuildSayItemAck(
            channelID, bType, content, senderName,
            senderMapID, senderPosX, senderPosY);

        // 6. 按范围广播
        if (bType == CT_SAYITEM_CELL) {
            // 小范围：仅发送者所在地图 AoI 广播
            if (g_MapInstances.count(senderMapID)) {
                CMapInstance* mapInst = g_MapInstances[senderMapID];
                std::vector<BYTE> sendBuf = ackBuf;
                EncryptPacket(sendBuf.data(), 0x42);
                std::lock_guard<std::mutex> lock(mapInst->GetMutex());
                mapInst->BroadcastPacket(sendBuf);
            }
        } else if (bType == CT_SAYITEM_MAP) {
            // 全地图：发送者所在地图所有玩家
            EncryptPacket(ackBuf.data(), 0x42);
            SessionMgr::GetInstance().ForEachSocketInMap(senderMapID, [&](SOCKET s, DWORD) {
                SafeSend(s, (const char*)ackBuf.data(), (int)ackBuf.size(), 0);
            });
        } else {
            // 全频道：CT_SAYITEM_CHANNEL 和 CT_SAYITEM_GOLD 广播到所有在线玩家
            EncryptPacket(ackBuf.data(), 0x42);
            SessionMgr::GetInstance().ForEachSocket([&](SOCKET s) {
                SafeSend(s, (const char*)ackBuf.data(), (int)ackBuf.size(), 0);
            });
        }

        // 7. 耐久刷新 / 销毁
        if (newDur > 0) {
            // 复用已验证的 SendItemRefresh（remove+add 模式）刷新客户端物品 tooltip
            SendItemRefresh(clientSocket, dwHornItemID, hornSackID, hornSackPos);
        } else {
            // 8. 耐久归零 → 销毁物品并通知客户端
            ItemDB::GetInstance().DeleteItemCascade(dwHornItemID);
            std::vector<BYTE> rmBuf(7);
            PACKET_HEADER* rmHead = (PACKET_HEADER*)rmBuf.data();
            rmHead->id = 0x4208;
            rmHead->payloadSize = 3;
            rmBuf[4] = hornSackID;
            rmBuf[5] = hornSackPos;
            rmBuf[6] = 2;
            EncryptPacket(rmBuf.data(), 0x42);
            SafeSend(clientSocket, (const char*)rmBuf.data(), (int)rmBuf.size(), 0);
            LOG("[CHAT-HORN] 号角耐久归零已销毁: itemID=" + std::to_string(dwHornItemID));
        }

        break;
    }

    default:
        LOG("[CHAT] Unhandled chat type: " + std::to_string(bType));
        break;
    }
}


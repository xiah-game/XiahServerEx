#include "ShopHandler.h"
#include "../DBHelper.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../GameObjects/MapInstance.h"
#include "../GameObjects/PlayerManager.h"
#include "../Network/ProtocolConstants.h"
#include <sstream>
#include <iomanip>

extern std::map<DWORD, CMapInstance*> g_MapInstances;

// ============================================================
// Helper Serialization and Broadcast Functions
// ============================================================

static void pushDWord(std::vector<BYTE>& buf, DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back(d >> 24); }
static void pushWord(std::vector<BYTE>& buf, WORD w) { buf.push_back(w & 0xFF); buf.push_back(w >> 8); }
static void pushByte(std::vector<BYTE>& buf, BYTE b) { buf.push_back(b); }
static void pushString(std::vector<BYTE>& buf, const std::string& str) {
    pushWord(buf, (WORD)str.length());
    for (char c : str) buf.push_back(c);
}

// Helper to escape single quotes in SQL to prevent syntax errors / injection
static std::string SQLEscape(const std::string& src) {
    std::string dst;
    for (char c : src) {
        if (c == '\'') dst += "''";
        else dst += c;
    }
    return dst;
}

// Sends a CT_TIMEMESSAGE (type 8) warning directly to player chat window
static void SendSystemWarningChat(SOCKET clientSocket, const std::string& msg) {
    std::vector<BYTE> buf;
    buf.resize(4, 0); // Header placeholder

    // dwSenderObjectID
    pushDWord(buf, 0);

    // type: CT_TIMEMESSAGE (8)
    pushByte(buf, 8);

    // content (sString)
    pushString(buf, msg);

    // Fill Header
    WORD packetID = 0x3E02; // CS_CH_CHAT_ACK
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);

    EncryptPacket(buf.data(), 0x42);
    SafeSend(clientSocket, (const char*)buf.data(), (int)buf.size(), 0);
}

// Serializes standard Xiah GetItemData structure into target byte buffer
static void SerializeItemData(const ItemDB::FullItemRow& row, std::vector<BYTE>& bi) {
    int vis = row.wVisualID, type = row.bType, kind = row.bKind, lvl = row.wLevel, cost = row.dwCost;
    int dat18 = row.nData18, dat19 = row.nData19, dat20 = row.nData20, dat21 = row.nData21, dat25 = row.nData25;
    int refid = row.wRefID, amount = row.wAmount;
    int d[17];
    for (int i = 0; i < 17; i++) d[i] = row.d[i];

    int nd1 = 0, nd2 = 0, nd3 = 0, nd4 = 0, nd5 = 0;
    BYTE charType = 1;
    if (g_ItemTemplates.count(refid)) {
        if (type == 0) type = g_ItemTemplates[refid].bType;
        if (kind == 0) kind = g_ItemTemplates[refid].bKind;
        if (vis == 0) vis = g_ItemTemplates[refid].wVisualID;
        if (lvl == 0) lvl = g_ItemTemplates[refid].wLevel;
        if (cost == 0) cost = g_ItemTemplates[refid].dwCost;
        if (amount == 0) amount = g_ItemTemplates[refid].wAmount;
        charType = g_ItemTemplates[refid].bCharType;
        nd1 = g_ItemTemplates[refid].nBasicData1;
        nd2 = g_ItemTemplates[refid].nBasicData2;
        nd3 = g_ItemTemplates[refid].nBasicData3;
        nd4 = g_ItemTemplates[refid].nBasicData4;
        nd5 = g_ItemTemplates[refid].nBasicData5;
        if (d[0] == -9999) d[0] = g_ItemTemplates[refid].nData1;
        if (d[1] == -9999) d[1] = g_ItemTemplates[refid].nData2;
        if (d[2] == -9999) d[2] = g_ItemTemplates[refid].nData3;
        if (d[3] == -9999) d[3] = g_ItemTemplates[refid].nData4;
        if (d[4] == -9999) d[4] = g_ItemTemplates[refid].nData5;
        if (d[5] == -9999) d[5] = g_ItemTemplates[refid].nData6;
        if (d[6] == -9999) d[6] = g_ItemTemplates[refid].nData7;
        if (d[7] == -9999) d[7] = g_ItemTemplates[refid].nData8;
        if (d[8] == -9999) d[8] = g_ItemTemplates[refid].nData9;
        if (d[9] == -9999) d[9] = g_ItemTemplates[refid].nData10;
    }
    for (int i = 0; i < 17; i++) { if (d[i] == -9999) d[i] = 0; }

    std::string itemName(row.szName);
    if (itemName.empty() && g_ItemTemplates.count(refid)) itemName = g_ItemTemplates[refid].szName;

    pushDWord(bi, row.dwItemID); pushWord(bi, refid);
    pushByte(bi, type); pushByte(bi, kind); pushWord(bi, vis);
    pushString(bi, itemName);
    pushDWord(bi, cost); pushWord(bi, lvl); pushByte(bi, charType);
    pushWord(bi, amount);

    if (type >= 1 && type <= 9) {
        pushWord(bi, nd1); pushWord(bi, nd2); pushWord(bi, nd3); pushWord(bi, nd4); pushWord(bi, nd5);
        pushByte(bi, d[0]);
        pushWord(bi, d[1]); pushWord(bi, d[2]);
        pushWord(bi, d[3]); pushWord(bi, d[4]); pushWord(bi, d[5]); pushWord(bi, d[6]); pushWord(bi, d[7]);
        pushWord(bi, d[8]); pushWord(bi, d[9]); pushWord(bi, d[10]); pushWord(bi, d[11]); pushWord(bi, d[12]);
        pushByte(bi, dat18); pushByte(bi, dat19);
        pushByte(bi, d[13]); pushByte(bi, d[14]); pushByte(bi, d[15]); pushByte(bi, d[16]);
        if (type == 9) {
            pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0);
        } else if (type == 8) {
            for (int i = 0; i < 8; i++) pushByte(bi, 0);
        } else {
            pushByte(bi, 0);
        }
        if (type >= 1 && type <= 4) {
            pushByte(bi, dat19); pushByte(bi, dat20); pushByte(bi, dat21); pushWord(bi, dat25);
        }
    } else {
        switch (type) {
            case 11: case 12: case 13: case 14: case 17:
                pushByte(bi, 0); pushWord(bi, d[1]); pushWord(bi, d[2]); break;
            case 15:
                pushByte(bi, d[0]); pushWord(bi, d[1]); pushWord(bi, d[2]); pushByte(bi, 0); pushByte(bi, 0); break;
            case 16:
                pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); break;
            case 18:
                pushByte(bi, 0); pushDWord(bi, 0); pushWord(bi, d[1]); pushWord(bi, d[2]); pushByte(bi, 0); break;
            case 19:
                pushWord(bi, 0); pushWord(bi, 0); break;
            case 20:
                pushByte(bi, 0); pushDWord(bi, 0); break;
            case 21: {
                DWORD mid = nd2;
                pushWord(bi, lvl); pushDWord(bi, mid); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 1); break;
            }
            case 22:
                pushDWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
            case 23:
                pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
            case 25:
                pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
            case 27:
                pushByte(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
            case 29:
                pushWord(bi, 0); pushWord(bi, 0); break;
            case 32:
                pushWord(bi, 0); pushWord(bi, d[1]); pushWord(bi, d[2]); pushDWord(bi, 0); break;
            case 31:
                pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
            case 34:
                pushDWord(bi, 0); pushByte(bi, 0); break;
        }
    }
}

// Broadcasts shop status and details to other players in the Area of Interest (AoI)
static void BroadcastShopStatusChange(sServerObject* player, BYTE bNewStatus) {
    if (!player) return;
    
    std::vector<BYTE> buf;
    buf.resize(4, 0); // Header placeholder

    // bType (BYTE) -> ACT_SHOPCHANGE = 2
    buf.push_back(2);

    // dwObjID (DWORD)
    DWORD dwObjID = player->dwObjectID;
    pushDWord(buf, dwObjID);

    // dwData1 (DWORD) -> bShopStatus
    pushDWord(buf, (DWORD)bNewStatus);

    // dwData2 (DWORD) -> 0
    pushDWord(buf, 0);

    // dwData3 (DWORD) -> 0
    pushDWord(buf, 0);

    // strTemp1 (sString) -> strShopName
    pushString(buf, player->strShopName);

    // strTemp2 (sString) -> strShopDescription
    pushString(buf, player->strShopDescription);

    // strTemp3 (sString) -> empty
    pushString(buf, "");

    // Fill Header
    WORD packetID = 0x3F10; // CS_CD_CHARUPDATE_ACK
    WORD payloadSize = (WORD)(buf.size() - 4);
    memcpy(&buf[0], &packetID, 2);
    memcpy(&buf[2], &payloadSize, 2);

    EncryptPacket(buf.data(), 0x42);

    // Broadcast to map
    DWORD mapID = player->dwMapID;
    if (g_MapInstances.count(mapID)) {
        CMapInstance* mapInst = g_MapInstances[mapID];
        std::lock_guard<std::mutex> lock(mapInst->GetMutex());
        mapInst->BroadcastPacket(buf); // Broadcast to everyone in map
    }
}

// Standard sString reader (Strips trailing null-terminators to prevent database SQL query truncations)
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
    
    // Strip trailing null-terminator if present (sent by client LPCTSTR serialization)
    if (!s.empty() && s.back() == '\0') {
        s.pop_back();
    }
    return s;
}

// ============================================================
// ShopHandler Namespace Implementations
// ============================================================

namespace ShopHandler {

    void OnOpenShop(SOCKET clientSocket, DWORD charID) {
        LOG("[ShopHandler] OnOpenShop called! charID=" + std::to_string(charID));
        
        DWORD mapID = SessionMgr::GetInstance().GetMapID(clientSocket);
        sServerObject* player = nullptr;
        if (g_MapInstances.count(mapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
            player = g_MapInstances[mapID]->GetPlayer(charID + 400000000);
        }
        
        if (!player) {
            LOG("[ShopHandler] ERROR: player not found in map! charID=" + std::to_string(charID));
            return;
        }

        if (player->bShopStatus == 1) {
            SendSystemWarningChat(clientSocket, "您当前处于营业状态，不能重新开启店铺！");
            return;
        }

        // Fetch or create shop info
        DWORD dwShopID = 0;
        std::string shopName = "";
        std::string shopDesc = "";
        DWORD dwShopMoney = 0;
        bool shopFound = false;

        std::string qSelect = "SELECT dwShopID, szName, szDescription, dwMoney FROM SHOP WHERE dwOwnerID = " + std::to_string(charID) + " AND bKind = 0";
        DBHelper::GetInstance().ExecuteQuery(qSelect, [&](SQLHSTMT hStmt) {
            SQLLEN cb;
            int sID = 0;
            char nameBuf[128] = {0};
            char descBuf[256] = {0};
            SQLGetData(hStmt, 1, SQL_C_SLONG, &sID, 0, &cb); dwShopID = (DWORD)sID;
            SQLGetData(hStmt, 2, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &cb); shopName = nameBuf;
            SQLGetData(hStmt, 3, SQL_C_CHAR, descBuf, sizeof(descBuf), &cb); shopDesc = descBuf;
            SQLGetData(hStmt, 4, SQL_C_ULONG, &dwShopMoney, 0, &cb);
            shopFound = true;
        });

        if (!shopFound) {
            // Create a new shop record
            std::string qInsert = "INSERT INTO SHOP (dwOwnerID, bKind, szName, szDescription, dwMoney) VALUES (" + std::to_string(charID) + ", 0, '', '', 0)";
            DBHelper::GetInstance().ExecuteUpdate(qInsert);
            
            // Fetch the generated shop ID
            DBHelper::GetInstance().ExecuteQuery(qSelect, [&](SQLHSTMT hStmt) {
                SQLLEN cb;
                int sID = 0;
                SQLGetData(hStmt, 1, SQL_C_SLONG, &sID, 0, &cb); dwShopID = (DWORD)sID;
                shopFound = true;
            });
        }

        if (dwShopID == 0) {
            LOG("[ShopHandler] ERROR: Failed to get/create ShopID for charID=" + std::to_string(charID));
            return;
        }

        // Update in-memory player fields
        {
            if (g_MapInstances.count(mapID)) {
                std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
                sServerObject* pObj = g_MapInstances[mapID]->GetPlayer(charID + 400000000);
                if (pObj) {
                    pObj->dwShopID = dwShopID;
                    pObj->strShopName = shopName;
                    pObj->strShopDescription = shopDesc;
                    pObj->bShopStatus = 0; // Starts closed
                }
            }
        }

        // Fetch registered shop items
        struct ShopItemRow {
            BYTE bSackPos;
            DWORD dwItemID;
            DWORD dwPrice;
        };
        std::vector<ShopItemRow> shopItems;
        std::string qItems = "SELECT bSackPos, dwItemID, dwPrice FROM SHOPITEM WHERE dwShopID = " + std::to_string(dwShopID);
        DBHelper::GetInstance().ExecuteQuery(qItems, [&](SQLHSTMT hStmt) {
            ShopItemRow sir; SQLLEN c1, c2, c3;
            int pos = 0;
            SQLGetData(hStmt, 1, SQL_C_SLONG, &pos, 0, &c1); sir.bSackPos = (BYTE)pos;
            SQLGetData(hStmt, 2, SQL_C_ULONG, &sir.dwItemID, 0, &c2);
            SQLGetData(hStmt, 3, SQL_C_ULONG, &sir.dwPrice, 0, &c3);
            shopItems.push_back(sir);
        });

        // Build PKT_SH_SHOPINFO_ACK (0x3101)
        std::vector<BYTE> ackBuf;
        ackBuf.resize(4, 0); // Header placeholder

        pushByte(ackBuf, 0); // bResult (0 = success)
        pushDWord(ackBuf, charID + 400000000); // dwCharID
        pushByte(ackBuf, 0); // bKind (0)
        pushString(ackBuf, shopName);
        pushString(ackBuf, shopDesc);
        pushDWord(ackBuf, dwShopMoney);
        pushWord(ackBuf, (WORD)shopItems.size()); // wRemainShop (count of items currently)
        pushByte(ackBuf, (BYTE)shopItems.size()); // bItemCnt

        // For each item in shop, serialize its item structure + price + rebuild value
        for (auto& item : shopItems) {
            pushByte(ackBuf, item.bSackPos);
            
            // Get full item details
            ItemDB::FullItemRow fRow;
            if (ItemDB::GetInstance().GetFullItemData(item.dwItemID, fRow)) {
                std::vector<BYTE> serialItem;
                SerializeItemData(fRow, serialItem);
                ackBuf.insert(ackBuf.end(), serialItem.begin(), serialItem.end());
                pushDWord(ackBuf, item.dwPrice);
                pushWord(ackBuf, (WORD)fRow.nData20); // wRebuithValue
            } else {
                // Fail-safe empty item data
                LOG("[ShopHandler] Warning: Registered item ID " + std::to_string(item.dwItemID) + " not found in ITEM table!");
                // Pushing dummy zeroes to satisfy size
                std::vector<BYTE> dummy(32, 0);
                ackBuf.insert(ackBuf.end(), dummy.begin(), dummy.end());
                pushDWord(ackBuf, item.dwPrice);
                pushWord(ackBuf, 0);
            }
        }

        // Write Header
        WORD packetID = PKT_SH_SHOPINFO_ACK;
        WORD payloadSize = (WORD)(ackBuf.size() - 4);
        memcpy(&ackBuf[0], &packetID, 2);
        memcpy(&ackBuf[2], &payloadSize, 2);

        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), (int)ackBuf.size(), 0);
        LOG("[ShopHandler] Sent PKT_SH_SHOPINFO_ACK to charID=" + std::to_string(charID) + " item count=" + std::to_string(shopItems.size()));
    }

    void OnSetShopReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
        LOG("[ShopHandler] OnSetShopReq called! charID=" + std::to_string(charID));
        BYTE* ptr = payload;
        WORD remaining = size;

        std::string strName = ReadSString(ptr, remaining);
        std::string strDesc = ReadSString(ptr, remaining);

        DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
        sServerObject* player = nullptr;
        if (g_MapInstances.count(mapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
            player = g_MapInstances[mapID]->GetPlayer(charID + 400000000);
        }

        if (!player || player->dwShopID == 0) {
            LOG("[ShopHandler] OnSetShopReq: Invalid player or ShopID!");
            return;
        }

        // Update database
        std::string qUpdate = "UPDATE SHOP SET szName = '" + SQLEscape(strName) + 
                             "', szDescription = '" + SQLEscape(strDesc) + 
                             "' WHERE dwShopID = " + std::to_string(player->dwShopID) + 
                             " AND dwOwnerID = " + std::to_string(charID);
        DBHelper::GetInstance().ExecuteUpdate(qUpdate);

        // Update in-memory player fields
        {
            if (g_MapInstances.count(mapID)) {
                std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
                sServerObject* pObj = g_MapInstances[mapID]->GetPlayer(charID + 400000000);
                if (pObj) {
                    pObj->strShopName = strName;
                    pObj->strShopDescription = strDesc;
                }
            }
        }

        // Send PKT_SH_SETSHOP_ACK (0x3103)
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = PKT_SH_SETSHOP_ACK;
        head->payloadSize = 1;
        ackBuf[4] = 0; // bResult (0 = success)

        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
        LOG("[ShopHandler] SetShop success. Name=" + strName);
    }

    void OnMoveShopReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
        if (size < 10) return;
        BYTE bSrcSackPos = payload[0];
        DWORD dwSrcItemID = *(DWORD*)(payload + 1);
        BYTE bDesSackPos = payload[5];
        DWORD dwDesItemID = *(DWORD*)(payload + 6);

        LOG("[ShopHandler] OnMoveShopReq: " + std::to_string(bSrcSackPos) + " -> " + std::to_string(bDesSackPos));

        DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
        sServerObject* player = nullptr;
        if (g_MapInstances.count(mapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
            player = g_MapInstances[mapID]->GetPlayer(charID + 400000000);
        }

        if (!player || player->dwShopID == 0) return;

        // Verify source item exists in shop
        bool srcExists = false;
        std::string qCheck = "SELECT 1 FROM SHOPITEM WHERE dwShopID = " + std::to_string(player->dwShopID) + 
                             " AND bSackPos = " + std::to_string(bSrcSackPos) + " AND dwItemID = " + std::to_string(dwSrcItemID);
        DBHelper::GetInstance().ExecuteQuery(qCheck, [&](SQLHSTMT) { srcExists = true; });

        if (!srcExists) {
            // Error
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_MOVESHOP_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 1; // Result: 1 (item not found)
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        // Check if destination is occupied
        bool desExists = false;
        std::string qCheckDes = "SELECT 1 FROM SHOPITEM WHERE dwShopID = " + std::to_string(player->dwShopID) + 
                                " AND bSackPos = " + std::to_string(bDesSackPos);
        DBHelper::GetInstance().ExecuteQuery(qCheckDes, [&](SQLHSTMT) { desExists = true; });

        if (desExists) {
            // Swap: Target pos to temp 99, source to target, temp to source
            DBHelper::GetInstance().ExecuteUpdate("UPDATE SHOPITEM SET bSackPos = 99 WHERE dwShopID = " + std::to_string(player->dwShopID) + " AND bSackPos = " + std::to_string(bDesSackPos));
            DBHelper::GetInstance().ExecuteUpdate("UPDATE SHOPITEM SET bSackPos = " + std::to_string(bDesSackPos) + " WHERE dwShopID = " + std::to_string(player->dwShopID) + " AND bSackPos = " + std::to_string(bSrcSackPos));
            DBHelper::GetInstance().ExecuteUpdate("UPDATE SHOPITEM SET bSackPos = " + std::to_string(bSrcSackPos) + " WHERE dwShopID = " + std::to_string(player->dwShopID) + " AND bSackPos = 99");
        } else {
            // Simple move
            DBHelper::GetInstance().ExecuteUpdate("UPDATE SHOPITEM SET bSackPos = " + std::to_string(bDesSackPos) + " WHERE dwShopID = " + std::to_string(player->dwShopID) + " AND bSackPos = " + std::to_string(bSrcSackPos));
        }

        // Send ACK
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = PKT_SH_MOVESHOP_ACK;
        head->payloadSize = 1;
        ackBuf[4] = 0; // Success
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
    }

    void OnRegShopReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
        if (size < 11) return;
        BYTE bSackID = payload[0];
        BYTE bSackPos = payload[1];
        DWORD dwItemID = *(DWORD*)(payload + 2);
        BYTE bShopSackPos = payload[6];
        DWORD dwPrice = *(DWORD*)(payload + 7);

        LOG("[ShopHandler] OnRegShopReq: pos=" + std::to_string(bSackPos) + " -> shopPos=" + std::to_string(bShopSackPos) + " price=" + std::to_string(dwPrice));

        DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
        sServerObject* player = nullptr;
        if (g_MapInstances.count(mapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
            player = g_MapInstances[mapID]->GetPlayer(charID + 400000000);
        }

        if (!player || player->dwShopID == 0) return;

        // Block item registration while shop is active
        if (player->bShopStatus == 1) {
            SendSystemWarningChat(s, "店铺营业中，请先关闭店铺再上架物品！");
            return;
        }

        // 1. Verify item is not shop token (ID 20272)
        WORD wRefID = ItemDB::GetInstance().GetItemRefID(dwItemID);
        if (wRefID == 20272) {
            // Reject: 4 (Do not sell token)
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_REGSHOP_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 4;
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        // 2. Verify player owns item in backpack
        if (!ItemDB::GetInstance().IsSackItemOwned(charID, dwItemID)) {
            // Reject: 1 (Internal error / not owned)
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_REGSHOP_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 1;
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        // 3. Verify shop target position is unoccupied
        bool occupied = false;
        std::string qOcc = "SELECT 1 FROM SHOPITEM WHERE dwShopID = " + std::to_string(player->dwShopID) + " AND bSackPos = " + std::to_string(bShopSackPos);
        DBHelper::GetInstance().ExecuteQuery(qOcc, [&](SQLHSTMT) { occupied = true; });

        if (occupied) {
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_REGSHOP_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 3; // Occupied
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        // 4. Action: Remove from backpack, Insert into SHOPITEM
        ItemDB::GetInstance().RemoveFromSack(charID, dwItemID);
        
        std::string qInsert = "INSERT INTO SHOPITEM (dwShopID, bSackPos, dwItemID, dwPrice) VALUES (" + 
                             std::to_string(player->dwShopID) + ", " + 
                             std::to_string(bShopSackPos) + ", " + 
                             std::to_string(dwItemID) + ", " + 
                             std::to_string(dwPrice) + ")";
        DBHelper::GetInstance().ExecuteUpdate(qInsert);

        // 5. Send REGSHOP_ACK (0x3107)
        std::vector<BYTE> ackBuf(9);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = PKT_SH_REGSHOP_ACK;
        head->payloadSize = 5;
        ackBuf[4] = 0; // Success
        *(DWORD*)(ackBuf.data() + 5) = dwPrice;
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);

        // 6. Send ADDONSHOP_ACK (0x3112)
        ItemDB::FullItemRow fRow;
        if (ItemDB::GetInstance().GetFullItemData(dwItemID, fRow)) {
            std::vector<BYTE> addBuf;
            addBuf.resize(4, 0); // Header placeholder
            pushByte(addBuf, 1); // Action: 1 = Add/register
            pushByte(addBuf, bShopSackPos);
            pushDWord(addBuf, dwPrice);
            
            std::vector<BYTE> serialItem;
            SerializeItemData(fRow, serialItem);
            addBuf.insert(addBuf.end(), serialItem.begin(), serialItem.end());
            pushWord(addBuf, (WORD)fRow.nData20); // wRebuithValue

            WORD addID = PKT_SH_ADDONSHOP_ACK;
            WORD addPay = (WORD)(addBuf.size() - 4);
            memcpy(&addBuf[0], &addID, 2);
            memcpy(&addBuf[2], &addPay, 2);

            EncryptPacket(addBuf.data(), 0x42);
            SafeSend(s, (const char*)addBuf.data(), addBuf.size(), 0);
        }

        // 7. Send REMOVEFROMSACK_ACK (0x4208) to remove from backpack UI
        std::vector<BYTE> rmBuf(7);
        PACKET_HEADER* rmHead = (PACKET_HEADER*)rmBuf.data();
        rmHead->id = PKT_REMOVEFROMSACK_ACK;
        rmHead->payloadSize = 3;
        rmBuf[4] = bSackID;
        rmBuf[5] = bSackPos;
        rmBuf[6] = 0; // Reason
        EncryptPacket(rmBuf.data(), 0x42);
        SafeSend(s, (const char*)rmBuf.data(), rmBuf.size(), 0);

        // 8. Send SHOPINFOCHANGE_ACK (0x3114)
        DWORD shopMoney = 0;
        DBHelper::GetInstance().ExecuteQuery("SELECT dwMoney FROM SHOP WHERE dwShopID = " + std::to_string(player->dwShopID), [&](SQLHSTMT hStmt) {
            SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_ULONG, &shopMoney, 0, &cb);
        });

        int remainCount = 0;
        DBHelper::GetInstance().ExecuteQuery("SELECT COUNT(*) FROM SHOPITEM WHERE dwShopID = " + std::to_string(player->dwShopID), [&](SQLHSTMT hStmt) {
            SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_SLONG, &remainCount, 0, &cb);
        });

        std::vector<BYTE> chgBuf(10);
        PACKET_HEADER* chgHead = (PACKET_HEADER*)chgBuf.data();
        chgHead->id = PKT_SH_SHOPINFOCHANGE_ACK;
        chgHead->payloadSize = 6;
        *(DWORD*)(chgBuf.data() + 4) = shopMoney;
        *(WORD*)(chgBuf.data() + 8) = (WORD)remainCount;
        EncryptPacket(chgBuf.data(), 0x42);
        SafeSend(s, (const char*)chgBuf.data(), chgBuf.size(), 0);
    }

    void OnDelShopReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
        if (size < 8) return;
        BYTE bShopSackPos = payload[0];
        DWORD dwItemID = *(DWORD*)(payload + 1);
        BYTE bSackID = payload[5];
        BYTE bSackPos = payload[6]; // Auto-find if 255

        LOG("[ShopHandler] OnDelShopReq: shopPos=" + std::to_string(bShopSackPos) + " itemID=" + std::to_string(dwItemID) + " dest=" + std::to_string(bSackID) + ":" + std::to_string(bSackPos));

        DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
        sServerObject* player = nullptr;
        if (g_MapInstances.count(mapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
            player = g_MapInstances[mapID]->GetPlayer(charID + 400000000);
        }

        if (!player || player->dwShopID == 0) return;

        // Block item deletion if shop is currently open/selling
        if (player->bShopStatus == 1) {
            // Reject: 5 (Stall is open/selling)
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_DELSHOP_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 5;
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        // Verify item is actually in the shop at that position
        bool inShop = false;
        std::string qCheck = "SELECT 1 FROM SHOPITEM WHERE dwShopID = " + std::to_string(player->dwShopID) + 
                             " AND bSackPos = " + std::to_string(bShopSackPos) + " AND dwItemID = " + std::to_string(dwItemID);
        DBHelper::GetInstance().ExecuteQuery(qCheck, [&](SQLHSTMT) { inShop = true; });

        if (!inShop) {
            // Reject: 2 (Item not found)
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_DELSHOP_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 2;
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        WORD wRefID = ItemDB::GetInstance().GetItemRefID(dwItemID);
        BYTE bcx = 1, bcy = 1;
        if (g_ItemTemplates.count(wRefID)) {
            bcx = g_ItemTemplates[wRefID].bCX;
            bcy = g_ItemTemplates[wRefID].bCY;
        }
        if (bcx < 1) bcx = 1; if (bcy < 1) bcy = 1;

        // If target position is auto-find
        if (bSackPos == 255) {
            bool foundSpace = false;
            for (BYTE testSack = 1; testSack <= 3; testSack++) {
                BYTE freeSlot = FindFreeSackPos(charID, testSack, bcx, bcy);
                if (freeSlot != 255) {
                    bSackID = testSack;
                    // Absolute sack position returned, convert to sack page relative
                    int startPos = (testSack == 1) ? 20 : (testSack == 2) ? 60 : 100;
                    bSackPos = freeSlot - startPos;
                    foundSpace = true;
                    break;
                }
            }
            if (!foundSpace) {
                // Reject: 3 (Full backpack)
                std::vector<BYTE> ackBuf(5);
                PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
                head->id = PKT_SH_DELSHOP_ACK;
                head->payloadSize = 1;
                ackBuf[4] = 3;
                EncryptPacket(ackBuf.data(), 0x42);
                SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
                return;
            }
        } else {
            // Verify chosen slot is unoccupied and fits
            int absPos = bSackPos + (bSackID == 1 ? 20 : bSackID == 2 ? 60 : 100);
            if (ItemDB::GetInstance().IsSackPosOccupiedAbs(charID, (BYTE)absPos)) {
                // Reject: 3 (Full warehouse/occupied)
                std::vector<BYTE> ackBuf(5);
                PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
                head->id = PKT_SH_DELSHOP_ACK;
                head->payloadSize = 1;
                ackBuf[4] = 3;
                EncryptPacket(ackBuf.data(), 0x42);
                SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
                return;
            }
        }

        // Action: Delete from SHOPITEM, add to SACKITEM
        DBHelper::GetInstance().ExecuteUpdate("DELETE FROM SHOPITEM WHERE dwShopID = " + std::to_string(player->dwShopID) + " AND bSackPos = " + std::to_string(bShopSackPos));
        
        int finalAbsPos = bSackPos + (bSackID == 1 ? 20 : bSackID == 2 ? 60 : 100);
        ItemDB::GetInstance().AddToSack(charID, (BYTE)finalAbsPos, dwItemID);

        // Send DELSHOP_ACK
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = PKT_SH_DELSHOP_ACK;
        head->payloadSize = 1;
        ackBuf[4] = 0; // Success
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);

        // Send REMOVEFROMSHOP_ACK (0x3113)
        std::vector<BYTE> rmBuf(7);
        PACKET_HEADER* rmHead = (PACKET_HEADER*)rmBuf.data();
        rmHead->id = PKT_SH_REMOVEFROMSHOP_ACK;
        rmHead->payloadSize = 3;
        rmBuf[4] = bShopSackPos;
        *(WORD*)(rmBuf.data() + 5) = 1; // amount removed
        EncryptPacket(rmBuf.data(), 0x42);
        SafeSend(s, (const char*)rmBuf.data(), rmBuf.size(), 0);

        // Send ADDONSACK_ACK (0x420A) to refresh client backpack UI
        ItemDB::FullItemRow fRow;
        if (ItemDB::GetInstance().GetFullItemData(dwItemID, fRow)) {
            std::vector<BYTE> addBuf;
            addBuf.resize(4, 0); // Header placeholder
            pushByte(addBuf, bSackID);
            pushByte(addBuf, bSackPos);
            
            std::vector<BYTE> serialItem;
            SerializeItemData(fRow, serialItem);
            addBuf.insert(addBuf.end(), serialItem.begin(), serialItem.end());
            pushWord(addBuf, (WORD)fRow.nData20); // wRebuithValue

            WORD addID = PKT_ADDONSACK_ACK;
            WORD addPay = (WORD)(addBuf.size() - 4);
            memcpy(&addBuf[0], &addID, 2);
            memcpy(&addBuf[2], &addPay, 2);

            EncryptPacket(addBuf.data(), 0x42);
            SafeSend(s, (const char*)addBuf.data(), addBuf.size(), 0);
        }

        // Send SHOPINFOCHANGE_ACK (0x3114)
        DWORD shopMoney = 0;
        DBHelper::GetInstance().ExecuteQuery("SELECT dwMoney FROM SHOP WHERE dwShopID = " + std::to_string(player->dwShopID), [&](SQLHSTMT hStmt) {
            SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_ULONG, &shopMoney, 0, &cb);
        });

        int remainCount = 0;
        DBHelper::GetInstance().ExecuteQuery("SELECT COUNT(*) FROM SHOPITEM WHERE dwShopID = " + std::to_string(player->dwShopID), [&](SQLHSTMT hStmt) {
            SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_SLONG, &remainCount, 0, &cb);
        });

        std::vector<BYTE> chgBuf(10);
        PACKET_HEADER* chgHead = (PACKET_HEADER*)chgBuf.data();
        chgHead->id = PKT_SH_SHOPINFOCHANGE_ACK;
        chgHead->payloadSize = 6;
        *(DWORD*)(chgBuf.data() + 4) = shopMoney;
        *(WORD*)(chgBuf.data() + 8) = (WORD)remainCount;
        EncryptPacket(chgBuf.data(), 0x42);
        SafeSend(s, (const char*)chgBuf.data(), chgBuf.size(), 0);
    }

    void OnStatusChangeReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
        if (size < 1) return;
        BYTE bStatus = payload[0]; // 0=Close, 1=Open/Start

        LOG("[ShopHandler] OnStatusChangeReq: new status=" + std::to_string(bStatus));

        DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
        sServerObject* player = nullptr;
        if (g_MapInstances.count(mapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
            player = g_MapInstances[mapID]->GetPlayer(charID + 400000000);
        }

        if (!player || player->dwShopID == 0) return;

        if (player->bShopStatus == bStatus) {
            // Already in this state
            std::vector<BYTE> ackBuf(6);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_STATUSCHANGE_ACK;
            head->payloadSize = 2;
            ackBuf[4] = 2; // Prevsame
            ackBuf[5] = bStatus;
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        if (bStatus == 1) {
            // Opening stall checks:
            // 1. Must have items registered
            int itemCount = 0;
            DBHelper::GetInstance().ExecuteQuery("SELECT COUNT(*) FROM SHOPITEM WHERE dwShopID = " + std::to_string(player->dwShopID), [&](SQLHSTMT hStmt) {
                SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_SLONG, &itemCount, 0, &cb);
            });

            if (itemCount == 0) {
                // Reject: 3 (No items listed)
                std::vector<BYTE> ackBuf(6);
                PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
                head->id = PKT_SH_STATUSCHANGE_ACK;
                head->payloadSize = 2;
                ackBuf[4] = 3;
                ackBuf[5] = bStatus;
                EncryptPacket(ackBuf.data(), 0x42);
                SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
                return;
            }

            // 2. Map & Coordinate Bounding Box Restrictions
            bool isAllowed = false;
            bool zoneRecordExists = false;
            std::string qZone = "SELECT wPosX1, wPosY1, wPosX2, wPosY2 FROM SHOP_ZONE WHERE dwMapID = " + std::to_string(mapID);
            DBHelper::GetInstance().ExecuteQuery(qZone, [&](SQLHSTMT hStmt) {
                zoneRecordExists = true;
                SQLLEN c[4];
                int x1 = 0, y1 = 0, x2 = 0, y2 = 0;
                SQLGetData(hStmt, 1, SQL_C_SLONG, &x1, 0, &c[0]);
                SQLGetData(hStmt, 2, SQL_C_SLONG, &y1, 0, &c[1]);
                SQLGetData(hStmt, 3, SQL_C_SLONG, &x2, 0, &c[2]);
                SQLGetData(hStmt, 4, SQL_C_SLONG, &y2, 0, &c[3]);
                
                // If coordinate boundaries are 0, it means entire map is allowed
                if (x1 == 0 && y1 == 0 && x2 == 0 && y2 == 0) {
                    isAllowed = true;
                } else {
                    // Check if player is within coordinate box
                    if (player->wPosX >= x1 && player->wPosX <= x2 && player->wPosY >= y1 && player->wPosY <= y2) {
                        isAllowed = true;
                    }
                }
            });

            // Fallback map restriction: If zone table does not have records, enforce default ordinary map restrictions (Map 1~16 except 10)
            if (!zoneRecordExists) {
                if (mapID >= 1 && mapID <= 16 && mapID != 10) {
                    isAllowed = true;
                }
            }

            if (!isAllowed) {
                // Reject: 7 (Zone restriction not allowed)
                std::vector<BYTE> ackBuf(6);
                PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
                head->id = PKT_SH_STATUSCHANGE_ACK;
                head->payloadSize = 2;
                ackBuf[4] = 7;
                ackBuf[5] = bStatus;
                EncryptPacket(ackBuf.data(), 0x42);
                SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
                
                SendSystemWarningChat(s, "[Shop] Stall setup is not allowed in this map or zone!");
                return;
            }
        }

        // Action: Update state
        {
            if (g_MapInstances.count(mapID)) {
                std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
                sServerObject* pObj = g_MapInstances[mapID]->GetPlayer(charID + 400000000);
                if (pObj) {
                    pObj->bShopStatus = bStatus;
                }
            }
        }

        // Send STATUSCHANGE_ACK (0x310B)
        std::vector<BYTE> ackBuf(6);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = PKT_SH_STATUSCHANGE_ACK;
        head->payloadSize = 2;
        ackBuf[4] = 0; // Success
        ackBuf[5] = bStatus; // Status (0 = Close, 1 = Open)
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);

        // Broadcast shop state changes to all players in the AoI via 0x3F10 CHARUPDATE_ACK
        BroadcastShopStatusChange(player, bStatus);
        LOG("[ShopHandler] StatusChanged to " + std::to_string(bStatus) + " and broadcasted successfully.");
    }

    void OnGetMoneyReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
        if (size < 4) return;
        DWORD dwRequestMoney = *(DWORD*)payload;

        LOG("[ShopHandler] OnGetMoneyReq: requested=" + std::to_string(dwRequestMoney));

        DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
        sServerObject* player = nullptr;
        if (g_MapInstances.count(mapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
            player = g_MapInstances[mapID]->GetPlayer(charID + 400000000);
        }

        if (!player || player->dwShopID == 0) return;

        // Fetch current shop balance
        DWORD dwShopMoney = 0;
        DBHelper::GetInstance().ExecuteQuery("SELECT dwMoney FROM SHOP WHERE dwShopID = " + std::to_string(player->dwShopID), [&](SQLHSTMT hStmt) {
            SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_ULONG, &dwShopMoney, 0, &cb);
        });

        if (dwRequestMoney > dwShopMoney) {
            // Reject: 1 (requested money exceeds stall balance)
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_GETMONEY_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 1;
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        // Action: Deduct from SHOP, add to CHAR_DATA
        DBHelper::GetInstance().ExecuteUpdate("UPDATE SHOP SET dwMoney = dwMoney - " + std::to_string(dwRequestMoney) + " WHERE dwShopID = " + std::to_string(player->dwShopID));
        CharacterDB::GetInstance().AddMoney(charID, dwRequestMoney);

        // Fetch buyer's updated backpack money & send PKT_CHAREXP_ACK (0x3B13) to refresh currency UI
        INT64 currentMoney = CharacterDB::GetInstance().GetMoney(charID);
        std::vector<BYTE> moneyBuf(13);
        PACKET_HEADER* mHead = (PACKET_HEADER*)moneyBuf.data();
        mHead->id = 0x3B13; // CS_IF_CHAREXP_ACK / Money refresh
        mHead->payloadSize = 9;
        *((INT64*)(moneyBuf.data() + 4)) = (INT64)currentMoney;
        moneyBuf[12] = 0; // Reason
        EncryptPacket(moneyBuf.data(), 0x42);
        SafeSend(s, (const char*)moneyBuf.data(), moneyBuf.size(), 0);

        // Send GETMONEY_ACK (0x310D)
        std::vector<BYTE> ackBuf(9);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = PKT_SH_GETMONEY_ACK;
        head->payloadSize = 5;
        ackBuf[4] = 0; // Success
        *(DWORD*)(ackBuf.data() + 5) = dwRequestMoney;
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);

        // Send SHOPINFOCHANGE_ACK (0x3114)
        DWORD newShopMoney = dwShopMoney - dwRequestMoney;
        int remainCount = 0;
        DBHelper::GetInstance().ExecuteQuery("SELECT COUNT(*) FROM SHOPITEM WHERE dwShopID = " + std::to_string(player->dwShopID), [&](SQLHSTMT hStmt) {
            SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_SLONG, &remainCount, 0, &cb);
        });

        std::vector<BYTE> chgBuf(10);
        PACKET_HEADER* chgHead = (PACKET_HEADER*)chgBuf.data();
        chgHead->id = PKT_SH_SHOPINFOCHANGE_ACK;
        chgHead->payloadSize = 6;
        *(DWORD*)(chgBuf.data() + 4) = newShopMoney;
        *(WORD*)(chgBuf.data() + 8) = (WORD)remainCount;
        EncryptPacket(chgBuf.data(), 0x42);
        SafeSend(s, (const char*)chgBuf.data(), chgBuf.size(), 0);
    }

    void OnGetShopInfoReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
        if (size < 4) return;
        DWORD dwTargetObjectID = *(DWORD*)payload;

        LOG("[ShopHandler] OnGetShopInfoReq: targetObjID=" + std::to_string(dwTargetObjectID));

        DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
        sServerObject* targetPlayer = nullptr;
        if (g_MapInstances.count(mapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
            targetPlayer = g_MapInstances[mapID]->GetPlayer(dwTargetObjectID);
        }

        if (!targetPlayer || targetPlayer->bShopStatus != 1) {
            // Target is offline or not selling
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_GETSHOPINFO_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 1; // Error
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        DWORD targetCharID = dwTargetObjectID - 400000000;

        // Retrieve shop info from DB
        DWORD dwShopID = 0;
        std::string shopName = "";
        std::string shopDesc = "";
        bool shopFound = false;

        std::string qSelect = "SELECT dwShopID, szName, szDescription FROM SHOP WHERE dwOwnerID = " + std::to_string(targetCharID) + " AND bKind = 0";
        DBHelper::GetInstance().ExecuteQuery(qSelect, [&](SQLHSTMT hStmt) {
            SQLLEN cb;
            int sID = 0;
            char nameBuf[128] = {0};
            char descBuf[256] = {0};
            SQLGetData(hStmt, 1, SQL_C_SLONG, &sID, 0, &cb); dwShopID = (DWORD)sID;
            SQLGetData(hStmt, 2, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &cb); shopName = nameBuf;
            SQLGetData(hStmt, 3, SQL_C_CHAR, descBuf, sizeof(descBuf), &cb); shopDesc = descBuf;
            shopFound = true;
        });

        if (!shopFound || dwShopID == 0) {
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_GETSHOPINFO_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 1;
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        // Fetch registered shop items
        struct ShopItemRow {
            BYTE bSackPos;
            DWORD dwItemID;
            DWORD dwPrice;
        };
        std::vector<ShopItemRow> shopItems;
        std::string qItems = "SELECT bSackPos, dwItemID, dwPrice FROM SHOPITEM WHERE dwShopID = " + std::to_string(dwShopID);
        DBHelper::GetInstance().ExecuteQuery(qItems, [&](SQLHSTMT hStmt) {
            ShopItemRow sir; SQLLEN c1, c2, c3;
            int pos = 0;
            SQLGetData(hStmt, 1, SQL_C_SLONG, &pos, 0, &c1); sir.bSackPos = (BYTE)pos;
            SQLGetData(hStmt, 2, SQL_C_ULONG, &sir.dwItemID, 0, &c2);
            SQLGetData(hStmt, 3, SQL_C_ULONG, &sir.dwPrice, 0, &c3);
            shopItems.push_back(sir);
        });

        // Build GETSHOPINFO_ACK (0x310F)
        std::vector<BYTE> ackBuf;
        ackBuf.resize(4, 0); // Header placeholder

        pushByte(ackBuf, 0); // bResult: 0 = SUCCESS
        pushDWord(ackBuf, dwTargetObjectID); // dwCharID
        pushByte(ackBuf, (BYTE)shopItems.size()); // bItemCnt

        for (auto& item : shopItems) {
            pushByte(ackBuf, item.bSackPos);
            
            ItemDB::FullItemRow fRow;
            if (ItemDB::GetInstance().GetFullItemData(item.dwItemID, fRow)) {
                std::vector<BYTE> serialItem;
                SerializeItemData(fRow, serialItem);
                ackBuf.insert(ackBuf.end(), serialItem.begin(), serialItem.end());
                pushDWord(ackBuf, item.dwPrice);
            } else {
                std::vector<BYTE> dummy(32, 0);
                ackBuf.insert(ackBuf.end(), dummy.begin(), dummy.end());
                pushDWord(ackBuf, item.dwPrice);
            }
        }

        WORD packID = PKT_SH_GETSHOPINFO_ACK;
        WORD payLen = (WORD)(ackBuf.size() - 4);
        memcpy(&ackBuf[0], &packID, 2);
        memcpy(&ackBuf[2], &payLen, 2);

        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
    }

    void OnBuyPcShopReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
        if (size < 9) return;
        DWORD dwTargetObjectID = *(DWORD*)payload;
        BYTE bShopSackPos = payload[4];
        DWORD dwItemID = *(DWORD*)(payload + 5);

        LOG("[ShopHandler] OnBuyPcShopReq: sellerObjID=" + std::to_string(dwTargetObjectID) + " pos=" + std::to_string(bShopSackPos) + " itemID=" + std::to_string(dwItemID));

        DWORD mapID = SessionMgr::GetInstance().GetMapID(s);
        sServerObject* sellerPlayer = nullptr;
        if (g_MapInstances.count(mapID)) {
            std::lock_guard<std::mutex> lock(g_MapInstances[mapID]->GetMutex());
            sellerPlayer = g_MapInstances[mapID]->GetPlayer(dwTargetObjectID);
        }

        if (!sellerPlayer || sellerPlayer->bShopStatus != 1 || sellerPlayer->dwShopID == 0) {
            // Seller is offline/not open
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_BUYPCSHOP_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 1; // Seller offline / shop closed
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        DWORD sellerCharID = dwTargetObjectID - 400000000;

        // 1. Verify item exists and fetch price
        DWORD dwPrice = 0;
        bool exists = false;
        std::string qCheck = "SELECT dwPrice FROM SHOPITEM WHERE dwShopID = " + std::to_string(sellerPlayer->dwShopID) + 
                             " AND bSackPos = " + std::to_string(bShopSackPos) + " AND dwItemID = " + std::to_string(dwItemID);
        DBHelper::GetInstance().ExecuteQuery(qCheck, [&](SQLHSTMT hStmt) {
            SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_ULONG, &dwPrice, 0, &cb);
            exists = true;
        });

        if (!exists) {
            // Reject: 2 (Item not found or already sold)
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_BUYPCSHOP_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 2;
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        // 2. Verify buyer money
        INT64 buyerMoney = CharacterDB::GetInstance().GetMoney(charID);
        if (buyerMoney < (INT64)dwPrice) {
            // Reject: 3 (Not enough money)
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_BUYPCSHOP_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 3;
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        // 3. Find free sack position in buyer backpack
        WORD wRefID = ItemDB::GetInstance().GetItemRefID(dwItemID);
        BYTE bcx = 1, bcy = 1;
        if (g_ItemTemplates.count(wRefID)) {
            bcx = g_ItemTemplates[wRefID].bCX;
            bcy = g_ItemTemplates[wRefID].bCY;
        }
        if (bcx < 1) bcx = 1; if (bcy < 1) bcy = 1;

        BYTE targetSackID = 255;
        BYTE targetSackPos = 255;
        bool foundSpace = false;
        for (BYTE testSack = 1; testSack <= 3; testSack++) {
            BYTE freeSlot = FindFreeSackPos(charID, testSack, bcx, bcy);
            if (freeSlot != 255) {
                targetSackID = testSack;
                int startPos = (testSack == 1) ? 20 : (testSack == 2) ? 60 : 100;
                targetSackPos = freeSlot - startPos;
                foundSpace = true;
                break;
            }
        }

        if (!foundSpace) {
            // Reject: 4 (Full inventory)
            std::vector<BYTE> ackBuf(5);
            PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
            head->id = PKT_SH_BUYPCSHOP_ACK;
            head->payloadSize = 1;
            ackBuf[4] = 4;
            EncryptPacket(ackBuf.data(), 0x42);
            SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);
            return;
        }

        // ============================================================
        // ATOMIC TRANSACTION LOGIC
        // ============================================================
        
        // A. Deduct money from buyer
        CharacterDB::GetInstance().SubtractMoney(charID, dwPrice);
        
        // B. Add money to seller's SHOP earnings
        std::string qAdd = "UPDATE SHOP SET dwMoney = dwMoney + " + std::to_string(dwPrice) + " WHERE dwShopID = " + std::to_string(sellerPlayer->dwShopID);
        DBHelper::GetInstance().ExecuteUpdate(qAdd);

        // C. Delete item from SHOPITEM
        std::string qDel = "DELETE FROM SHOPITEM WHERE dwShopID = " + std::to_string(sellerPlayer->dwShopID) + 
                           " AND bSackPos = " + std::to_string(bShopSackPos) + " AND dwItemID = " + std::to_string(dwItemID);
        DBHelper::GetInstance().ExecuteUpdate(qDel);

        // D. Add item to buyer's backpack (updates SACKITEM)
        int finalAbsPos = targetSackPos + (targetSackID == 1 ? 20 : targetSackID == 2 ? 60 : 100);
        ItemDB::GetInstance().AddToSack(charID, (BYTE)finalAbsPos, dwItemID);

        // ============================================================
        // NETWORK SYNCHRONIZATION
        // ============================================================

        // 1. Send BUYPCSHOP_ACK (0x3111) to buyer
        std::vector<BYTE> ackBuf(5);
        PACKET_HEADER* head = (PACKET_HEADER*)ackBuf.data();
        head->id = PKT_SH_BUYPCSHOP_ACK;
        head->payloadSize = 1;
        ackBuf[4] = 0; // Success
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(s, (const char*)ackBuf.data(), ackBuf.size(), 0);

        // 2. Send ADDONSACK_ACK (0x420A) to buyer client to draw the item in backpack UI
        ItemDB::FullItemRow fRow;
        if (ItemDB::GetInstance().GetFullItemData(dwItemID, fRow)) {
            std::vector<BYTE> addBuf;
            addBuf.resize(4, 0);
            pushByte(addBuf, targetSackID);
            pushByte(addBuf, targetSackPos);
            
            std::vector<BYTE> serialItem;
            SerializeItemData(fRow, serialItem);
            addBuf.insert(addBuf.end(), serialItem.begin(), serialItem.end());
            pushWord(addBuf, (WORD)fRow.nData20);

            WORD addID = PKT_ADDONSACK_ACK;
            WORD addPay = (WORD)(addBuf.size() - 4);
            memcpy(&addBuf[0], &addID, 2);
            memcpy(&addBuf[2], &addPay, 2);

            EncryptPacket(addBuf.data(), 0x42);
            SafeSend(s, (const char*)addBuf.data(), addBuf.size(), 0);
        }

        // 3. Send PKT_CHAREXP_ACK (0x3B13) money update to buyer to refresh currency UI
        INT64 currentMoney = CharacterDB::GetInstance().GetMoney(charID);
        std::vector<BYTE> moneyBuf(13);
        PACKET_HEADER* mHead = (PACKET_HEADER*)moneyBuf.data();
        mHead->id = 0x3B13;
        mHead->payloadSize = 9;
        *((INT64*)(moneyBuf.data() + 4)) = (INT64)currentMoney;
        moneyBuf[12] = 0;
        EncryptPacket(moneyBuf.data(), 0x42);
        SafeSend(s, (const char*)moneyBuf.data(), moneyBuf.size(), 0);

        // 4. If the seller is online, push shop updates to seller socket
        SOCKET sellerSocket = SessionMgr::GetInstance().GetSocketByCharID(sellerCharID);
        if (sellerSocket != INVALID_SOCKET) {
            // A. Send REMOVEFROMSHOP_ACK (0x3113) to seller
            std::vector<BYTE> rmBuf(7);
            PACKET_HEADER* rmHead = (PACKET_HEADER*)rmBuf.data();
            rmHead->id = PKT_SH_REMOVEFROMSHOP_ACK;
            rmHead->payloadSize = 3;
            rmBuf[4] = bShopSackPos;
            *(WORD*)(rmBuf.data() + 5) = 1; // amount removed
            EncryptPacket(rmBuf.data(), 0x42);
            SafeSend(sellerSocket, (const char*)rmBuf.data(), rmBuf.size(), 0);

            // B. Send SHOPINFOCHANGE_ACK (0x3114) to seller (updated earnings & count)
            DWORD newSellerShopMoney = 0;
            DBHelper::GetInstance().ExecuteQuery("SELECT dwMoney FROM SHOP WHERE dwShopID = " + std::to_string(sellerPlayer->dwShopID), [&](SQLHSTMT hStmt) {
                SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_ULONG, &newSellerShopMoney, 0, &cb);
            });

            int remainCount = 0;
            DBHelper::GetInstance().ExecuteQuery("SELECT COUNT(*) FROM SHOPITEM WHERE dwShopID = " + std::to_string(sellerPlayer->dwShopID), [&](SQLHSTMT hStmt) {
                SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_SLONG, &remainCount, 0, &cb);
            });

            std::vector<BYTE> chgBuf(10);
            PACKET_HEADER* chgHead = (PACKET_HEADER*)chgBuf.data();
            chgHead->id = PKT_SH_SHOPINFOCHANGE_ACK;
            chgHead->payloadSize = 6;
            *(DWORD*)(chgBuf.data() + 4) = newSellerShopMoney;
            *(WORD*)(chgBuf.data() + 8) = (WORD)remainCount;
            EncryptPacket(chgBuf.data(), 0x42);
            SafeSend(sellerSocket, (const char*)chgBuf.data(), chgBuf.size(), 0);

            // C. Push chat message notification to seller
            std::string itemName(fRow.szName);
            if (itemName.empty() && g_ItemTemplates.count(wRefID)) itemName = g_ItemTemplates[wRefID].szName;
            
            std::stringstream ssNotice;
            ssNotice << "[Shop] Player bought your [" << itemName << "]! Earned: " << dwPrice << " Money.";
            SendSystemWarningChat(sellerSocket, ssNotice.str());
        }

        LOG("[ShopHandler] BuyPcShop completed successfully! Buyer=" + std::to_string(charID) + " Seller=" + std::to_string(sellerCharID));
    }
}

// ============================================================
// Handler Registration
// ============================================================

void RegisterShopHandlers() {
    LOG("[ShopHandler] Registering Personal Shop handlers (CS_SH family offset 0x3101)...");
    
    RegisterHandler(0x3102, [](SOCKET s, BYTE* p, WORD sz) { ShopHandler::OnSetShopReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x3104, [](SOCKET s, BYTE* p, WORD sz) { ShopHandler::OnMoveShopReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x3106, [](SOCKET s, BYTE* p, WORD sz) { ShopHandler::OnRegShopReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x3108, [](SOCKET s, BYTE* p, WORD sz) { ShopHandler::OnDelShopReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x310A, [](SOCKET s, BYTE* p, WORD sz) { ShopHandler::OnStatusChangeReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x310C, [](SOCKET s, BYTE* p, WORD sz) { ShopHandler::OnGetMoneyReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x310E, [](SOCKET s, BYTE* p, WORD sz) { ShopHandler::OnGetShopInfoReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });
    RegisterHandler(0x3110, [](SOCKET s, BYTE* p, WORD sz) { ShopHandler::OnBuyPcShopReq(s, SessionMgr::GetInstance().GetCharID(s), p, sz); });

    LOG("[ShopHandler] Personal Shop handlers successfully hooked!");
}

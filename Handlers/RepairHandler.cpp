// ============================================================
// RepairHandler.cpp — Equipment Repair via NPC or Repair Item
// ============================================================
// CS_IM_REPAIRITEM_REQ  (0x4228) = NPC-based repair
// CS_IM_REPAIRITEM_ACK  (0x4229) = result: bResult(1) + dwMoney(4)
// CS_IM_DURABILITY_ACK  (0x4236) = durability update
//
// CS_IM_REPAIRWITHITEM_REQ (0x4248) = item-based repair
// CS_IM_REPAIRWITHITEM_ACK (0x4249) = result: bResult(1) + dwMoney(4)
//
// Repair cost formula (from client): Price * 0.1 * ((MaxDur - CurDur) / MaxDur)
//
// DB mapping:
//   nData2 in ITEMDATA = wCurDur (current durability)
//   nData3 in ITEMDATA = wMaxDur (max durability)
//   ITEMTEMPLATE.nBasicData1 = base max durability for new items
// ============================================================

#include "RepairHandler.h"
#include "../Network/ProtocolConstants.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../ServerCore.h"
#include "../DB/ItemDB.h"
#include "../DB/CharacterDB.h"
#include "../DBHelper.h"
#include "../GameObjects/PlayerManager.h"

#include <cmath>

extern std::map<WORD, sItemTemplate> g_ItemTemplates;

static void pushByte(std::vector<BYTE>& buf, BYTE b) { buf.push_back(b); }
static void pushWord(std::vector<BYTE>& buf, WORD w) { buf.push_back(w & 0xFF); buf.push_back((w >> 8) & 0xFF); }
static void pushDWord(std::vector<BYTE>& buf, DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back(d >> 24); }

// Forward declaration
void SendCharStatusInfoAck(SOCKET clientSocket, DWORD dwCharID, WORD opCode);

// ============================================================
// Packet IDs
// ============================================================
constexpr WORD PKT_REPAIRITEM_REQ       = OFFSET_CS_IM + 39;  // 0x4228
constexpr WORD PKT_REPAIRITEM_ACK       = OFFSET_CS_IM + 40;  // 0x4229
constexpr WORD PKT_DURABILITY_ACK       = OFFSET_CS_IM + 35;  // 0x4236
constexpr WORD PKT_REPAIRWITHITEM_REQ   = OFFSET_CS_IM + 70;  // 0x4247
constexpr WORD PKT_REPAIRWITHITEM_ACK   = OFFSET_CS_IM + 71;  // 0x4248

// ============================================================
// Error codes (matching client csprotocol.h)
// ============================================================
constexpr BYTE ERR_REPAIR_SUCCESS           = 0;
constexpr BYTE ERR_REPAIR_NOTFOUND          = 1;
constexpr BYTE ERR_REPAIR_NOMONEY           = 2;
constexpr BYTE ERR_REPAIR_FULLDURABILITY    = 3;
constexpr BYTE ERR_REPAIR_DBOPEN            = 4;
constexpr BYTE ERR_REPAIR_NOTREPAIR         = 5;   // Cannot repair (ice item etc.)
constexpr BYTE ERR_REPAIR_NOTREPAIRITEM     = 9;   // Not a repairable item type

// RepairWithItem error codes
constexpr BYTE ERR_RWITH_SUCCESS            = 0;
constexpr BYTE ERR_RWITH_NOTFOUND           = 1;
constexpr BYTE ERR_RWITH_NOTFOUNDRES        = 2;
constexpr BYTE ERR_RWITH_INVALIDRES         = 3;
constexpr BYTE ERR_RWITH_NOMONEY            = 4;
constexpr BYTE ERR_RWITH_FULLDURABILITY     = 5;
constexpr BYTE ERR_RWITH_DBOPEN             = 6;
constexpr BYTE ERR_RWITH_FAIL               = 7;

// ============================================================
// Helper: Send REPAIRITEM_ACK
// ============================================================
static void SendRepairItemAck(SOCKET s, BYTE bResult, DWORD dwCost) {
    std::vector<BYTE> buf;
    buf.resize(4); // header
    pushByte(buf, bResult);
    pushDWord(buf, dwCost);
    
    PACKET_HEADER* hdr = (PACKET_HEADER*)buf.data();
    hdr->id = PKT_REPAIRITEM_ACK;
    hdr->payloadSize = (WORD)(buf.size() - 4);
    EncryptPacket(buf.data(), ENCRYPT_KEY);
    SafeSend(s, (const char*)buf.data(), (int)buf.size(), 0);
}

// ============================================================
// Helper: Send REPAIRWITHITEM_ACK
// ============================================================
static void SendRepairWithItemAck(SOCKET s, BYTE bResult, DWORD dwCost) {
    std::vector<BYTE> buf;
    buf.resize(4); // header
    pushByte(buf, bResult);
    pushDWord(buf, dwCost);
    
    PACKET_HEADER* hdr = (PACKET_HEADER*)buf.data();
    hdr->id = PKT_REPAIRWITHITEM_ACK;
    hdr->payloadSize = (WORD)(buf.size() - 4);
    EncryptPacket(buf.data(), ENCRYPT_KEY);
    SafeSend(s, (const char*)buf.data(), (int)buf.size(), 0);
}

// ============================================================
// Helper: Send DURABILITY_ACK to update client's item durability
// Payload: dwItemID(4) + bItemType(1) + bSackID(1) + bSackPos(1) + wCurDur(2)
// ============================================================
static void SendDurabilityAck(SOCKET s, DWORD dwItemID, BYTE bItemType, BYTE bSackID, BYTE bSackPos, WORD wCurDur) {
    std::vector<BYTE> buf;
    buf.resize(4); // header
    pushDWord(buf, dwItemID);
    pushByte(buf, bItemType);
    pushByte(buf, bSackID);
    pushByte(buf, bSackPos);
    pushWord(buf, wCurDur);
    
    PACKET_HEADER* hdr = (PACKET_HEADER*)buf.data();
    hdr->id = PKT_DURABILITY_ACK;
    hdr->payloadSize = (WORD)(buf.size() - 4);
    EncryptPacket(buf.data(), ENCRYPT_KEY);
    SafeSend(s, (const char*)buf.data(), (int)buf.size(), 0);
}

// ============================================================
// Calculate repair cost
// Formula from client: Price * 0.1 * ((MaxDur - CurDur) / MaxDur)
// ============================================================
static DWORD CalcRepairCost(DWORD dwPrice, WORD wMaxDur, WORD wCurDur) {
    if (wMaxDur == 0) return 0;
    double ratio = (double)(wMaxDur - wCurDur) / (double)wMaxDur;
    DWORD cost = (DWORD)(dwPrice * 0.1 * ratio);
    if (cost == 0 && wCurDur < wMaxDur) cost = 1; // minimum 1 if damaged
    return cost;
}

// ============================================================
// Handler: CS_IM_REPAIRITEM_REQ (NPC-based repair)
// Payload: dwItemID(4) + bSackID(1) + bSackPos(1)
// ============================================================
static void OnRepairItemReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size) {
    if (size < 6) {
        LOG("[RepairHandler] Invalid payload size for REPAIRITEM_REQ: " + std::to_string(size));
        SendRepairItemAck(s, ERR_REPAIR_DBOPEN, 0);
        return;
    }
    
    DWORD dwItemID = *(DWORD*)(payload);
    BYTE bSackID   = payload[4];
    BYTE bSackPos  = payload[5];
    
    LOG("[RepairHandler] REPAIRITEM_REQ from " + std::to_string(dwCharID) 
        + " ItemID=" + std::to_string(dwItemID) 
        + " SackID=" + std::to_string(bSackID) 
        + " SackPos=" + std::to_string(bSackPos));
    
    // 1. Look up the item
    ItemDB::FullItemRow row;
    if (!ItemDB::GetInstance().GetFullItemData(dwItemID, row)) {
        LOG("[RepairHandler] Item not found: " + std::to_string(dwItemID));
        SendRepairItemAck(s, ERR_REPAIR_NOTFOUND, 0);
        return;
    }
    
    // Fill in template defaults
    int type = row.bType;
    DWORD cost = row.dwCost;
    WORD refid = (WORD)row.wRefID;
    
    if (g_ItemTemplates.count(refid)) {
        if (type == 0) type = g_ItemTemplates[refid].bType;
        if (cost == 0) cost = g_ItemTemplates[refid].dwCost;
    }
    
    // 2. Check if item is an equipment type (bType 1-9 = equipment)
    if (type < 1 || type > 9) {
        LOG("[RepairHandler] Not an equipment item, bType=" + std::to_string(type));
        SendRepairItemAck(s, ERR_REPAIR_NOTREPAIRITEM, 0);
        return;
    }
    
    // 3. Get durability from ITEMDATA
    // d[1] = nData2 = wCurDur, d[2] = nData3 = wMaxDur
    WORD wCurDur = (WORD)row.d[1];
    WORD wMaxDur = (WORD)row.d[2];
    
    // If durability data not set in ITEMDATA (-9999 sentinel), use template defaults
    if (row.d[1] == -9999 || row.d[2] == -9999) {
        if (g_ItemTemplates.count(refid)) {
            wMaxDur = (WORD)g_ItemTemplates[refid].nBasicData1;
            wCurDur = wMaxDur; // assume full durability
        }
    }
    
    // 4. Check if already at full durability
    if (wCurDur >= wMaxDur || wMaxDur == 0) {
        LOG("[RepairHandler] Item already at full durability or no durability stat");
        SendRepairItemAck(s, ERR_REPAIR_FULLDURABILITY, 0);
        return;
    }
    
    // 5. Calculate repair cost
    DWORD repairCost = CalcRepairCost(cost, wMaxDur, wCurDur);
    
    // 6. Check player's money
    INT64 currentMoney = CharacterDB::GetInstance().GetMoney(dwCharID);
    if ((DWORD)currentMoney < repairCost) {
        LOG("[RepairHandler] Not enough money. Have=" + std::to_string(currentMoney) + " Need=" + std::to_string(repairCost));
        SendRepairItemAck(s, ERR_REPAIR_NOMONEY, 0);
        return;
    }
    
    // 7. Deduct money from DB
    CharacterDB::GetInstance().SubtractMoney(dwCharID, repairCost);
    
    // 8. Update durability in ITEMDATA: set nData2 (CurDur) = nData3 (MaxDur)
    ItemDB::GetInstance().UpdateItemData(dwItemID, 2, (int)wMaxDur);
    
    LOG("[RepairHandler] Repair SUCCESS! ItemID=" + std::to_string(dwItemID) 
        + " Cost=" + std::to_string(repairCost) 
        + " Dur: " + std::to_string(wCurDur) + "->" + std::to_string(wMaxDur));
    
    // 9. Send REPAIRITEM_ACK (success + cost)
    SendRepairItemAck(s, ERR_REPAIR_SUCCESS, repairCost);
    
    // 10. Send DURABILITY_ACK to update client's durability display
    SendDurabilityAck(s, dwItemID, (BYTE)type, bSackID, bSackPos, wMaxDur);
    
    // 11. Send CharStatusInfo to update money on client
    SendCharStatusInfoAck(s, dwCharID, PKT_CHARSTATUSINFO_ACK);
}

// ============================================================
// Handler: CS_IM_REPAIRWITHITEM_REQ (Item-based repair)
// Payload: dwResItemID(4) + bResSackID(1) + bResSackPos(1)
//        + dwTarItemID(4) + bTarSackID(1) + bTarSackPos(1)
// ============================================================
static void OnRepairWithItemReq(SOCKET s, DWORD dwCharID, BYTE* payload, WORD size) {
    if (size < 12) {
        LOG("[RepairHandler] Invalid payload size for REPAIRWITHITEM_REQ: " + std::to_string(size));
        SendRepairWithItemAck(s, ERR_RWITH_FAIL, 0);
        return;
    }
    
    DWORD dwResItemID = *(DWORD*)(payload);       // repair item (resource)
    BYTE bResSackID   = payload[4];
    BYTE bResSackPos  = payload[5];
    DWORD dwTarItemID = *(DWORD*)(payload + 6);   // target item to repair
    BYTE bTarSackID   = payload[10];
    BYTE bTarSackPos  = payload[11];
    
    LOG("[RepairHandler] REPAIRWITHITEM_REQ from " + std::to_string(dwCharID) 
        + " ResItemID=" + std::to_string(dwResItemID) 
        + " TarItemID=" + std::to_string(dwTarItemID));
    
    // 1. Look up the repair resource item
    ItemDB::FullItemRow resRow;
    if (!ItemDB::GetInstance().GetFullItemData(dwResItemID, resRow)) {
        LOG("[RepairHandler] Resource item not found: " + std::to_string(dwResItemID));
        SendRepairWithItemAck(s, ERR_RWITH_NOTFOUNDRES, 0);
        return;
    }
    
    // 2. Look up the target item
    ItemDB::FullItemRow tarRow;
    if (!ItemDB::GetInstance().GetFullItemData(dwTarItemID, tarRow)) {
        LOG("[RepairHandler] Target item not found: " + std::to_string(dwTarItemID));
        SendRepairWithItemAck(s, ERR_RWITH_NOTFOUND, 0);
        return;
    }
    
    // Fill template defaults for target
    int type = tarRow.bType;
    DWORD cost = tarRow.dwCost;
    WORD refid = (WORD)tarRow.wRefID;
    
    if (g_ItemTemplates.count(refid)) {
        if (type == 0) type = g_ItemTemplates[refid].bType;
        if (cost == 0) cost = g_ItemTemplates[refid].dwCost;
    }
    
    // 3. Validate target is equipment
    if (type < 1 || type > 9) {
        LOG("[RepairHandler] Target not equipment, bType=" + std::to_string(type));
        SendRepairWithItemAck(s, ERR_REPAIR_NOTREPAIRITEM, 0);
        return;
    }
    
    // 4. Get durability
    WORD wCurDur = (WORD)tarRow.d[1];
    WORD wMaxDur = (WORD)tarRow.d[2];
    
    if (tarRow.d[1] == -9999 || tarRow.d[2] == -9999) {
        if (g_ItemTemplates.count(refid)) {
            wMaxDur = (WORD)g_ItemTemplates[refid].nBasicData1;
            wCurDur = wMaxDur;
        }
    }
    
    // 5. Check if already at full durability
    if (wCurDur >= wMaxDur || wMaxDur == 0) {
        SendRepairWithItemAck(s, ERR_RWITH_FULLDURABILITY, 0);
        return;
    }
    
    // 6. Calculate repair cost (item-based repair typically has reduced cost)
    DWORD repairCost = CalcRepairCost(cost, wMaxDur, wCurDur);
    // Item-based repair costs half
    repairCost = repairCost / 2;
    if (repairCost == 0 && wCurDur < wMaxDur) repairCost = 1;
    
    // 7. Check player's money
    INT64 currentMoney = CharacterDB::GetInstance().GetMoney(dwCharID);
    if ((DWORD)currentMoney < repairCost) {
        SendRepairWithItemAck(s, ERR_RWITH_NOMONEY, 0);
        return;
    }
    
    // 8. Deduct money
    CharacterDB::GetInstance().SubtractMoney(dwCharID, repairCost);
    
    // 9. Update durability to max
    ItemDB::GetInstance().UpdateItemData(dwTarItemID, 2, (int)wMaxDur);
    
    // 10. Consume the repair item (reduce amount by 1 or delete)
    WORD resAmount = ItemDB::GetInstance().GetItemAmount(dwResItemID);
    if (resAmount > 1) {
        // Just decrease the amount
        std::string q = "UPDATE ITEM SET wAmount = wAmount - 1 WHERE dwItemID = " + std::to_string(dwResItemID);
        DBHelper::GetInstance().ExecuteUpdate(q);
    } else {
        // Delete the item and remove from sack
        std::string q1 = "DELETE FROM SACKITEM WHERE dwItemID = " + std::to_string(dwResItemID);
        std::string q2 = "DELETE FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwResItemID);
        std::string q3 = "DELETE FROM ITEM WHERE dwItemID = " + std::to_string(dwResItemID);
        DBHelper::GetInstance().ExecuteUpdate(q1);
        DBHelper::GetInstance().ExecuteUpdate(q2);
        DBHelper::GetInstance().ExecuteUpdate(q3);
        
        // Send RemoveFromSack to client
        std::vector<BYTE> rmBuf;
        rmBuf.resize(4);
        pushByte(rmBuf, bResSackID);
        pushByte(rmBuf, bResSackPos);
        pushDWord(rmBuf, dwResItemID);
        
        PACKET_HEADER* rmHdr = (PACKET_HEADER*)rmBuf.data();
        rmHdr->id = PKT_REMOVEFROMSACK_ACK; // 0x4208
        rmHdr->payloadSize = (WORD)(rmBuf.size() - 4);
        EncryptPacket(rmBuf.data(), ENCRYPT_KEY);
        SafeSend(s, (const char*)rmBuf.data(), (int)rmBuf.size(), 0);
    }
    
    LOG("[RepairHandler] RepairWithItem SUCCESS! TarItemID=" + std::to_string(dwTarItemID) 
        + " Cost=" + std::to_string(repairCost) 
        + " Dur: " + std::to_string(wCurDur) + "->" + std::to_string(wMaxDur));
    
    // 11. Send REPAIRWITHITEM_ACK
    SendRepairWithItemAck(s, ERR_RWITH_SUCCESS, repairCost);
    
    // 12. Send DURABILITY_ACK
    SendDurabilityAck(s, dwTarItemID, (BYTE)type, bTarSackID, bTarSackPos, wMaxDur);
    
    // 13. Update money on client
    SendCharStatusInfoAck(s, dwCharID, PKT_CHARSTATUSINFO_ACK);
}

// ============================================================
// Register Handlers
// ============================================================
void RegisterRepairHandlers() {
    RegisterHandler(PKT_REPAIRITEM_REQ, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (charID) OnRepairItemReq(s, charID, p, size);
    });
    
    RegisterHandler(PKT_REPAIRWITHITEM_REQ, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (charID) OnRepairWithItemReq(s, charID, p, size);
    });
    
    LOG("[RepairHandler] Registered repair handlers (NPC=0x" + 
        ([](WORD v){ char buf[8]; snprintf(buf,8,"%04X",v); return std::string(buf); })(PKT_REPAIRITEM_REQ) + 
        ", Item=0x" +
        ([](WORD v){ char buf[8]; snprintf(buf,8,"%04X",v); return std::string(buf); })(PKT_REPAIRWITHITEM_REQ) + 
        ")");
}

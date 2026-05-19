#include "TradeHandler.h"
#include "../DB/CharacterDB.h"
#include "../DB/ItemDB.h"
#include "../GameObjects/MugongManager.h"
#include <algorithm>

extern std::map<WORD, sItemTemplate> g_ItemTemplates;

static void pushByte(std::vector<BYTE>& buf, BYTE b) { buf.push_back(b); }
static void pushWord(std::vector<BYTE>& buf, WORD w) { buf.push_back(w & 0xFF); buf.push_back((w >> 8) & 0xFF); }
static void pushDWord(std::vector<BYTE>& buf, DWORD d) { buf.push_back(d & 0xFF); buf.push_back((d >> 8) & 0xFF); buf.push_back((d >> 16) & 0xFF); buf.push_back(d >> 24); }
static void pushInt64(std::vector<BYTE>& buf, INT64 v) { for (int i = 0; i < 8; i++) buf.push_back((v >> (i * 8)) & 0xFF); }

static void SendPacket(SOCKET s, WORD id, const std::vector<BYTE>& payload) {
    std::vector<BYTE> pkt(4 + payload.size());
    PACKET_HEADER* h = (PACKET_HEADER*)pkt.data();
    h->id = id; h->payloadSize = (WORD)payload.size();
    if (!payload.empty()) memcpy(pkt.data() + 4, payload.data(), payload.size());
    EncryptPacket(pkt.data(), 0x42);
    SafeSend(s, (const char*)pkt.data(), (int)pkt.size(), 0);
}

// Forward declaration — full definition after CompleteTrade
static void SendAddOnSackAck(SOCKET s, BYTE bSackID, BYTE bSackPos, DWORD dwItemID);

// ============================================================
// AskTrade: Player A Ctrl+clicks Player B to request trade
// Payload: bAction(1) + dwSelfObjectID(4) + dwTargetObjectID(4) = 9 bytes
// ============================================================
void TradeManager::OnAskTradeReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    if (size < 9) {
        LOG("[Trade] OnAskTradeReq: payload too small (" + std::to_string(size) + " bytes)");
        return;
    }
    // BYTE bAction = payload[0]; // 0 = request trade
    // DWORD dwSelfObjID = *(DWORD*)(payload + 1); // requester's own ObjectID
    DWORD dwTargetObjID = *(DWORD*)(payload + 5); // target's ObjectID
    
    LOG("[Trade] OnAskTradeReq: targetObjID=" + std::to_string(dwTargetObjID));
    
    // Convert ObjectID (800M format from map click) to CharID (400M format)
    DWORD targetCharID = dwTargetObjID;
    if (targetCharID >= 800000000) targetCharID -= 400000000;
    
    SOCKET targetSock = SessionMgr::GetInstance().GetSocketByCharID(targetCharID);
    if (targetSock == INVALID_SOCKET) {
        LOG("[Trade] Target not online: " + std::to_string(targetCharID));
        return;
    }
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Check if either player is already trading
    if (m_activeTrades.count(charID) || m_activeTrades.count(targetCharID)) {
        LOG("[Trade] One of the players is already in a trade");
        return;
    }
    
    // Create trade session
    auto session = std::make_shared<TradeSession>();
    session->dwPlayerA = charID;
    session->dwPlayerB = targetCharID;
    session->sockA = s;
    session->sockB = targetSock;
    session->stateA = TradeState::PENDING;
    session->stateB = TradeState::IDLE;
    
    m_activeTrades[charID] = session;
    m_activeTrades[targetCharID] = session;
    
    // Send AskTrade ACK to target player
    // Payload: bResult(1) + dwAskerObjectID(4)
    DWORD askerObjID = charID + 400000000; // Convert to 800M ObjectID for client
    std::vector<BYTE> ack;
    pushByte(ack, 0); // bResult = 0 (request)
    pushDWord(ack, askerObjID);
    SendPacket(targetSock, PKT_ASKTRADE_ACK, ack);
    
    LOG("[Trade] Trade request sent from " + std::to_string(charID) + " to " + std::to_string(targetCharID));
}

// ============================================================
// TradeOpenSack: Target player accepts the trade request
// This is actually the response to AskTrade — client sends back
// PKT_ASKTRADE_REQ with bResult=0 to accept
// ============================================================

// Helper to send TRADEOPENSACK_ACK to both players
static void SendTradeOpenSack(SOCKET s, DWORD traderObjID) {
    std::vector<BYTE> payload;
    pushByte(payload, 0); // bResult = success
    pushDWord(payload, traderObjID);
    SendPacket(s, PKT_TRADEOPENSACK_ACK, payload);
    LOG("[Trade] Sent TRADEOPENSACK_ACK to socket, traderObjID=" + std::to_string(traderObjID));
}

// ============================================================
// TradeSackOnItem: Player places an item into the trade window
// Payload: bSackID(1) bSackPos(1) dwItemID(4) wAmount(2)
// ============================================================
void TradeManager::OnTradeSackOnItemReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    if (size < 8) return;
    BYTE bSackID = payload[0];
    BYTE bSackPos = payload[1];
    DWORD dwItemID = *(DWORD*)(payload + 2);
    WORD wAmount = *(WORD*)(payload + 6);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_activeTrades.find(charID);
    if (it == m_activeTrades.end()) return;
    auto session = it->second;
    
    bool isA = (session->dwPlayerA == charID);
    auto& mySlots = isA ? session->slotsA : session->slotsB;
    auto& myState = isA ? session->stateA : session->stateB;
    SOCKET otherSock = isA ? session->sockB : session->sockA;
    DWORD otherCharID = isA ? session->dwPlayerB : session->dwPlayerA;
    
    if (myState != TradeState::OPEN) return;
    
    // Verify item ownership
    bool owned = ItemDB::GetInstance().IsSackItemOwned(charID, dwItemID);
    if (!owned) return;
    
    // Check not already placed
    for (auto& sl : mySlots) {
        if (sl.dwItemID == dwItemID) return;
    }
    
    // Add to trade slots
    TradeSlot slot;
    slot.dwItemID = dwItemID;
    slot.dwAmount = wAmount;
    slot.bSrcSackID = bSackID;
    slot.bSrcSackPos = bSackPos;
    slot.bTradePos = (BYTE)mySlots.size();
    mySlots.push_back(slot);
    
    // Reset confirm states when items change
    session->stateA = TradeState::OPEN;
    session->stateB = TradeState::OPEN;
    
    // ACK to self: remove from sack visually
    std::vector<BYTE> rmPayload;
    pushByte(rmPayload, bSackID);
    pushByte(rmPayload, bSackPos);
    pushByte(rmPayload, 2); // reason = trade
    SendPacket(s, 0x4208, rmPayload); // REMOVESACK_ACK
    
    // ACK to self
    std::vector<BYTE> selfAck;
    pushByte(selfAck, 0); // bResult
    pushByte(selfAck, slot.bTradePos);
    pushDWord(selfAck, dwItemID);
    pushWord(selfAck, wAmount);
    SendPacket(s, PKT_TRADESACKONITEM_ACK, selfAck);
    
    // Notify other player about the item being placed
    // Build full item data for the other player to see
    BuildAndSendItemData(otherSock, charID + 400000000, bSackID, bSackPos, 0, slot.bTradePos, dwItemID, wAmount);
    
    LOG("[Trade] Player " + std::to_string(charID) + " placed item " + std::to_string(dwItemID) + " in trade");
}

// ============================================================
// TradeSackOffItem: Player removes an item from trade window
// Payload: bTradePos(1) dwItemID(4)
// ============================================================
void TradeManager::OnTradeSackOffItemReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    if (size < 5) return;
    BYTE bTradePos = payload[0];
    DWORD dwItemID = *(DWORD*)(payload + 1);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_activeTrades.find(charID);
    if (it == m_activeTrades.end()) return;
    auto session = it->second;
    
    bool isA = (session->dwPlayerA == charID);
    auto& mySlots = isA ? session->slotsA : session->slotsB;
    auto& myState = isA ? session->stateA : session->stateB;
    SOCKET otherSock = isA ? session->sockB : session->sockA;
    
    if (myState != TradeState::OPEN) return;
    
    // Find and remove the slot
    auto sit = std::find_if(mySlots.begin(), mySlots.end(), [&](const TradeSlot& sl) { return sl.dwItemID == dwItemID; });
    if (sit == mySlots.end()) return;
    
    BYTE origSackPos = sit->bSrcSackPos;
    mySlots.erase(sit);
    
    // Reset confirm states
    session->stateA = TradeState::OPEN;
    session->stateB = TradeState::OPEN;
    
    // ACK to self
    std::vector<BYTE> selfAck;
    pushByte(selfAck, 0); // bResult
    pushByte(selfAck, bTradePos);
    pushDWord(selfAck, dwItemID);
    SendPacket(s, PKT_TRADESACKOFFITEM_ACK, selfAck);
    
    // Notify other player
    std::vector<BYTE> otherAck;
    pushByte(otherAck, 0);
    pushByte(otherAck, bTradePos);
    pushDWord(otherAck, dwItemID);
    SendPacket(otherSock, PKT_TRADESACKOFFITEM_ACK, otherAck);
    
    LOG("[Trade] Player " + std::to_string(charID) + " removed item " + std::to_string(dwItemID) + " from trade");
}

// ============================================================
// TradeItem: Player confirms the trade (clicks OK/Confirm)
// Payload: dwTargetObjectID(4) bAction(1)
//   bAction=0: initial confirm, bAction=1: accept after both confirmed
// ============================================================
void TradeManager::OnTradeItemReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    if (size < 5) return;
    DWORD dwTargetObjID = *(DWORD*)(payload);
    BYTE bAction = payload[4];
    
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_activeTrades.find(charID);
    if (it == m_activeTrades.end()) {
        // This might be the accept response to AskTrade
        // bAction=0 means accept, bAction=1 means reject
        return;
    }
    auto session = it->second;
    
    bool isA = (session->dwPlayerA == charID);
    auto& myState = isA ? session->stateA : session->stateB;
    auto& otherState = isA ? session->stateB : session->stateA;
    SOCKET otherSock = isA ? session->sockB : session->sockA;
    DWORD otherCharID = isA ? session->dwPlayerB : session->dwPlayerA;
    
    if (bAction == 2) {
        // Cancel trade
        CancelTrade(session, charID);
        return;
    }
    
    // Player confirms
    myState = TradeState::CONFIRMED;
    
    // Notify other player that this player confirmed
    SendTradeItemAck(otherSock, 0, charID + 400000000);
    
    // Check if both players confirmed
    if (session->stateA == TradeState::CONFIRMED && session->stateB == TradeState::CONFIRMED) {
        CompleteTrade(session);
    }
}

// ============================================================
// TradeSackOnMoney: Player places money in trade
// Payload: dwMoney(4) or INT64 money
// ============================================================
void TradeManager::OnTradeSackOnMoneyReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    if (size < 4) return;
    DWORD dwMoney = *(DWORD*)(payload);
    
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_activeTrades.find(charID);
    if (it == m_activeTrades.end()) return;
    auto session = it->second;
    
    bool isA = (session->dwPlayerA == charID);
    auto& myState = isA ? session->stateA : session->stateB;
    auto& myMoney = isA ? session->dwMoneyA : session->dwMoneyB;
    SOCKET otherSock = isA ? session->sockB : session->sockA;
    
    if (myState != TradeState::OPEN) return;
    
    // Verify player has enough money
    DWORD currentMoney = CharacterDB::GetInstance().GetMoney(charID);
    if (dwMoney > currentMoney) dwMoney = currentMoney;
    
    myMoney = dwMoney;
    
    // Reset confirm states
    session->stateA = TradeState::OPEN;
    session->stateB = TradeState::OPEN;
    
    // ACK to self
    std::vector<BYTE> selfAck;
    pushByte(selfAck, 0);
    pushDWord(selfAck, dwMoney);
    SendPacket(s, PKT_TRADESACKONMONEY_ACK, selfAck);
    
    // Notify other
    std::vector<BYTE> otherAck;
    pushByte(otherAck, 0);
    pushDWord(otherAck, dwMoney);
    SendPacket(otherSock, PKT_TRADESACKONMONEY_ACK, otherAck);
    
    LOG("[Trade] Player " + std::to_string(charID) + " placed " + std::to_string(dwMoney) + " money in trade");
}

// ============================================================
// TradeSackOffMoney: Player removes money from trade
// ============================================================
void TradeManager::OnTradeSackOffMoneyReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_activeTrades.find(charID);
    if (it == m_activeTrades.end()) return;
    auto session = it->second;
    
    bool isA = (session->dwPlayerA == charID);
    auto& myState = isA ? session->stateA : session->stateB;
    auto& myMoney = isA ? session->dwMoneyA : session->dwMoneyB;
    SOCKET otherSock = isA ? session->sockB : session->sockA;
    
    if (myState != TradeState::OPEN) return;
    
    myMoney = 0;
    
    session->stateA = TradeState::OPEN;
    session->stateB = TradeState::OPEN;
    
    std::vector<BYTE> ack;
    pushByte(ack, 0);
    SendPacket(s, PKT_TRADESACKOFFMONEY_ACK, ack);
    SendPacket(otherSock, PKT_TRADESACKOFFMONEY_ACK, ack);
    
    LOG("[Trade] Player " + std::to_string(charID) + " removed money from trade");
}

// ============================================================
// Disconnect cleanup
// ============================================================
void TradeManager::OnPlayerDisconnect(DWORD charID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    auto it = m_activeTrades.find(charID);
    if (it == m_activeTrades.end()) return;
    CancelTrade(it->second, charID);
}

bool TradeManager::IsTrading(DWORD charID) {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_activeTrades.count(charID) > 0;
}

// ============================================================
// Cancel: return all items to original owners' UI
// Items were never removed from DB, only visually via REMOVESACK.
// We must send ADDONSACK_ACK to restore each item in the client UI.
// ============================================================
void TradeManager::CancelTrade(std::shared_ptr<TradeSession> session, DWORD cancellerID) {
    // Send cancel ACK to both players
    std::vector<BYTE> cancelPayload;
    pushByte(cancelPayload, 2); // bResult = cancel
    pushDWord(cancelPayload, cancellerID + 400000000);
    
    if (session->sockA != INVALID_SOCKET)
        SendPacket(session->sockA, PKT_TRADECOMPLETE_ACK, cancelPayload);
    if (session->sockB != INVALID_SOCKET)
        SendPacket(session->sockB, PKT_TRADECOMPLETE_ACK, cancelPayload);
    
    // Restore items to UI: send ADDONSACK_ACK for each item back to its owner
    for (auto& slot : session->slotsA) {
        if (session->sockA != INVALID_SOCKET)
            SendAddOnSackAck(session->sockA, slot.bSrcSackID, slot.bSrcSackPos, slot.dwItemID);
    }
    for (auto& slot : session->slotsB) {
        if (session->sockB != INVALID_SOCKET)
            SendAddOnSackAck(session->sockB, slot.bSrcSackID, slot.bSrcSackPos, slot.dwItemID);
    }
    
    // Clean up
    m_activeTrades.erase(session->dwPlayerA);
    m_activeTrades.erase(session->dwPlayerB);
    
    LOG("[Trade] Trade cancelled by " + std::to_string(cancellerID));
}

// ============================================================
// Helper: convert absolute DB position to (sackID, relative pos)
// ============================================================
static void AbsPosToSackPos(BYTE absPos, BYTE& outSackID, BYTE& outRelPos) {
    if (absPos < 20) { outSackID = 0; outRelPos = absPos; }
    else if (absPos < 60) { outSackID = 1; outRelPos = absPos - 20; }
    else if (absPos < 100) { outSackID = 2; outRelPos = absPos - 60; }
    else { outSackID = 3; outRelPos = absPos - 100; }
}

// ============================================================
// Complete: swap items and money between players
// ============================================================
void TradeManager::CompleteTrade(std::shared_ptr<TradeSession> session) {
    DWORD charA = session->dwPlayerA;
    DWORD charB = session->dwPlayerB;
    
    // Track positions for UI sync
    struct TransferResult { DWORD dwItemID; BYTE absSackPos; };
    std::vector<TransferResult> itemsForB, itemsForA;
    
    // Transfer items from A to B
    for (auto& slot : session->slotsA) {
        ItemDB::GetInstance().RemoveFromSack(charA, slot.dwItemID);
        BYTE bCX = 1, bCY = 1;
        ItemDB::ItemBasicInfo ib;
        if (ItemDB::GetInstance().GetItemBasicInfo(slot.dwItemID, ib) && g_ItemTemplates.count(ib.wRefID)) {
            bCX = g_ItemTemplates[ib.wRefID].bCX;
            bCY = g_ItemTemplates[ib.wRefID].bCY;
        }
        BYTE freePos = 255;
        for (BYTE tryID = 1; tryID <= 3; tryID++) {
            freePos = FindFreeSackPos(charB, tryID, bCX, bCY);
            if (freePos != 255) break;
        }
        if (freePos != 255) {
            ItemDB::GetInstance().AddToSack(charB, freePos, slot.dwItemID);
            itemsForB.push_back({slot.dwItemID, freePos});
        } else {
            LOG("[Trade] WARNING: No free sack space for B to receive item " + std::to_string(slot.dwItemID));
        }
    }
    
    // Transfer items from B to A
    for (auto& slot : session->slotsB) {
        ItemDB::GetInstance().RemoveFromSack(charB, slot.dwItemID);
        BYTE bCX = 1, bCY = 1;
        ItemDB::ItemBasicInfo ib;
        if (ItemDB::GetInstance().GetItemBasicInfo(slot.dwItemID, ib) && g_ItemTemplates.count(ib.wRefID)) {
            bCX = g_ItemTemplates[ib.wRefID].bCX;
            bCY = g_ItemTemplates[ib.wRefID].bCY;
        }
        BYTE freePos = 255;
        for (BYTE tryID = 1; tryID <= 3; tryID++) {
            freePos = FindFreeSackPos(charA, tryID, bCX, bCY);
            if (freePos != 255) break;
        }
        if (freePos != 255) {
            ItemDB::GetInstance().AddToSack(charA, freePos, slot.dwItemID);
            itemsForA.push_back({slot.dwItemID, freePos});
        } else {
            LOG("[Trade] WARNING: No free sack space for A to receive item " + std::to_string(slot.dwItemID));
        }
    }
    
    // Transfer money
    if (session->dwMoneyA > 0) {
        CharacterDB::GetInstance().SubtractMoney(charA, session->dwMoneyA);
        CharacterDB::GetInstance().AddMoney(charB, session->dwMoneyA);
    }
    if (session->dwMoneyB > 0) {
        CharacterDB::GetInstance().SubtractMoney(charB, session->dwMoneyB);
        CharacterDB::GetInstance().AddMoney(charA, session->dwMoneyB);
    }
    
    // Send complete ACK to both (this closes the trade window)
    SendTradeCompleteAck(session->sockA, session->dwPlayerB + 400000000);
    SendTradeCompleteAck(session->sockB, session->dwPlayerA + 400000000);
    
    // Send ADDONSACK_ACK (0x420A) for each received item so they appear in the UI
    for (auto& tr : itemsForA) {
        BYTE sackID, relPos;
        AbsPosToSackPos(tr.absSackPos, sackID, relPos);
        SendAddOnSackAck(session->sockA, sackID, relPos, tr.dwItemID);
    }
    for (auto& tr : itemsForB) {
        BYTE sackID, relPos;
        AbsPosToSackPos(tr.absSackPos, sackID, relPos);
        SendAddOnSackAck(session->sockB, sackID, relPos, tr.dwItemID);
    }
    
    // Send updated money to both
    SendMoneyUpdate(session->sockA, charA);
    SendMoneyUpdate(session->sockB, charB);
    
    // Clean up session
    m_activeTrades.erase(charA);
    m_activeTrades.erase(charB);
    
    LOG("[Trade] Trade completed between " + std::to_string(charA) + " and " + std::to_string(charB));
}

void TradeManager::SendTradeItemAck(SOCKET s, BYTE bResult, DWORD dwTraderID) {
    std::vector<BYTE> payload;
    pushByte(payload, bResult);
    pushDWord(payload, dwTraderID);
    SendPacket(s, PKT_TRADEITEM_ACK, payload);
}

void TradeManager::SendTradeCompleteAck(SOCKET s, DWORD dwTraderID) {
    std::vector<BYTE> payload;
    pushByte(payload, 0); // bResult = success
    pushDWord(payload, dwTraderID);
    SendPacket(s, PKT_TRADECOMPLETE_ACK, payload);
}

void TradeManager::SendMoneyUpdate(SOCKET s, DWORD charID) {
    INT64 money = (INT64)CharacterDB::GetInstance().GetMoney(charID);
    std::vector<BYTE> payload;
    pushInt64(payload, money);
    pushByte(payload, 0); // bFreeUser
    SendPacket(s, 0x3B13, payload); // CS_IF_CHARMONEY_ACK
}

void TradeManager::BuildAndSendItemData(SOCKET targetSocket, DWORD ownerObjectID,
                                         BYTE bSrcSackID, BYTE bSrcPos,
                                         BYTE bDesSackID, BYTE bDesPos,
                                         DWORD dwItemID, DWORD dwAmount) {
    // Simplified: send TRADESACKONITEM_ACK with item info to the other player
    ItemDB::FullItemRow row;
    if (!ItemDB::GetInstance().GetFullItemData(dwItemID, row)) return;
    
    int type = row.bType, refid = row.wRefID;
    if (g_ItemTemplates.count(refid)) {
        if (type == 0) type = g_ItemTemplates[refid].bType;
    }
    
    std::string itemName(row.szName);
    if (itemName.empty() && g_ItemTemplates.count(refid)) itemName = g_ItemTemplates[refid].szName;
    
    std::vector<BYTE> payload;
    pushByte(payload, 0);       // bResult
    pushByte(payload, bDesPos); // trade position
    pushDWord(payload, dwItemID);
    pushWord(payload, (WORD)refid);
    pushByte(payload, (BYTE)type);
    pushWord(payload, row.wVisualID);
    pushWord(payload, (WORD)itemName.length());
    for (char ch : itemName) pushByte(payload, ch);
    pushWord(payload, (WORD)dwAmount);
    
    SendPacket(targetSocket, PKT_TRADESACKONITEM_ACK, payload);
}

// ============================================================
// SendAddOnSackAck: Full item serialization for 0x420A
// Same GetItemData format as BankHandler/ItemHandler
// ============================================================
static void SendAddOnSackAck(SOCKET s, BYTE bSackID, BYTE bSackPos, DWORD dwItemID) {
    ItemDB::FullItemRow row;
    if (!ItemDB::GetInstance().GetFullItemData(dwItemID, row)) return;
    
    int vis=row.wVisualID, type=row.bType, kind=row.bKind, lvl=row.wLevel, cost=row.dwCost;
    int dat18=row.nData18, dat19=row.nData19, dat20=row.nData20;
    int refid=row.wRefID, amount=row.wAmount;
    int d[17]; for (int i=0;i<17;i++) d[i]=row.d[i];
    
    int nd1=0, nd2=0, nd3=0, nd4=0, nd5=0;
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
    for (int i=0;i<17;i++) { if (d[i]==-9999) d[i]=0; }
    
    std::string itemName(row.szName);
    if (itemName.empty() && g_ItemTemplates.count(refid)) itemName = g_ItemTemplates[refid].szName;
    
    std::vector<BYTE> bi;
    bi.resize(4); // header
    pushByte(bi, bSackID);
    pushByte(bi, bSackPos);
    pushDWord(bi, dwItemID); pushWord(bi, refid);
    pushByte(bi, type); pushByte(bi, kind); pushWord(bi, vis);
    pushWord(bi, (WORD)itemName.length());
    for (char ch : itemName) pushByte(bi, ch);
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
        if (type == 9) { pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); }
        else if (type == 8) { for(int i=0;i<8;i++) pushByte(bi, 0); }
        else { pushByte(bi, 0); }
        if (type >= 1 && type <= 4) { pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushWord(bi, dat20); }
    } else {
        switch (type) {
            case 11: case 12: case 13: case 14: case 17: pushByte(bi, 0); pushWord(bi, d[1]); pushWord(bi, d[2]); break;
            case 15: pushByte(bi, d[0]); pushWord(bi, d[1]); pushWord(bi, d[2]); pushByte(bi, 0); pushByte(bi, 0); break;
            case 16: pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); break;
            case 18: pushByte(bi, 0); pushDWord(bi, 0); pushWord(bi, d[1]); pushWord(bi, d[2]); pushByte(bi, 0); break;
            case 19: pushWord(bi, 0); pushWord(bi, 0); break;
            case 20: pushByte(bi, 0); pushDWord(bi, 0); break;
            case 21: { DWORD mid=nd2; sMugongTemplate* mg=MugongManager::GetInstance()->GetTemplate(mid); pushWord(bi, lvl); pushDWord(bi, mid); pushByte(bi, mg?mg->bType:0); pushByte(bi, mg?mg->bKind:0); pushByte(bi, 1); break; }
            case 22: pushDWord(bi, 0); pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
            case 23: pushDWord(bi, 0); pushWord(bi, 0); pushWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); break;
            case 25: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
            case 27: pushByte(bi, 0); pushDWord(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushByte(bi, 0); pushDWord(bi, 0); break;
            case 29: pushWord(bi, 0); pushWord(bi, 0); break;
            case 32: pushWord(bi, 0); pushWord(bi, d[1]); pushWord(bi, d[2]); pushDWord(bi, 0); break;
            case 31: pushByte(bi, 0); pushWord(bi, 0); pushWord(bi, 0); break;
            case 34: pushDWord(bi, 0); pushByte(bi, 0); break;
        }
    }
    pushWord(bi, (WORD)dat20); // wRebuithValue
    
    PACKET_HEADER* head = (PACKET_HEADER*)bi.data();
    head->id = 0x420A; // ADDONSACK_ACK
    head->payloadSize = (WORD)(bi.size() - 4);
    EncryptPacket(bi.data(), 0x42);
    SafeSend(s, (const char*)bi.data(), (int)bi.size(), 0);
}

// ============================================================
// Handler Registration
// ============================================================
void RegisterTradeHandlers() {
    // AskTrade REQ - initiates trade or accepts/rejects
    RegisterHandler(PKT_ASKTRADE_REQ, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        
        // Debug: log the raw payload
        std::string hexDump;
        for (int i = 0; i < size; i++) { char tmp[8]; sprintf(tmp, "%02X ", p[i]); hexDump += tmp; }
        LOG("[Trade] AskTrade REQ from charID=" + std::to_string(charID) + " size=" + std::to_string(size) + " hex: " + hexDump);
        
        // Packet format: bAction(1) + dwSelfObjID(4) + dwTargetObjID(4) = 9 bytes
        // p[0]=0: initial trade request, p[0]=1: accept, p[0]=2: reject
        BYTE bAction = p[0];
        LOG("[Trade] AskTrade REQ bAction=" + std::to_string(bAction));
        
        if (bAction == 0) {
            // Initial trade request
            LOG("[Trade] Initial trade request from " + std::to_string(charID));
            TradeManager::GetInstance().OnAskTradeReq(s, charID, p, size);
        } else {
            // Accept or Reject response
            auto& tm = TradeManager::GetInstance();
            std::lock_guard<std::mutex> lock(tm.m_mutex);
            auto it = tm.m_activeTrades.find(charID);
            if (it == tm.m_activeTrades.end()) {
                LOG("[Trade] No session found for charID=" + std::to_string(charID));
                return;
            }
            auto session = it->second;
            
            if (bAction == 1) {
                // Accept trade
                session->stateA = TradeState::OPEN;
                session->stateB = TradeState::OPEN;
                SendTradeOpenSack(session->sockA, session->dwPlayerB + 400000000);
                SendTradeOpenSack(session->sockB, session->dwPlayerA + 400000000);
                LOG("[Trade] Trade opened between " + std::to_string(session->dwPlayerA) + " and " + std::to_string(session->dwPlayerB));
            } else {
                // Reject trade (bAction=2 or any other value)
                SOCKET askerSock = (session->dwPlayerA == charID) ? session->sockB : session->sockA;
                std::vector<BYTE> rejectAck;
                pushByte(rejectAck, 1);
                pushDWord(rejectAck, charID + 400000000);
                SendPacket(askerSock, PKT_ASKTRADE_ACK, rejectAck);
                tm.m_activeTrades.erase(session->dwPlayerA);
                tm.m_activeTrades.erase(session->dwPlayerB);
                LOG("[Trade] Trade rejected by " + std::to_string(charID));
            }
        }
    });
    
    RegisterHandler(PKT_TRADESACKONITEM_REQ, [](SOCKET s, BYTE* p, WORD size) {
        TradeManager::GetInstance().OnTradeSackOnItemReq(s, SessionMgr::GetInstance().GetCharID(s), p, size);
    });
    
    RegisterHandler(PKT_TRADESACKOFFITEM_REQ, [](SOCKET s, BYTE* p, WORD size) {
        TradeManager::GetInstance().OnTradeSackOffItemReq(s, SessionMgr::GetInstance().GetCharID(s), p, size);
    });
    
    RegisterHandler(PKT_TRADEITEM_REQ, [](SOCKET s, BYTE* p, WORD size) {
        TradeManager::GetInstance().OnTradeItemReq(s, SessionMgr::GetInstance().GetCharID(s), p, size);
    });
    
    RegisterHandler(PKT_TRADESACKONMONEY_REQ, [](SOCKET s, BYTE* p, WORD size) {
        TradeManager::GetInstance().OnTradeSackOnMoneyReq(s, SessionMgr::GetInstance().GetCharID(s), p, size);
    });
    
    RegisterHandler(PKT_TRADESACKOFFMONEY_REQ, [](SOCKET s, BYTE* p, WORD size) {
        TradeManager::GetInstance().OnTradeSackOffMoneyReq(s, SessionMgr::GetInstance().GetCharID(s), p, size);
    });
}

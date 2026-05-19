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

// ============================================================
// AskTrade: Player A Ctrl+clicks Player B to request trade
// Payload: dwTargetObjectID(4)
// ============================================================
void TradeManager::OnAskTradeReq(SOCKET s, DWORD charID, BYTE* payload, WORD size) {
    if (size < 4) return;
    DWORD dwTargetObjID = *(DWORD*)(payload);
    
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
    pushDWord(payload, traderObjID);
    SendPacket(s, PKT_TRADEOPENSACK_ACK, payload);
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
// Cancel: return all items to original owners
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
    
    // Clean up
    m_activeTrades.erase(session->dwPlayerA);
    m_activeTrades.erase(session->dwPlayerB);
    
    LOG("[Trade] Trade cancelled by " + std::to_string(cancellerID));
}

// ============================================================
// Complete: swap items and money between players
// ============================================================
void TradeManager::CompleteTrade(std::shared_ptr<TradeSession> session) {
    DWORD charA = session->dwPlayerA;
    DWORD charB = session->dwPlayerB;
    
    // Transfer items from A to B
    for (auto& slot : session->slotsA) {
        // Remove from A's sack
        ItemDB::GetInstance().RemoveFromSack(charA, slot.dwItemID);
        // Find free position in B's sack
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
    
    // Send complete ACK to both
    SendTradeCompleteAck(session->sockA, session->dwPlayerB + 400000000);
    SendTradeCompleteAck(session->sockB, session->dwPlayerA + 400000000);
    
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
// Handler Registration
// ============================================================
void RegisterTradeHandlers() {
    // AskTrade REQ - initiates trade or accepts/rejects
    RegisterHandler(PKT_ASKTRADE_REQ, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (size >= 5 && p[4] == 0) {
            // Accept trade: find pending session and open it
            DWORD askerObjID = *(DWORD*)(p);
            DWORD askerCharID = askerObjID;
            if (askerCharID >= 800000000) askerCharID -= 400000000;
            
            auto& tm = TradeManager::GetInstance();
            // The session was already created, just open it
            std::lock_guard<std::mutex> lock(tm.m_mutex);
            auto it = tm.m_activeTrades.find(charID);
            if (it != tm.m_activeTrades.end()) {
                auto session = it->second;
                session->stateA = TradeState::OPEN;
                session->stateB = TradeState::OPEN;
                // Send TRADEOPENSACK to both
                SendTradeOpenSack(session->sockA, session->dwPlayerB + 400000000);
                SendTradeOpenSack(session->sockB, session->dwPlayerA + 400000000);
                LOG("[Trade] Trade opened between " + std::to_string(session->dwPlayerA) + " and " + std::to_string(session->dwPlayerB));
            }
        } else if (size >= 5 && p[4] == 1) {
            // Reject trade
            auto& tm = TradeManager::GetInstance();
            std::lock_guard<std::mutex> lock(tm.m_mutex);
            auto it = tm.m_activeTrades.find(charID);
            if (it != tm.m_activeTrades.end()) {
                auto session = it->second;
                SOCKET askerSock = (session->dwPlayerA == charID) ? session->sockB : session->sockA;
                std::vector<BYTE> rejectAck;
                pushByte(rejectAck, 1); // bResult = rejected
                pushDWord(rejectAck, charID + 400000000);
                SendPacket(askerSock, PKT_ASKTRADE_ACK, rejectAck);
                tm.m_activeTrades.erase(session->dwPlayerA);
                tm.m_activeTrades.erase(session->dwPlayerB);
                LOG("[Trade] Trade rejected by " + std::to_string(charID));
            }
        } else {
            // Initial trade request
            TradeManager::GetInstance().OnAskTradeReq(s, charID, p, size);
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

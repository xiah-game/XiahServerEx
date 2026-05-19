#pragma once
#include "../ServerCore.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../DB/ItemDB.h"
#include "../DB/CharacterDB.h"
#include "../GameObjects/PlayerManager.h"
#include <map>
#include <mutex>
#include <vector>

// ============================================================
// Trade Packet IDs  (CS_EC family, base 0x3D01)
// ============================================================
constexpr WORD PKT_ASKTRADE_REQ         = 0x3D91;  // +144
constexpr WORD PKT_ASKTRADE_ACK         = 0x3D92;  // +145
constexpr WORD PKT_TRADEOPENSACK_ACK    = 0x3D94;  // +147
constexpr WORD PKT_TRADESACKONITEM_REQ  = 0x3D95;  // +148
constexpr WORD PKT_TRADESACKONITEM_ACK  = 0x3D96;  // +149
constexpr WORD PKT_TRADESACKOFFITEM_REQ = 0x3D97;  // +150
constexpr WORD PKT_TRADESACKOFFITEM_ACK = 0x3D98;  // +151
constexpr WORD PKT_TRADEITEM_REQ        = 0x3D99;  // +152
constexpr WORD PKT_TRADEITEM_ACK        = 0x3D9A;  // +153
constexpr WORD PKT_TRADECOMPLETE_ACK    = 0x3D9C;  // +155
constexpr WORD PKT_TRADESACKONMONEY_REQ = 0x3DA9;  // +168
constexpr WORD PKT_TRADESACKONMONEY_ACK = 0x3DAA;  // +169
constexpr WORD PKT_TRADESACKOFFMONEY_REQ = 0x3DAB; // +170
constexpr WORD PKT_TRADESACKOFFMONEY_ACK = 0x3DAC; // +171

// ============================================================
// Trade State
// ============================================================
enum class TradeState {
    IDLE,
    PENDING,     // invitation sent, waiting for accept/reject
    OPEN,        // trade window open, adding items
    CONFIRMED,   // this player clicked "confirm"
};

// Per-player trade slot (item placed in trade window)
struct TradeSlot {
    DWORD dwItemID = 0;
    DWORD dwAmount = 0;
    BYTE  bSrcSackPos = 0;    // original sack position
    BYTE  bTradePos = 0;      // position in trade grid
};

// Active trade session between two players
struct TradeSession {
    DWORD dwPlayerA = 0;      // CharID (400M format ObjectID)
    DWORD dwPlayerB = 0;      // CharID
    SOCKET sockA = INVALID_SOCKET;
    SOCKET sockB = INVALID_SOCKET;
    TradeState stateA = TradeState::IDLE;
    TradeState stateB = TradeState::IDLE;
    std::vector<TradeSlot> slotsA;   // items player A placed
    std::vector<TradeSlot> slotsB;   // items player B placed
    DWORD dwMoneyA = 0;       // money player A offered
    DWORD dwMoneyB = 0;       // money player B offered
};

// ============================================================
// TradeManager singleton
// ============================================================
class TradeManager {
public:
    static TradeManager& GetInstance() {
        static TradeManager instance;
        return instance;
    }

    // Packet handlers
    void OnAskTradeReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnTradeSackOnItemReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnTradeSackOffItemReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnTradeItemReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnTradeSackOnMoneyReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnTradeSackOffMoneyReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);

    // Called when a player disconnects — cancel any active trade
    void OnPlayerDisconnect(DWORD charID);

    // Check if player is currently trading
    bool IsTrading(DWORD charID);

private:
    TradeManager() {}

    std::mutex m_mutex;
    // Map charID → trade session (both players point to the same session)
    std::map<DWORD, std::shared_ptr<TradeSession>> m_activeTrades;

    // Helpers
    void CancelTrade(std::shared_ptr<TradeSession> session, DWORD cancellerID);
    void CompleteTrade(std::shared_ptr<TradeSession> session);
    void SendTradeItemAck(SOCKET s, BYTE bResult, DWORD dwTraderID);
    void SendTradeCompleteAck(SOCKET s, DWORD dwTraderID);
    void SendMoneyUpdate(SOCKET s, DWORD charID);
    void BuildAndSendItemData(SOCKET targetSocket, DWORD ownerObjectID,
                              BYTE bSrcSackID, BYTE bSrcPos,
                              BYTE bDesSackID, BYTE bDesPos,
                              DWORD dwItemID, DWORD dwAmount);
};

// Registration
void RegisterTradeHandlers();

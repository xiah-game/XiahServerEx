#pragma once
#include "../ServerCore.h"
#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "../DB/ItemDB.h"
#include "../DB/CharacterDB.h"
#include <map>
#include <mutex>
#include <vector>
#include <string>

// ============================================================
// Shop Packet IDs  (CS_SH family, OFFSET_CS_SH = 0x3101)
// ============================================================
constexpr WORD PKT_SH_SHOPINFO_ACK        = 0x3101;  // +0  S->C  Open shop window
constexpr WORD PKT_SH_SETSHOP_REQ         = 0x3102;  // +1  C->S  Set shop name/desc
constexpr WORD PKT_SH_SETSHOP_ACK         = 0x3103;  // +2  S->C
constexpr WORD PKT_SH_MOVESHOP_REQ        = 0x3104;  // +3  C->S  Move item in shop grid
constexpr WORD PKT_SH_MOVESHOP_ACK        = 0x3105;  // +4  S->C
constexpr WORD PKT_SH_REGSHOP_REQ         = 0x3106;  // +5  C->S  Register item (sack->shop)
constexpr WORD PKT_SH_REGSHOP_ACK         = 0x3107;  // +6  S->C
constexpr WORD PKT_SH_DELSHOP_REQ         = 0x3108;  // +7  C->S  Remove item (shop->sack)
constexpr WORD PKT_SH_DELSHOP_ACK         = 0x3109;  // +8  S->C
constexpr WORD PKT_SH_STATUSCHANGE_REQ    = 0x310A;  // +9  C->S  Start/stop shop
constexpr WORD PKT_SH_STATUSCHANGE_ACK    = 0x310B;  // +10 S->C
constexpr WORD PKT_SH_GETMONEY_REQ        = 0x310C;  // +11 C->S  Withdraw earnings
constexpr WORD PKT_SH_GETMONEY_ACK        = 0x310D;  // +12 S->C
constexpr WORD PKT_SH_GETSHOPINFO_REQ     = 0x310E;  // +13 C->S  Buyer views shop
constexpr WORD PKT_SH_GETSHOPINFO_ACK     = 0x310F;  // +14 S->C
constexpr WORD PKT_SH_BUYPCSHOP_REQ       = 0x3110;  // +15 C->S  Buyer purchases
constexpr WORD PKT_SH_BUYPCSHOP_ACK       = 0x3111;  // +16 S->C
constexpr WORD PKT_SH_ADDONSHOP_ACK       = 0x3112;  // +17 S->C  Push: item added
constexpr WORD PKT_SH_REMOVEFROMSHOP_ACK  = 0x3113;  // +18 S->C  Push: item removed
constexpr WORD PKT_SH_SHOPINFOCHANGE_ACK  = 0x3114;  // +19 S->C  Push: money/count change

// ============================================================
// Shop Item in memory
// ============================================================
struct ShopSlot {
    BYTE  bSackPos = 0;    // Position in shop grid (0~35)
    DWORD dwItemID = 0;
    DWORD dwPrice  = 0;
};

// ============================================================
// ShopHandler — handles all CS_SH packet processing
// ============================================================
namespace ShopHandler {
    // Called from OnUseItemReq when bType==32 (店铺令牌)
    void OnOpenShop(SOCKET clientSocket, DWORD charID);

    // CS_SH REQ handlers
    void OnSetShopReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnMoveShopReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnRegShopReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnDelShopReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnStatusChangeReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnGetMoneyReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnGetShopInfoReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
    void OnBuyPcShopReq(SOCKET s, DWORD charID, BYTE* payload, WORD size);
}

// Registration
void RegisterShopHandlers();

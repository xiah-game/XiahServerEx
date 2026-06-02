#include "../Network/PacketRouter.h"
#include "../Network/SessionMgr.h"
#include "LoginHandler.h"
#include "ItemHandler.h"

#include "MapHandler.h"
#include "MoveHandler.h"
#include "CombatHandler.h"
#include "NpcHandler.h"
#include "MugongHandler.h"
#include "SlotHandler.h"
#include "CharHandler.h"
#include "ChatHandler.h"
#include "PartyHandler.h"
#include "../GameObjects/DropManager.h"
#include "RebuildItemHandler.h"
#include "BankHandler.h"
#include "StatHandler.h"
#include "TradeHandler.h"
#include "ShopHandler.h"
#include "RepairHandler.h"
#include "MunpaHandler.h"

void InitPacketHandlers() {
    RegisterHandler(CS_IT_LOGINCHECK_REQ, [](SOCKET s, BYTE* p, WORD size) {
        std::string account = SessionMgr::GetInstance().GetAccount(s);
        OnLoginCheckReq(s, account, p, size);
    });

    RegisterHandler(CS_IT_CHARACTERLIST_REQ, [](SOCKET s, BYTE* p, WORD size) {
        std::string account = SessionMgr::GetInstance().GetAccount(s);
        OnCharacterListReq(s, account, p, size);
    });

    RegisterHandler(CS_IT_NEWCHARACTER_REQ, [](SOCKET s, BYTE* p, WORD size) {
        std::string account = SessionMgr::GetInstance().GetAccount(s);
        OnNewCharacterReq(s, account, p, size);
    });

    RegisterHandler(CS_IT_DELCHARACTER_REQ, [](SOCKET s, BYTE* p, WORD size) {
        std::string account = SessionMgr::GetInstance().GetAccount(s);
        OnDelCharacterReq(s, account, p, size);
    });

    RegisterHandler(CS_NV_STARTGAME_REQ, [](SOCKET s, BYTE* p, WORD size) {
        std::string account = SessionMgr::GetInstance().GetAccount(s);
        OnStartGameReq(s, account, p, size);
    });

    RegisterHandler(CS_NV_ENDGAME_REQ, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        OnEndGameReq(s, charID, p, size);
    });

    RegisterHandler(0x4431, [](SOCKET s, BYTE* p, WORD size) { // CS_IT_SETSLOT_REQ
        std::string account = SessionMgr::GetInstance().GetAccount(s);
        OnSetSlotReq(s, account, p, size);
    });


    RegisterHandler(0x4413, [](SOCKET s, BYTE* p, WORD size) { // CS_IT_SACKLIST_REQ (OFFSET_CS_IT + 20 = 0x4414)
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        OnSackItemReq(s, charID, p, size);
    });

    RegisterHandler(0x4419, [](SOCKET s, BYTE* p, WORD size) { // CS_IT_MUGONGLIST_REQ (OFFSET_CS_IT + 24 = 0x4418)
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        OnMugongListReq(s, charID, p, size);
    });

    RegisterHandler(0x3D30, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (charID) OnBuyItemReq(s, charID, p, size);
    });

    RegisterHandler(0x3D58, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (charID) OnSellItemReq(s, charID, p, size);
    });

    RegisterHandler(0x4417, [](SOCKET s, BYTE* p, WORD size) { // CS_IT_ITEMLIST_REQ (OFFSET_CS_IT + 22 = 0x4416)
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        OnItemListReq(s, charID, p, size);
    });

    RegisterHandler(0x442D, [](SOCKET s, BYTE* p, WORD size) { // CS_IT_CHARSLOT_REQ (OFFSET_CS_IT + 44 = 0x442D)
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        SendCharSlotInfoAck(s, charID);
    });

    RegisterHandler(0x420D, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        OnItemMoveReq(s, charID, p, size);
    });

    RegisterHandler(0x4218, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        LOG("[DEBUG] Triggered 0x4218 OnUseItemReq");
        OnUseItemReq(s, charID, p, size);
    });

    RegisterHandler(0x4219, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        LOG("[DEBUG] Triggered 0x4219 OnUseItemReq");
        OnUseItemReq(s, charID, p, size);
    });

    // 遁身符 "记录当前位置" (CS_IM_REMARKITEM_REQ)
    RegisterHandler(0x4249, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (charID) OnRemarkItemReq(s, charID, p, size);
    });

    RegisterHandler(0x4201, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        DropManager::GetInstance()->HandlePickup(s, charID, p, size);
    });

    RegisterHandler(0x4232, [](SOCKET s, BYTE* p, WORD size) {
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        DropManager::GetInstance()->HandlePickup(s, charID, p, size);
    });

    RegisterHandler(0x4203, [](SOCKET s, BYTE* p, WORD size) { // CS_IM_THROW_REQ
        DWORD charID = SessionMgr::GetInstance().GetCharID(s);
        if (charID) OnItemDropReq(s, charID, p, size);
    });

    // Map & Loading Handlers
    RegisterHandler(0x3A55, [](SOCKET s, BYTE* p, WORD size) { OnMapLoadingSequenceReq(s, SessionMgr::GetInstance().GetCharID(s), p, size, 0x3A55); });
    RegisterHandler(0x3203, [](SOCKET s, BYTE* p, WORD size) { OnMapLoadingSequenceReq(s, SessionMgr::GetInstance().GetCharID(s), p, size, 0x3203); });
    RegisterHandler(0x3903, [](SOCKET s, BYTE* p, WORD size) { OnMapLoadingSequenceReq(s, SessionMgr::GetInstance().GetCharID(s), p, size, 0x3903); });
    
    RegisterHandler(0x4413, [](SOCKET s, BYTE* p, WORD size) { 
        OnCharStatusInfoReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); 
    });
    // Silent handler for SPEEDPING_REQ (0x442B) to prevent log flooding every 3 seconds
    RegisterHandler(0x442B, [](SOCKET s, BYTE* p, WORD size) { 
        // We can safely ignore SPEEDPING_REQ or update a last-seen timestamp
    });

    RegisterHandler(0x4305, [](SOCKET s, BYTE* p, WORD size) { OnMapEnterReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x4437, [](SOCKET s, BYTE* p, WORD size) { OnMapInfoReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x4435, [](SOCKET s, BYTE* p, WORD size) { OnImReadyReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x4411, [](SOCKET s, BYTE* p, WORD size) { OnCharInfoListReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x440F, [](SOCKET s, BYTE* p, WORD size) { OnCharInfoReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x352B, [](SOCKET s, BYTE* p, WORD size) { OnNpcInfoReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x352D, [](SOCKET s, BYTE* p, WORD size) { OnNpcInfoListReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3531, [](SOCKET s, BYTE* p, WORD size) { OnFunctionalNpcInfoListReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3533, [](SOCKET s, BYTE* p, WORD size) { OnFunctionalNpcItemListReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });

    // Move Handlers
    RegisterHandler(0x430B, [](SOCKET s, BYTE* p, WORD size) { OnMoveReq(s, SessionMgr::GetInstance().GetCharID(s), p, size, 0x430B); });
    RegisterHandler(0x430D, [](SOCKET s, BYTE* p, WORD size) { OnMoveReq(s, SessionMgr::GetInstance().GetCharID(s), p, size, 0x430D); });
    RegisterHandler(0x430F, [](SOCKET s, BYTE* p, WORD size) { OnMoveReq(s, SessionMgr::GetInstance().GetCharID(s), p, size, 0x430F); });

    // 宠物封印与解封（BONGIN）
    RegisterHandler(0x3547, [](SOCKET s, BYTE* p, WORD size) { OnPetBongInReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3549, [](SOCKET s, BYTE* p, WORD size) { OnPetBongOutReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });

    // Combat Handlers
    RegisterHandler(0x4003, [](SOCKET s, BYTE* p, WORD size) { OnPreAttackReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x4013, [](SOCKET s, BYTE* p, WORD size) { OnMugongPreAttackReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x4015, [](SOCKET s, BYTE* p, WORD size) { OnMugongAttackReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x4023, [](SOCKET s, BYTE* p, WORD size) { OnMugongLearnReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x4017, [](SOCKET s, BYTE* p, WORD size) { OnSelMugongReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x4005, [](SOCKET s, BYTE* p, WORD size) { OnAttackHitReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(0x3A00, [](SOCKET s, BYTE* p, WORD size) { OnSetOptionReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });

    // Stat Point Allocation (Character Window +1 buttons)
    RegisterHandler(0x401D, [](SOCKET s, BYTE* p, WORD size) { StatHandler::OnExecSpReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });

    // Map Move (Death Respawn / Teleport)
    RegisterHandler(0x4307, [](SOCKET s, BYTE* p, WORD size) { OnMapMoveReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });

    // Chat Handlers
    RegisterHandler(CS_CH_CHAT_REQ_ID, [](SOCKET s, BYTE* p, WORD size) { OnChatReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });

    // Party Handlers
    RegisterHandler(PKT_ASKPARTY_REQ, [](SOCKET s, BYTE* p, WORD size) { OnAskPartyReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(PKT_INVITEPARTY_REQ, [](SOCKET s, BYTE* p, WORD size) { OnInvitePartyReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(PKT_LEAVEPARTY_REQ, [](SOCKET s, BYTE* p, WORD size) { OnLeavePartyReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(PKT_BANISHPARTY_REQ, [](SOCKET s, BYTE* p, WORD size) { OnBanishPartyReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });
    RegisterHandler(PKT_PARTYSHARE_REQ, [](SOCKET s, BYTE* p, WORD size) { OnPartyShareReq(s, SessionMgr::GetInstance().GetCharID(s), p, size); });

    RegisterRebuildItemHandlers();
    RegisterBankMallHandlers();
    RegisterTradeHandlers();
    RegisterShopHandlers();
    RegisterRepairHandlers();
    RegisterMunpaHandlers();
}

#pragma once
#include <winsock2.h>
#include <windows.h>

// ============================================================
// Packet Family Offsets
// ============================================================
constexpr WORD OFFSET_CS_MV = 0x4301;  // Movement family
constexpr WORD OFFSET_CS_BT = 0x4001;  // Battle/Combat family
constexpr WORD OFFSET_CS_IT = 0x4401;  // Item/Status family
constexpr WORD OFFSET_CS_IM = 0x4201;  // Inventory Management family
constexpr WORD OFFSET_CS_IF = 0x3B01;  // Info family
constexpr WORD OFFSET_CS_CH = 0x3E01;  // Chat family

// ============================================================
// Login / Auth
// ============================================================
constexpr WORD PKT_LOGINCHECK_ACK      = 0x4414;  // CS_IT_CHARSTATUSINFO_ACK (also login ack)

// ============================================================
// Character Info (CS_IF family, offset 0x3B01)
// ============================================================
constexpr WORD PKT_CHARINFO_REQ        = 0x440F;
constexpr WORD PKT_CHARINFO_ACK        = 0x3B02;  // CS_IF_CHARINFO_ACK
constexpr WORD PKT_CHAREXP_ACK         = 0x3B10;  // CS_IF_CHAREXP_ACK
constexpr WORD PKT_CHARINFOLIST_REQ    = 0x4411;
constexpr WORD PKT_HELPMESSAGE_ACK     = 0x3B3F;  // CS_IF_HELPMESSAGE_ACK
constexpr WORD PKT_EXECSTAMINA_REQ    = 0x3B78;  // CS_IF_EXECSTAMINA_REQ
constexpr WORD PKT_EXECSTAMINA_ACK    = 0x3B79;  // CS_IF_EXECSTAMINA_ACK

// ============================================================
// Character Status (CS_IT family, offset 0x4401)
// ============================================================
constexpr WORD PKT_CHARSTATUSINFO_ACK  = 0x4414;  // CS_IT_CHARSTATUSINFO_ACK
constexpr WORD PKT_SACKLIST_REQ        = 0x4413;
constexpr WORD PKT_ITEMLIST_REQ        = 0x4417;
constexpr WORD PKT_ITEMLIST_ACK        = 0x4418;  // CS_IT_ITEMLIST_ACK
constexpr WORD PKT_MUGONGLIST_REQ      = 0x4419;
constexpr WORD PKT_GENERALMUGONGLIST_ACK  = 0x441A;
constexpr WORD PKT_PASSIVEMUGONGLIST_ACK  = 0x441C;
constexpr WORD PKT_ACTIVEMUGONGLIST_ACK   = 0x441D;
constexpr WORD PKT_CHARSLOT_REQ       = 0x442D;
constexpr WORD PKT_CHARSLOT_ACK       = 0x442E;  // CS_IT_CHARSLOT_ACK
constexpr WORD PKT_SETSLOT_REQ        = 0x4431;
constexpr WORD PKT_SETSLOT_ACK        = 0x4432;  // CS_IT_SETSLOT_ACK
constexpr WORD PKT_IMREADY_REQ        = 0x4435;
constexpr WORD PKT_IMREADY_ACK        = 0x4436;  // CS_IT_IMREADY_ACK
constexpr WORD PKT_MAPINFO_REQ        = 0x4437;
constexpr WORD PKT_MAPINFO_ACK        = 0x4438;  // CS_IT_MAPINFO_ACK

// ============================================================
// Map / Movement (CS_MV family, offset 0x4301)
// ============================================================
constexpr WORD PKT_MAPENTER_REQ        = 0x4305;
constexpr WORD PKT_MAPENTER_ACK        = 0x4306;  // CS_MV_MAPENTER_ACK
constexpr WORD PKT_MAPMOVE_REQ         = 0x4307;
constexpr WORD PKT_MOVE_REQ_430B       = 0x430B;
constexpr WORD PKT_MOVE_REQ_430D       = 0x430D;
constexpr WORD PKT_MOVE_REQ_430F       = 0x430F;

// Map loading sequence (various families)
constexpr WORD PKT_MAPLOAD_3A55        = 0x3A55;
constexpr WORD PKT_MAPLOAD_3A56        = 0x3A56;  // ACK for 3A55
constexpr WORD PKT_MAPLOAD_3A34        = 0x3A34;
constexpr WORD PKT_MAPLOAD_3A35        = 0x3A35;  // ACK for 3A34
constexpr WORD PKT_MAPLOAD_3203        = 0x3203;
constexpr WORD PKT_MAPLOAD_3204        = 0x3204;  // ACK for 3203
constexpr WORD PKT_MAPLOAD_3903        = 0x3903;
constexpr WORD PKT_MAPLOAD_3904        = 0x3904;  // ACK for 3903

// ============================================================
// NPC Info
// ============================================================
constexpr WORD PKT_NPCINFO_REQ         = 0x352B;
constexpr WORD PKT_NPCINFO_ACK         = 0x352C;
constexpr WORD PKT_NPCINFOLIST_REQ     = 0x352D;
constexpr WORD PKT_FUNCNPCINFOLIST_REQ = 0x3531;
constexpr WORD PKT_FUNCNPCITEMLIST_REQ = 0x3533;
constexpr WORD PKT_NPCMOVE_ACK        = 0x3508;  // Monster/NPC movement broadcast

// ============================================================
// Combat (CS_BT family, offset 0x4001)
// ============================================================
constexpr WORD PKT_PREATTACK_REQ       = 0x4003;
constexpr WORD PKT_ATTACKHIT_REQ       = 0x4005;
constexpr WORD PKT_ATTACKHIT_ACK       = 0x4006;  // CS_BT_ATTACKHIT_ACK
constexpr WORD PKT_MUGONGPREATTACK_REQ = 0x4013;
constexpr WORD PKT_MUGONGPREATTACK_ACK = 0x4014;
constexpr WORD PKT_MUGONGATTACK_REQ    = 0x4015;
constexpr WORD PKT_MUGONGATTACK_ACK    = 0x4016;
constexpr WORD PKT_SELMUGONG_REQ       = 0x4017;
constexpr WORD PKT_SELMUGONG_ACK       = 0x4018;
constexpr WORD PKT_EXECSP_REQ          = 0x401D;
constexpr WORD PKT_EXECSP_ACK          = 0x401E;  // CS_BT_EXECSP_ACK
constexpr WORD PKT_LEARNMUGONG_REQ     = 0x4023;
constexpr WORD PKT_LEARNMUGONG_ACK     = 0x4024;

// ============================================================
// Inventory Management (CS_IM family, offset 0x4201)
// ============================================================
constexpr WORD PKT_ITEMMOVE_REQ        = 0x4201;
constexpr WORD PKT_ITEMTHROW_REQ       = 0x4203;
constexpr WORD PKT_REMOVELISTFROMMAP_ACK = 0x4206;
constexpr WORD PKT_REMOVEFROMSACK_ACK = 0x4208;  // CS_IM_REMOVEFROMSACK_ACK
constexpr WORD PKT_ADDONSACK_ACK      = 0x420A;  // CS_IM_ADDONSACK_ACK
constexpr WORD PKT_DROPITEMTOMAP_ACK   = 0x420C;  // CS_IM_DROPITEMTOMAP_ACK
constexpr WORD PKT_PICKUPITEM_REQ      = 0x420D;
constexpr WORD PKT_UPDATESACKITEM_ACK  = 0x420E;  // CS_IM_UPDATESACKITEM_ACK
constexpr WORD PKT_NPCTRADE_REQ        = 0x4218;
constexpr WORD PKT_NPCSELL_REQ         = 0x4219;
constexpr WORD PKT_ITEMMOVEONSACK_REQ  = 0x4232;

// ============================================================
// Rebuild / Reinforcement (CS_IM family)
// ============================================================
constexpr WORD PKT_REBUILDITEMTERM_REQ  = 0x4241;
constexpr WORD PKT_REBUILDITEMTERM_ACK  = 0x4242;
constexpr WORD PKT_REBUILDITEM_REQ     = 0x4243;
constexpr WORD PKT_REBUILDITEM_ACK     = 0x4244;

// ============================================================
// Repair (CS_IM family)
// ============================================================
constexpr WORD PKT_REPAIRITEM_REQ_ID      = 0x4228;  // OFFSET_CS_IM + 39
constexpr WORD PKT_REPAIRITEM_ACK_ID      = 0x4229;  // OFFSET_CS_IM + 40
constexpr WORD PKT_DURABILITY_ACK_ID      = 0x4236;  // OFFSET_CS_IM + 35
constexpr WORD PKT_REPAIRWITHITEM_REQ_ID  = 0x4247;  // OFFSET_CS_IM + 70
constexpr WORD PKT_REPAIRWITHITEM_ACK_ID  = 0x4248;  // OFFSET_CS_IM + 71

// ============================================================
// Bank (CS_NK family)
// ============================================================
constexpr WORD PKT_BANKITEMLIST_REQ     = 0x3D30;
constexpr WORD PKT_BANKITEMLIST2_REQ    = 0x3D58;
constexpr WORD PKT_ADDTOBANK_REQ       = 0x3D5A;
constexpr WORD PKT_REMOVEFROMBANK_REQ  = 0x3D5C;
constexpr WORD PKT_BANKOPEN_REQ        = 0x3D9D;
constexpr WORD PKT_BANKMONEY_REQ       = 0x3DA3;
constexpr WORD PKT_BANKMONEYDRAW_REQ   = 0x3DA5;
constexpr WORD PKT_REMOVEFROMBANK_ACK  = 0x3DA4;

// ============================================================
// Party
// ============================================================
constexpr WORD PKT_PARTYPOSITION_ACK   = 0x442C;  // CS_IT_PARTYPOSITION_ACK (0x4401 + 43)

// ============================================================
// Pet / Info (CS_IF family)
// ============================================================
constexpr WORD PKT_PETLIST_REQ          = 0x3B1F;  // CS_IF_PETLIST_REQ
constexpr WORD PKT_PETDETAILINFO_REQ    = 0x353C;  // CS_NC_PETDETAILINFO_REQ
constexpr WORD PKT_PETDETAILINFO_ACK    = 0x353D;  // CS_NC_PETDETAILINFO_ACK

// ============================================================
// Exit / Status (CS_NV family, offset 0x4301)
// ============================================================
constexpr WORD PKT_ENDGAME_REQ          = 0x4303;  // CS_NV_ENDGAME_REQ
constexpr WORD PKT_ENDGAME_ACK          = 0x4304;  // CS_NV_ENDGAME_ACK

// ============================================================
// Encryption key
// ============================================================
constexpr BYTE ENCRYPT_KEY = 0x42;


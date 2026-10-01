#pragma once

#include "../ServerCore.h"
#include <string>
#include <vector>
#include <map>
#include <set>
#include <mutex>

// =========================================================
// Party Packet IDs (OFFSET_CS_IF = 0x3B01, verified live)
// ASKPARTY_REQ confirmed as 0x3B46 via UNKNOWN PACKET log
// =========================================================
#define PKT_ASKPARTY_REQ        0x3B46  // +69
#define PKT_ASKPARTY_ACK        0x3B47  // +70
#define PKT_CREATEPARTY_ACK     0x3B49  // +72
#define PKT_INVITEPARTY_REQ     0x3B4A  // +73
#define PKT_INVITEPARTY_ACK     0x3B4B  // +74
#define PKT_ENTERPARTY_ACK      0x3B4D  // +76
#define PKT_LEAVEPARTY_REQ      0x3B4E  // +77
#define PKT_LEAVEPARTY_ACK      0x3B4F  // +78
#define PKT_DESTROYPARTY_ACK    0x3B51  // +80
#define PKT_BANISHPARTY_REQ     0x3B52  // +81
#define PKT_BANISHPARTY_ACK     0x3B53  // +82
#define PKT_PARTYPOSITION_ACK   0x3B55  // +84
#define PKT_PARTYLIST_ACK       0x3B57  // +86
#define PKT_PARTYSHARE_REQ      0x3B74  // +115
#define PKT_PARTYSHARE_ACK      0x3B75  // +116

// =========================================================
// Party Handler Functions (packet entry points)
// =========================================================
void OnAskPartyReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize);
void OnInvitePartyReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize);
void OnLeavePartyReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize);
void OnBanishPartyReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize);
void OnPartyShareReq(SOCKET clientSocket, DWORD dwCharID, BYTE* pPayload, WORD wSize);

// =========================================================
// PartyManager - In-memory party system
// =========================================================
#define MAX_PARTY_MEMBERS 8

struct PartyMemberInfo {
    DWORD dwCharID;         // 400M format (from GetCharID)
    DWORD dwObjectID;       // 800M format (map key)
    BYTE  bPriority;        // 0 = leader, 1+ = member
    std::string szName;
    WORD  wLevel;
    WORD  wPosX;
    WORD  wPosY;
    DWORD dwCurHp;
    DWORD dwMaxHp;
    DWORD dwMapID;
};

class PartyManager {
public:
    static PartyManager& GetInstance() {
        static PartyManager instance;
        return instance;
    }

    // Create a new party with the leader. Returns partyID.
    DWORD CreateParty(const PartyMemberInfo& leader);

    // Add a member to an existing party
    bool AddMember(DWORD dwPartyID, const PartyMemberInfo& member);

    // Remove a member. Returns true if party still exists.
    bool RemoveMember(DWORD dwCharID);

    // Get partyID for a character (0 = not in party)
    DWORD GetPartyID(DWORD dwCharID);

    // Get all members of a party
    std::vector<PartyMemberInfo> GetMembers(DWORD dwPartyID);

    // Get the leader's CharID for a party
    DWORD GetLeaderCharID(DWORD dwPartyID);

    // Disband entire party
    void DisbandParty(DWORD dwPartyID);

    // Track pending invites: askerCharID -> askedCharID
    void SetPendingInvite(DWORD dwAskerCharID, DWORD dwAskedCharID, BYTE bPartyType);
    bool GetPendingInvite(DWORD dwAskerCharID, DWORD& outAskedCharID, BYTE& outPartyType);
    void ClearPendingInvite(DWORD dwAskerCharID);

    // EXP/FE sharing mode (0=individual, 1=shared)
    void SetShareMode(DWORD dwPartyID, BYTE byExpMode, BYTE byFEMode);
    BYTE GetExpShareMode(DWORD dwPartyID);
    BYTE GetFEShareMode(DWORD dwPartyID);

    // Broadcast member position/level/HP to other party members
    void BroadcastMemberPosition(DWORD dwCharID, WORD wLevel, DWORD dwHpCur, DWORD dwHpMax, DWORD dwMapID, WORD wPosX, WORD wPosY);

private:
    PartyManager() : m_nextPartyID(1) {}

    std::mutex m_mutex;
    DWORD m_nextPartyID;

    // PartyID -> vector of members
    std::map<DWORD, std::vector<PartyMemberInfo>> m_parties;
    // CharID -> PartyID
    std::map<DWORD, DWORD> m_charToParty;

    // Pending invites: askerCharID -> {askedCharID, partyType}
    struct PendingInvite {
        DWORD dwAskedCharID;
        BYTE bPartyType;
    };
    std::map<DWORD, PendingInvite> m_pendingInvites;

    // Share mode per party: {byExpDivision, byFEDivision}
    // 0 = individual, 1 = shared (default = 1)
    struct ShareMode {
        BYTE byExpDivision = 1; // default: shared
        BYTE byFEDivision = 1;
    };
    std::map<DWORD, ShareMode> m_shareModes;
};

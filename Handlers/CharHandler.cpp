#include "CharHandler.h"
#include "../DB/ItemDB.h"
#include "../DB/CharacterDB.h"
#include "../ServerCore.h"
#include "../XiahClient/csprotocol.h"
#include "../DBHelper.h"

// ============================================================
// Create New Character
// Client sends: BYTE bCharType, sString szNickName
// Server replies: BYTE bResult, DWORD dwObjectID
// ============================================================
void OnNewCharacterReq(SOCKET clientSocket, const std::string& clientAccountName, BYTE* payload, WORD totalSize) {
    LOG("[CharHandler] Received CS_IT_NEWCHARACTER_REQ from account: " + clientAccountName);

    auto sendAck = [&](BYTE bResult, DWORD dwObjectID) {
        std::vector<BYTE> ackBuf(4);
        ackBuf.push_back(bResult);
        ackBuf.push_back((BYTE)(dwObjectID & 0xFF)); ackBuf.push_back((BYTE)((dwObjectID >> 8) & 0xFF));
        ackBuf.push_back((BYTE)((dwObjectID >> 16) & 0xFF)); ackBuf.push_back((BYTE)(dwObjectID >> 24));
        PACKET_HEADER* h = (PACKET_HEADER*)ackBuf.data();
        h->id = CS_IT_NEWCHARACTER_ACK; h->payloadSize = (WORD)(ackBuf.size() - 4);
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    };

    if (totalSize < 1) { sendAck(3, 0); return; }

    BYTE bCharType = payload[0];
    if (bCharType < 1 || bCharType > 4) {
        LOG("[CharHandler] Invalid bCharType: " + std::to_string(bCharType));
        sendAck(3, 0); return;
    }

    // Parse nickname (sString: WORD len + char[len], last byte is \0)
    std::string szNickName = "";
    if (totalSize >= 3) {
        WORD nameLen = *(WORD*)(payload + 1);
        if (nameLen > 0 && totalSize >= (WORD)(3 + nameLen)) {
            szNickName = std::string((char*)(payload + 3), nameLen - 1); // exclude null terminator
        }
    }
    if (szNickName.empty() || szNickName.length() > 20) {
        LOG("[CharHandler] Bad name length: " + std::to_string(szNickName.length()));
        sendAck(5, 0); return; // ERR_ITNEWCHARACTER_INTERNAL = bad name length
    }

    LOG("[CharHandler] Creating character: Name='" + szNickName + "' Type=" + std::to_string(bCharType));

    // 1. Check max 3 characters per account
    int charCount = CharacterDB::GetInstance().CountActiveCharacters(clientAccountName);
    if (charCount >= 3) {
        LOG("[CharHandler] Account already has " + std::to_string(charCount) + " characters");
        sendAck(9, 0); return;
    }

    // 2. Check duplicate nickname
    bool duplicated = CharacterDB::GetInstance().NicknameExists(szNickName);
    if (duplicated) {
        LOG("[CharHandler] Duplicate nickname: " + szNickName);
        sendAck(2, 0); return;
    }

    // 3. Get spawn position from LOCATION
    int wPosX = 412, wPosY = 615;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT TOP 1 wStartPosX, wStartPosY FROM LOCATION WHERE dwMapID = 6",
        [&](SQLHSTMT hStmt) { SQLLEN cb[2]; SQLGetData(hStmt, 1, SQL_C_SSHORT, &wPosX, 0, &cb[0]); SQLGetData(hStmt, 2, SQL_C_SSHORT, &wPosY, 0, &cb[1]); });
    wPosX += rand() % 10; wPosY += rand() % 10;

    // 4. Get default stats from CHAR_DEFAULT
    int wStr = 1, wDex = 1, wVit = 1, wSus = 1, bIncHp = 8, bIncIp = 4;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT wStr, wDex, wVit, wSus, bIncHp, bIncIp FROM CHAR_DEFAULT WHERE bCharType = " + std::to_string(bCharType),
        [&](SQLHSTMT hStmt) {
            SQLLEN cb[6];
            SQLGetData(hStmt, 1, SQL_C_SSHORT, &wStr, 0, &cb[0]); SQLGetData(hStmt, 2, SQL_C_SSHORT, &wDex, 0, &cb[1]);
            SQLGetData(hStmt, 3, SQL_C_SSHORT, &wVit, 0, &cb[2]); SQLGetData(hStmt, 4, SQL_C_SSHORT, &wSus, 0, &cb[3]);
            SQLGetData(hStmt, 5, SQL_C_SLONG, &bIncHp, 0, &cb[4]); SQLGetData(hStmt, 6, SQL_C_SLONG, &bIncIp, 0, &cb[5]);
        });
    int wHp = wVit * bIncHp;
    int wIp = wSus * bIncIp;

    // 5-9. Create all character tables via DAO
    DWORD newCharID = CharacterDB::GetInstance().CreateCharacterRecord(
        clientAccountName, szNickName, bCharType, wStr, wSus, wDex, wVit, wHp, wIp, wPosX, wPosY);
    if (newCharID == 0) {
        LOG("[CharHandler] Failed to insert CHAR_BASIC!");
        sendAck(5, 0); return;
    }
    CharacterDB::GetInstance().InitializeSlot(newCharID);
    LOG("[CharHandler] New CharID from CHAR_BASIC: " + std::to_string(newCharID));

    // 10. Create initial equipment per class
    // Weapon at SackPos=0, Armor at SackPos=2
    struct StartItem { WORD wRefID; BYTE bSackPos; };
    std::vector<StartItem> items;
    switch (bCharType) {
        case 1: items = {{20001, 0}, {20014, 2}}; break;
        case 2: items = {{20010, 0}, {20021, 2}}; break;
        case 3: items = {{20157, 0}, {20161, 2}}; break;
        case 4: items = {{20457, 0}, {20480, 2}}; break;
    }

    for (const auto& si : items) {
        DWORD dwNewItemID = ItemDB::GetInstance().InsertItemFromTemplate(newCharID, si.wRefID, si.bSackPos);
        if (dwNewItemID == 0) { LOG("[CharHandler] Failed to create item wRefID=" + std::to_string(si.wRefID)); continue; }
        LOG("[CharHandler] Created item wRefID=" + std::to_string(si.wRefID) + " dwItemID=" + std::to_string(dwNewItemID) + " at pos=" + std::to_string(si.bSackPos));
    }

    LOG("[CharHandler] Character created! dwCharID=" + std::to_string(newCharID) + " Name=" + szNickName);
    sendAck(0, newCharID); // Success
}

// ============================================================
// Delete Character
// Client sends: DWORD dwCharID (the full 400000XXX ID)
// Server replies: BYTE bResult
// ============================================================
void OnDelCharacterReq(SOCKET clientSocket, const std::string& clientAccountName, BYTE* payload, WORD totalSize) {
    LOG("[CharHandler] Received CS_IT_DELCHARACTER_REQ from account: " + clientAccountName);

    auto sendAck = [&](BYTE bResult) {
        std::vector<BYTE> ackBuf(4);
        ackBuf.push_back(bResult);
        PACKET_HEADER* h = (PACKET_HEADER*)ackBuf.data();
        h->id = CS_IT_DELCHARACTER_ACK; h->payloadSize = (WORD)(ackBuf.size() - 4);
        EncryptPacket(ackBuf.data(), 0x42);
        SafeSend(clientSocket, (const char*)ackBuf.data(), ackBuf.size(), 0);
    };

    if (totalSize < 4) { sendAck(1); return; }

    DWORD dwCharID = *(DWORD*)(payload);
    dwCharID -= 400000000; // Client sends dwObjectID (400000000 + dwCharID), convert back
    LOG("[CharHandler] Deleting CharID: " + std::to_string(dwCharID));

    // Verify this character belongs to this account and is active
    bool ownsChar = CharacterDB::GetInstance().AccountOwnsCharacter(dwCharID, clientAccountName);
    if (!ownsChar) {
        LOG("[CharHandler] Character does not belong to this account or already deleted!");
        sendAck(1); return;
    }

    // Soft delete: mark bActive = 9 (same as original game design)
    // The CHAR_VISUAL view filters bActive=9, so the character disappears from the list immediately.
    // The Delete_Character stored procedure handles the actual cascade data cleanup later.
    bool ok = CharacterDB::GetInstance().SoftDeleteCharacter(dwCharID, clientAccountName);

    if (ok) {
        LOG("[CharHandler] Character " + std::to_string(dwCharID) + " soft-deleted (bActive=9) successfully.");
        sendAck(0); // Success
    } else {
        LOG("[CharHandler] Failed to soft-delete character " + std::to_string(dwCharID));
        sendAck(1); // Failure
    }
}

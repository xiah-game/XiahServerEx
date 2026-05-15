#include "CharHandler.h"
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
    int charCount = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT COUNT(*) FROM CHAR_ACCOUNT WHERE szAccount = '" + clientAccountName + "' AND bActive = 1",
        [&](SQLHSTMT hStmt) { SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_SLONG, &charCount, 0, &cb); });
    if (charCount >= 3) {
        LOG("[CharHandler] Account already has " + std::to_string(charCount) + " characters");
        sendAck(9, 0); return;
    }

    // 2. Check duplicate nickname
    bool duplicated = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwCharID FROM CHAR_BASIC WHERE szNickName = '" + szNickName + "'",
        [&](SQLHSTMT hStmt) { duplicated = true; });
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

    // 5. Insert CHAR_BASIC, then query @@IDENTITY separately (ODBC doesn't support multi-statement)
    int dwBirthDate = 0;
    DWORD newCharID = 0;
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_BASIC (szNickName, bCharType, dwBirthDate, dwFatherID, dwMotherID, dwSpouseID, dwFamilyID, dwSchoolID, dwTeacherOrder, dwSchoolOrder, dwSchoolDepth, dwMunpaID, dwMunpaOrder, DateConnect) "
        "VALUES ('" + szNickName + "', " + std::to_string(bCharType) + ", " + std::to_string(dwBirthDate) + ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, GETDATE())");
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT @@IDENTITY AS NewID",
        [&](SQLHSTMT hStmt) { SQLLEN cb; int id = 0; SQLGetData(hStmt, 1, SQL_C_SLONG, &id, 0, &cb); newCharID = (DWORD)id; });

    if (newCharID == 0) {
        LOG("[CharHandler] Failed to insert CHAR_BASIC!");
        sendAck(5, 0); return;
    }
    LOG("[CharHandler] New CharID from CHAR_BASIC: " + std::to_string(newCharID));

    // 6. Insert CHAR_ACCOUNT
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_ACCOUNT (szAccount, dwCharID, dateCreate, dateReg, bActive) "
        "VALUES ('" + clientAccountName + "', " + std::to_string(newCharID) + ", GETDATE(), GETDATE(), 1)");

    // 7. Insert CHAR_STATUS
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_STATUS (dwCharID, dwMapID, wPosX, wPosY, bHeight, bStatusFlag, dwPkCnt, dwDieCnt, dwFlagInform, bTraining, dwConnecting) "
        "VALUES (" + std::to_string(newCharID) + ", 6, " + std::to_string(wPosX) + ", " + std::to_string(wPosY) + ", 0, 0, 0, 0, 0, 0, 0)");

    // 8. Insert CHAR_POWER
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_POWER (dwCharID, wLevel, wTpLevel, wStr, wSus, wDex, wVit, dwHpCur, dwHpMax, wIpCur, wIpMax, dwTotalTp, dwTotalSp, wRemainTp, wRemainSp, dwExp, dwMoney) "
        "VALUES (" + std::to_string(newCharID) + ", 1, 0, " + std::to_string(wStr) + ", " + std::to_string(wSus) + ", " + std::to_string(wDex) + ", " + std::to_string(wVit) + ", "
        + std::to_string(wHp) + ", " + std::to_string(wHp) + ", " + std::to_string(wIp) + ", " + std::to_string(wIp) + ", 0, 0, 0, 0, 0, 0)");

    // 9. Insert CHAR_OPTION, CHAR_SLOT, CHAR_RANK
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_OPTION (dwCharID, bWisperFlag, bRelationFlag, bTradeFlag) VALUES (" + std::to_string(newCharID) + ", 1, 1, 1)");
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_SLOT VALUES(" + std::to_string(newCharID) + ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)");
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_RANK (dwCharID, szCharName) VALUES(" + std::to_string(newCharID) + ", '" + szNickName + "')");

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
        // Step A: Insert into ITEM from template
        DBHelper::GetInstance().ExecuteUpdate(
            "INSERT INTO ITEM (wRefID, bType, bKind, wVisualID, szName, dwCost, wLevel, bCharType, wAmount, nBasicData1, nBasicData2, nBasicData3, nBasicData4, nBasicData5) "
            "SELECT wRefID, bType, bKind, wVisualID, szName, 0, wLevel, bCharType, 1, nBasicData1, nBasicData2, nBasicData3, nBasicData4, nBasicData5 "
            "FROM ITEMTEMPLATE WHERE wRefID = " + std::to_string(si.wRefID));
        // Step B: Get the new dwItemID
        DWORD dwNewItemID = 0;
        DBHelper::GetInstance().ExecuteQuery(
            "SELECT @@IDENTITY AS NewItemID",
            [&](SQLHSTMT hStmt) { SQLLEN cb; int id = 0; SQLGetData(hStmt, 1, SQL_C_SLONG, &id, 0, &cb); dwNewItemID = (DWORD)id; });
        if (dwNewItemID == 0) { LOG("[CharHandler] Failed to create item wRefID=" + std::to_string(si.wRefID)); continue; }
        // Step C: Insert ITEMDATA
        DBHelper::GetInstance().ExecuteUpdate(
            "INSERT INTO ITEMDATA (dwItemID, nData1, nData2, nData3, nData4, nData5, nData6, nData7, nData8) "
            "SELECT " + std::to_string(dwNewItemID) + ", nData1, nData2, nData3, nData4, nData5, nData6, nData7, nData8 FROM ITEMTEMPLATE WHERE wRefID = " + std::to_string(si.wRefID));
        // Step D: Insert SACKITEM
        DBHelper::GetInstance().ExecuteUpdate(
            "INSERT INTO SACKITEM (dwCharID, bSackPos, dwItemID) VALUES (" + std::to_string(newCharID) + ", " + std::to_string(si.bSackPos) + ", " + std::to_string(dwNewItemID) + ")");
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
    bool ownsChar = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwCharID FROM CHAR_ACCOUNT WHERE dwCharID = " + std::to_string(dwCharID) + " AND szAccount = '" + clientAccountName + "' AND bActive = 1",
        [&](SQLHSTMT hStmt) { ownsChar = true; });
    if (!ownsChar) {
        LOG("[CharHandler] Character does not belong to this account or already deleted!");
        sendAck(1); return;
    }

    // Soft delete: mark bActive = 9 (same as original game design)
    // The CHAR_VISUAL view filters bActive=9, so the character disappears from the list immediately.
    // The Delete_Character stored procedure handles the actual cascade data cleanup later.
    bool ok = DBHelper::GetInstance().ExecuteUpdate(
        "UPDATE CHAR_ACCOUNT SET bActive = 9, dateReg = GETDATE() WHERE dwCharID = " + std::to_string(dwCharID) + " AND szAccount = '" + clientAccountName + "'");

    if (ok) {
        LOG("[CharHandler] Character " + std::to_string(dwCharID) + " soft-deleted (bActive=9) successfully.");
        sendAck(0); // Success
    } else {
        LOG("[CharHandler] Failed to soft-delete character " + std::to_string(dwCharID));
        sendAck(1); // Failure
    }
}

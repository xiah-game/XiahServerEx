#include "CharacterDB.h"
#include "../DBHelper.h"
#include "../ServerCore.h"

bool CharacterDB::GetCharData(DWORD dwCharID, CharPower& out) {
    std::string q = "SELECT wLevel, wStr, wSus, wDex, wVit, wIpMax, wIpCur, dwHpMax, dwHpCur, "
                    "dwExp, dwTotalSp, wRemainSp, dwTotalTp, wRemainTp, dwMoney, dwFame, bCharType "
                    "FROM CHAR_DATA WHERE dwCharID = " + std::to_string(dwCharID);
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c;
        LONG tmp;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &tmp, 0, &c); out.wLevel = (WORD)tmp;
        SQLGetData(hStmt, 2, SQL_C_SLONG, &tmp, 0, &c); out.wStr = (WORD)tmp;
        SQLGetData(hStmt, 3, SQL_C_SLONG, &tmp, 0, &c); out.wSus = (WORD)tmp;
        SQLGetData(hStmt, 4, SQL_C_SLONG, &tmp, 0, &c); out.wDex = (WORD)tmp;
        SQLGetData(hStmt, 5, SQL_C_SLONG, &tmp, 0, &c); out.wVit = (WORD)tmp;
        SQLGetData(hStmt, 6, SQL_C_SLONG, &tmp, 0, &c); out.wIpMax = (WORD)tmp;
        SQLGetData(hStmt, 7, SQL_C_SLONG, &tmp, 0, &c); out.wIpCur = (WORD)tmp;
        SQLGetData(hStmt, 8, SQL_C_ULONG, &out.dwHpMax, 0, &c);
        SQLGetData(hStmt, 9, SQL_C_ULONG, &out.dwHpCur, 0, &c);
        SQLGetData(hStmt, 10, SQL_C_SBIGINT, &out.dwExp, 0, &c);
        SQLGetData(hStmt, 11, SQL_C_ULONG, &out.dwTotalSp, 0, &c);
        SQLGetData(hStmt, 12, SQL_C_SLONG, &tmp, 0, &c); out.wRemainSp = (WORD)tmp;
        SQLGetData(hStmt, 13, SQL_C_ULONG, &out.dwTotalTp, 0, &c);
        SQLGetData(hStmt, 14, SQL_C_SLONG, &tmp, 0, &c); out.wRemainTp = (WORD)tmp;
        SQLGetData(hStmt, 15, SQL_C_ULONG, &out.dwMoney, 0, &c);
        SQLGetData(hStmt, 16, SQL_C_ULONG, &out.dwFame, 0, &c);
        SQLGetData(hStmt, 17, SQL_C_SLONG, &tmp, 0, &c); out.bCharType = (BYTE)tmp;
        found = true;
    });
    return found;
}

bool CharacterDB::GetCharBasic(DWORD dwCharID, CharBasic& out) {
    std::string q = "SELECT dwCharID, bCharType, szCharName FROM CHAR_BASIC WHERE dwCharID = " + std::to_string(dwCharID);
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c;
        char nameBuf[64] = {0};
        SQLGetData(hStmt, 1, SQL_C_ULONG, &out.dwCharID, 0, &c);
        SQLGetData(hStmt, 2, SQL_C_UTINYINT, &out.bCharType, 0, &c);
        SQLGetData(hStmt, 3, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c);
        out.szCharName = nameBuf;
        found = true;
    });
    return found;
}

void CharacterDB::UpdateStatPoints(DWORD dwCharID, WORD wStr, WORD wSus, WORD wDex, WORD wVit, WORD wRemainSp) {
    std::string q = "UPDATE CHAR_POWER SET wStr=" + std::to_string(wStr)
        + ", wSus=" + std::to_string(wSus)
        + ", wDex=" + std::to_string(wDex)
        + ", wVit=" + std::to_string(wVit)
        + ", wRemainSp=" + std::to_string(wRemainSp)
        + " WHERE dwCharID=" + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::SetMoney(DWORD dwCharID, DWORD dwMoney) {
    std::string q = "UPDATE CHAR_DATA SET dwMoney = " + std::to_string(dwMoney) + " WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::AddMoney(DWORD dwCharID, DWORD amount) {
    std::string q = "UPDATE CHAR_DATA SET dwMoney = dwMoney + " + std::to_string(amount) + " WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

INT64 CharacterDB::GetMoney(DWORD dwCharID) {
    INT64 money = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwMoney FROM CHAR_DATA WHERE dwCharID = " + std::to_string(dwCharID),
        [&](SQLHSTMT hStmt) { SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_SBIGINT, &money, 0, &cb); });
    return money;
}

void CharacterDB::SubtractMoney(DWORD dwCharID, DWORD amount) {
    std::string q = "UPDATE CHAR_DATA SET dwMoney = dwMoney - " + std::to_string(amount) + " WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::UpdateHpIp(DWORD dwCharID, DWORD dwHpCur, WORD wIpCur) {
    std::string q = "UPDATE CHAR_POWER SET dwHpCur=" + std::to_string(dwHpCur)
        + ", wIpCur=" + std::to_string(wIpCur)
        + " WHERE dwCharID=" + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::RestoreHpIpToMax(DWORD dwCharID) {
    std::string q = "UPDATE CHAR_POWER SET dwHpCur = dwHpMax, wIpCur = "
                    "(SELECT P2.wIpMax FROM CHAR_POWER P2 WHERE P2.dwCharID = CHAR_POWER.dwCharID) "
                    "WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::UpdateExpAndLevel(DWORD dwCharID, long long int newExp, WORD wLevel,
                                     WORD wRemainSp, DWORD dwTotalSp,
                                     WORD wRemainTp, DWORD dwTotalTp,
                                     bool levelUp, bool tpUp) {
    std::string q = "UPDATE CHAR_POWER SET dwExp = " + std::to_string(newExp);
    if (levelUp) q += ", wLevel = " + std::to_string(wLevel);
    if (tpUp) {
        q += ", wRemainTp = " + std::to_string(wRemainTp);
        q += ", dwTotalTp = " + std::to_string(dwTotalTp);
    }
    if (levelUp) {
        q += ", wRemainSp = " + std::to_string(wRemainSp);
        q += ", dwTotalSp = " + std::to_string(dwTotalSp);
    }
    q += " WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

bool CharacterDB::GetExpData(DWORD dwCharID, ExpData& out) {
    std::string q = "SELECT P.dwExp, P.wLevel, P.dwTotalTp, P.wRemainTp, P.wRemainSp, P.dwTotalSp, B.bCharType "
                    "FROM CHAR_POWER P INNER JOIN CHAR_BASIC B ON P.dwCharID = B.dwCharID "
                    "WHERE P.dwCharID = " + std::to_string(dwCharID);
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_SBIGINT, &out.dwExp, 0, &c);
        SQLGetData(hStmt, 2, SQL_C_USHORT, &out.wLevel, 0, &c);
        SQLGetData(hStmt, 3, SQL_C_ULONG, &out.dwTotalTp, 0, &c);
        SQLGetData(hStmt, 4, SQL_C_USHORT, &out.wRemainTp, 0, &c);
        SQLGetData(hStmt, 5, SQL_C_USHORT, &out.wRemainSp, 0, &c);
        SQLGetData(hStmt, 6, SQL_C_ULONG, &out.dwTotalSp, 0, &c);
        SQLGetData(hStmt, 7, SQL_C_UTINYINT, &out.bCharType, 0, &c);
        found = true;
    });
    return found;
}

void CharacterDB::SavePosition(DWORD dwCharID, WORD wPosX, WORD wPosY, DWORD dwMapID) {
    std::string q = "UPDATE CHAR_STATUS SET wPosX=" + std::to_string(wPosX)
        + ", wPosY=" + std::to_string(wPosY)
        + ", dwMapID=" + std::to_string(dwMapID)
        + " WHERE dwCharID=" + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::GetCharacterList(const std::string& accountName, std::vector<CharListEntry>& out) {
    std::string q = "SELECT B.dwCharID, B.szCharName, B.bCharType, P.wLevel, P.wStr, P.wSus, P.wDex, P.wVit, S.dwMapID, S.wPosX, S.wPosY "
                    "FROM CHAR_BASIC B "
                    "INNER JOIN CHAR_POWER P ON B.dwCharID = P.dwCharID "
                    "INNER JOIN CHAR_STATUS S ON B.dwCharID = S.dwCharID "
                    "WHERE B.szAccountName = '" + accountName + "'";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        CharListEntry e;
        SQLLEN c;
        LONG tmp;
        char nameBuf[64] = {0};
        SQLGetData(hStmt, 1, SQL_C_ULONG, &e.dwCharID, 0, &c);
        SQLGetData(hStmt, 2, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c);
        e.szCharName = nameBuf;
        SQLGetData(hStmt, 3, SQL_C_UTINYINT, &e.bCharType, 0, &c);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &tmp, 0, &c); e.wLevel = (WORD)tmp;
        SQLGetData(hStmt, 5, SQL_C_SLONG, &tmp, 0, &c); e.wStr = (WORD)tmp;
        SQLGetData(hStmt, 6, SQL_C_SLONG, &tmp, 0, &c); e.wSus = (WORD)tmp;
        SQLGetData(hStmt, 7, SQL_C_SLONG, &tmp, 0, &c); e.wDex = (WORD)tmp;
        SQLGetData(hStmt, 8, SQL_C_SLONG, &tmp, 0, &c); e.wVit = (WORD)tmp;
        SQLGetData(hStmt, 9, SQL_C_ULONG, &e.dwMapID, 0, &c);
        SQLGetData(hStmt, 10, SQL_C_SLONG, &tmp, 0, &c); e.wPosX = (WORD)tmp;
        SQLGetData(hStmt, 11, SQL_C_SLONG, &tmp, 0, &c); e.wPosY = (WORD)tmp;
        out.push_back(e);
    });
}

void CharacterDB::DeductTP(DWORD dwCharID, WORD amount) {
    std::string q = "UPDATE CHAR_POWER SET wRemainTp = wRemainTp - " + std::to_string(amount)
        + " WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::DeductTpFromCharData(DWORD dwCharID, DWORD amount) {
    std::string q = "UPDATE CHAR_DATA SET wRemainTp = wRemainTp - " + std::to_string(amount)
        + " WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::InsertMugong(DWORD dwCharID, DWORD dwMugongID, BYTE bLevel) {
    std::string q = "INSERT INTO CHAR_MUGONG (dwCharID, dwMugongID, bMugongLevel) VALUES ("
        + std::to_string(dwCharID) + ", " + std::to_string(dwMugongID) + ", " + std::to_string(bLevel) + ")";
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::UpdateMugongLevel(DWORD dwCharID, DWORD dwMugongID, BYTE bLevel) {
    std::string q = "UPDATE CHAR_MUGONG SET bMugongLevel = " + std::to_string(bLevel)
        + " WHERE dwCharID = " + std::to_string(dwCharID) + " AND dwMugongID = " + std::to_string(dwMugongID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::InitializeSlot(DWORD dwCharID) {
    std::string q = "INSERT INTO CHAR_SLOT (dwCharID, dwValue1, dwValue2, dwValue3, dwValue4, dwValue5, dwValue6, dwValue7, dwValue8, dwValue9, dwValue10) VALUES ("
        + std::to_string(dwCharID) + ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0)";
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void CharacterDB::SetSlotValue(DWORD dwCharID, BYTE bSlot, DWORD dwValue) {
    if (bSlot < 1 || bSlot > 10) return;
    std::string q = "UPDATE CHAR_SLOT SET dwValue" + std::to_string(bSlot) + " = " + std::to_string(dwValue)
        + " WHERE dwCharID = " + std::to_string(dwCharID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}
int CharacterDB::CountActiveCharacters(const std::string& accountName) {
    int count = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT COUNT(*) FROM CHAR_ACCOUNT WHERE szAccount = '" + accountName + "' AND bActive = 1",
        [&](SQLHSTMT hStmt) { SQLLEN cb; SQLGetData(hStmt, 1, SQL_C_SLONG, &count, 0, &cb); });
    return count;
}

bool CharacterDB::NicknameExists(const std::string& nickName) {
    bool exists = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwCharID FROM CHAR_BASIC WHERE szNickName = '" + nickName + "'",
        [&](SQLHSTMT hStmt) { exists = true; });
    return exists;
}

bool CharacterDB::AccountOwnsCharacter(DWORD dwCharID, const std::string& accountName) {
    bool owns = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwCharID FROM CHAR_ACCOUNT WHERE dwCharID = " + std::to_string(dwCharID) + " AND szAccount = '" + accountName + "' AND bActive = 1",
        [&](SQLHSTMT hStmt) { owns = true; });
    return owns;
}

bool CharacterDB::SoftDeleteCharacter(DWORD dwCharID, const std::string& accountName) {
    return DBHelper::GetInstance().ExecuteUpdate(
        "UPDATE CHAR_ACCOUNT SET bActive = 9, dateReg = GETDATE() WHERE dwCharID = " + std::to_string(dwCharID) + " AND szAccount = '" + accountName + "'");
}

DWORD CharacterDB::CreateCharacterRecord(const std::string& accountName, const std::string& nickName, BYTE bCharType,
                                          WORD wStr, WORD wSus, WORD wDex, WORD wVit,
                                          int wHp, int wIp, WORD wPosX, WORD wPosY) {
    // Insert CHAR_BASIC
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_BASIC (szNickName, bCharType, dwBirthDate, dwFatherID, dwMotherID, dwSpouseID, dwFamilyID, dwSchoolID, dwTeacherOrder, dwSchoolOrder, dwSchoolDepth, dwMunpaID, dwMunpaOrder, DateConnect) "
        "VALUES ('" + nickName + "', " + std::to_string(bCharType) + ", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, GETDATE())");
    DWORD newCharID = 0;
    DBHelper::GetInstance().ExecuteQuery("SELECT @@IDENTITY AS NewID",
        [&](SQLHSTMT hStmt) { SQLLEN cb; int id = 0; SQLGetData(hStmt, 1, SQL_C_SLONG, &id, 0, &cb); newCharID = (DWORD)id; });
    if (newCharID == 0) return 0;
    // Insert CHAR_ACCOUNT
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_ACCOUNT (szAccount, dwCharID, dateCreate, dateReg, bActive) VALUES ('" + accountName + "', " + std::to_string(newCharID) + ", GETDATE(), GETDATE(), 1)");
    // Insert CHAR_STATUS
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_STATUS (dwCharID, dwMapID, wPosX, wPosY, bHeight, bStatusFlag, dwPkCnt, dwDieCnt, dwFlagInform, bTraining, dwConnecting) VALUES (" + std::to_string(newCharID) + ", 6, " + std::to_string(wPosX) + ", " + std::to_string(wPosY) + ", 0, 0, 0, 0, 0, 0, 0)");
    // Insert CHAR_POWER
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_POWER (dwCharID, wLevel, wTpLevel, wStr, wSus, wDex, wVit, dwHpCur, dwHpMax, wIpCur, wIpMax, dwTotalTp, dwTotalSp, wRemainTp, wRemainSp, dwExp, dwMoney) VALUES (" + std::to_string(newCharID) + ", 1, 0, " + std::to_string(wStr) + ", " + std::to_string(wSus) + ", " + std::to_string(wDex) + ", " + std::to_string(wVit) + ", " + std::to_string(wHp) + ", " + std::to_string(wHp) + ", " + std::to_string(wIp) + ", " + std::to_string(wIp) + ", 0, 0, 0, 0, 0, 0)");
    // Insert CHAR_OPTION
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_OPTION (dwCharID, bWisperFlag, bRelationFlag, bTradeFlag) VALUES (" + std::to_string(newCharID) + ", 1, 1, 1)");
    // Insert CHAR_RANK
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO CHAR_RANK (dwCharID, szCharName) VALUES(" + std::to_string(newCharID) + ", '" + nickName + "')");
    return newCharID;
}

void CharacterDB::GetSpawnPosition(DWORD dwMapID, int& wPosX, int& wPosY) {
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT TOP 1 wStartPosX, wStartPosY FROM LOCATION WHERE dwMapID = " + std::to_string(dwMapID),
        [&](SQLHSTMT hStmt) { SQLLEN cb[2]; SQLGetData(hStmt, 1, SQL_C_SSHORT, &wPosX, 0, &cb[0]); SQLGetData(hStmt, 2, SQL_C_SSHORT, &wPosY, 0, &cb[1]); });
}

bool CharacterDB::GetCharDefault(BYTE bCharType, CharDefault& out) {
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT wStr, wDex, wVit, wSus, bIncHp, bIncIp FROM CHAR_DEFAULT WHERE bCharType = " + std::to_string(bCharType),
        [&](SQLHSTMT hStmt) {
            found = true; SQLLEN cb[6];
            SQLGetData(hStmt, 1, SQL_C_SSHORT, &out.wStr, 0, &cb[0]); SQLGetData(hStmt, 2, SQL_C_SSHORT, &out.wDex, 0, &cb[1]);
            SQLGetData(hStmt, 3, SQL_C_SSHORT, &out.wVit, 0, &cb[2]); SQLGetData(hStmt, 4, SQL_C_SSHORT, &out.wSus, 0, &cb[3]);
            SQLGetData(hStmt, 5, SQL_C_SLONG, &out.bIncHp, 0, &cb[4]); SQLGetData(hStmt, 6, SQL_C_SLONG, &out.bIncIp, 0, &cb[5]);
        });
    return found;
}

bool CharacterDB::GetSlotValues(DWORD dwCharID, std::vector<DWORD>& slots) {
    bool found = false;
    slots.resize(10, 0);
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwValue1, dwValue2, dwValue3, dwValue4, dwValue5, dwValue6, dwValue7, dwValue8, dwValue9, dwValue10 FROM CHAR_SLOT WHERE dwCharID = " + std::to_string(dwCharID),
        [&](SQLHSTMT hStmt) {
            found = true; SQLLEN c;
            for (int i = 0; i < 10; ++i) { SQLGetData(hStmt, i + 1, SQL_C_ULONG, &slots[i], 0, &c); }
        });
    return found;
}

DWORD CharacterDB::AuthenticateUser(const std::string& username, const std::string& password,
                                     const std::string& dbAccount, bool& accountExists, bool& passMatch) {
    DWORD accountId = 0;
    accountExists = false;
    passMatch = false;
    std::string query = "SELECT id, CASE WHEN szPasswd = '" + password + "' THEN 1 ELSE 0 END FROM " + dbAccount + ".dbo.Account WHERE szAccount = '" + username + "'";
    DBHelper::GetInstance().ExecuteQuery(query, [&](SQLHSTMT hStmt) {
        SQLLEN cbId = 0, cbMatch = 0;
        SQLGetData(hStmt, 1, SQL_C_ULONG, &accountId, 0, &cbId);
        int match = 0;
        SQLGetData(hStmt, 2, SQL_C_LONG, &match, 0, &cbMatch);
        if (match == 1) passMatch = true;
        accountExists = true;
    });
    return accountId;
}

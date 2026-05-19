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

bool CharacterDB::GetCharPosition(DWORD dwCharID, int& wPosX, int& wPosY, int& bHeight) {
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT wPosX, wPosY, bHeight FROM CHAR_STATUS WHERE dwCharID = " + std::to_string(dwCharID),
        [&](SQLHSTMT hStmt) {
            SQLLEN cb1, cb2, cb3;
            SQLGetData(hStmt, 1, SQL_C_SLONG, &wPosX, 0, &cb1);
            SQLGetData(hStmt, 2, SQL_C_SLONG, &wPosY, 0, &cb2);
            SQLGetData(hStmt, 3, SQL_C_SLONG, &bHeight, 0, &cb3);
            found = true;
        });
    return found;
}

bool CharacterDB::GetCharVisual(DWORD dwCharID, std::string& szNickName, BYTE& bCharType) {
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT szNickName, bCharType FROM CHAR_VISUAL WHERE dwCharID = " + std::to_string(dwCharID),
        [&](SQLHSTMT hStmt) {
            char szNameBuf[64] = {0}; SQLLEN cbName = 0, cbType = 0;
            SQLGetData(hStmt, 1, SQL_C_CHAR, szNameBuf, sizeof(szNameBuf), &cbName);
            char ct = 0;
            SQLGetData(hStmt, 2, SQL_C_STINYINT, &ct, 0, &cbType);
            if (cbName != SQL_NULL_DATA) szNickName = szNameBuf;
            bCharType = (BYTE)ct;
            found = true;
        });
    return found;
}

DWORD CharacterDB::GetCharMapID(DWORD dwCharID) {
    DWORD mapID = 6; // default
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwMapID FROM CHAR_STATUS WHERE dwCharID = " + std::to_string(dwCharID),
        [&](SQLHSTMT hStmt) {
            int m = 0; SQLLEN cb;
            SQLGetData(hStmt, 1, SQL_C_SLONG, &m, 0, &cb);
            if (cb != SQL_NULL_DATA) mapID = (DWORD)m;
        });
    return mapID;
}

std::string CharacterDB::GetAccountName(DWORD dwCharID) {
    std::string result;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT szAccount FROM CHAR_ACCOUNT WHERE dwCharID = " + std::to_string(dwCharID),
        [&](SQLHSTMT hStmt) {
            char buf[64] = {0}; SQLLEN cb;
            if (SQL_SUCCEEDED(SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &cb)) && cb != SQL_NULL_DATA) {
                result = buf;
            }
        });
    return result;
}

int CharacterDB::GetMugongLevel(DWORD dwCharID, DWORD dwMugongID) {
    int level = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT bMugongLevel FROM CHAR_MUGONG WHERE dwCharID = " + std::to_string(dwCharID) + " AND dwMugongID = " + std::to_string(dwMugongID),
        [&](SQLHSTMT hStmt) { SQLLEN c; SQLGetData(hStmt, 1, SQL_C_SLONG, &level, 0, &c); });
    return level;
}

bool CharacterDB::GetCharSelectList(const std::string& accountName, std::vector<CharSelectEntry>& out) {
    std::string q = "SELECT TOP 3 dwCharID, szNickName, bCharType, dwBirthDate, dwMapID, wLevel, dwHpCur, dwHpMax, wIpCur, wIpMax, wVit, wStr, wSus, wDex, bRebirth FROM CHAR_VISUAL WHERE szAccount = '" + accountName + "' ORDER BY dwCharID ASC";
    bool ok = DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        CharSelectEntry d; SQLLEN cb[15];
        SQLGetData(hStmt, 1, SQL_C_SLONG, &d.dwCharID, 0, &cb[0]);
        SQLGetData(hStmt, 2, SQL_C_CHAR, d.szNickName, sizeof(d.szNickName), &cb[1]);
        SQLGetData(hStmt, 3, SQL_C_STINYINT, &d.bCharType, 0, &cb[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &d.dwBirthDate, 0, &cb[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &d.dwMapID, 0, &cb[4]);
        SQLGetData(hStmt, 6, SQL_C_SSHORT, &d.wLevel, 0, &cb[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &d.dwHpCur, 0, &cb[6]);
        SQLGetData(hStmt, 8, SQL_C_SLONG, &d.dwHpMax, 0, &cb[7]);
        SQLGetData(hStmt, 9, SQL_C_SSHORT, &d.wIpCur, 0, &cb[8]);
        SQLGetData(hStmt, 10, SQL_C_SSHORT, &d.wIpMax, 0, &cb[9]);
        SQLGetData(hStmt, 11, SQL_C_SSHORT, &d.wVit, 0, &cb[10]);
        SQLGetData(hStmt, 12, SQL_C_SSHORT, &d.wStr, 0, &cb[11]);
        SQLGetData(hStmt, 13, SQL_C_SSHORT, &d.wSus, 0, &cb[12]);
        SQLGetData(hStmt, 14, SQL_C_SSHORT, &d.wDex, 0, &cb[13]);
        SQLGetData(hStmt, 15, SQL_C_STINYINT, &d.bRebirth, 0, &cb[14]);
        out.push_back(d);
    });
    if (!ok) return false;
    for (auto& ch : out) {
        DBHelper::GetInstance().ExecuteQuery(
            "SELECT bSackPos, wVisualID FROM vCHAR_EQUIPITEM WHERE szAccount = '" + accountName + "' AND dwCharID = " + std::to_string(ch.dwCharID),
            [&](SQLHSTMT hStmt) {
                char pos = 0; short vis = 0; SQLLEN c1, c2;
                SQLGetData(hStmt, 1, SQL_C_STINYINT, &pos, 0, &c1);
                if (c1 == SQL_NULL_DATA) pos = 0;
                SQLGetData(hStmt, 2, SQL_C_SSHORT, &vis, 0, &c2);
                if (c2 == SQL_NULL_DATA) vis = 0;
                if (pos >= 0 && pos < 9) ch.items[pos].wVis = vis;
            });
        DBHelper::GetInstance().ExecuteQuery(
            "SELECT bSackPos, bStxType, bRarity FROM vCHAR_EQUIPITEMDATA WHERE szAccount = '" + accountName + "' AND dwCharID = " + std::to_string(ch.dwCharID),
            [&](SQLHSTMT hStmt) {
                char pos = 0, stx = 0, rar = 0; SQLLEN c1, c2, c3;
                SQLGetData(hStmt, 1, SQL_C_STINYINT, &pos, 0, &c1);
                SQLGetData(hStmt, 2, SQL_C_STINYINT, &stx, 0, &c2);
                if (c2 == SQL_NULL_DATA) stx = 0;
                SQLGetData(hStmt, 3, SQL_C_STINYINT, &rar, 0, &c3);
                if (c3 == SQL_NULL_DATA) rar = 0;
                if (pos >= 0 && pos < 9) { ch.items[pos].bStx = stx; ch.items[pos].bRar = rar; }
            });
    }
    return true;
}

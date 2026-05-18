#include "CharacterDB.h"
#include "../DBHelper.h"
#include "../ServerCore.h"

bool CharacterDB::GetCharData(DWORD dwCharID, CharPower& out) {
    std::string q = "SELECT wLevel, wStr, wSus, wDex, wVit, wIpMax, wIpCur, dwHpMax, dwHpCur, "
                    "dwExp, dwTotalSp, wRemainSp, dwTotalTp, wRemainTp, dwMoney, dwFame "
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

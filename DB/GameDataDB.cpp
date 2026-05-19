#include "GameDataDB.h"

bool GameDataDB::GetMapInfo(DWORD dwMapID, MapInfo& out) {
    bool found = false;
    std::string q = "SELECT szName, wWidth, wHeight, bType FROM LINKMAP WHERE dwMapID = " + std::to_string(dwMapID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        char name[256] = {0}; SQLLEN cb1, cb2, cb3, cb4;
        SQLGetData(hStmt, 1, SQL_C_CHAR, name, sizeof(name), &cb1);
        if (cb1 != SQL_NULL_DATA) out.szName = name;
        int w, h, t;
        SQLGetData(hStmt, 2, SQL_C_SLONG, &w, 0, &cb2);
        if (cb2 != SQL_NULL_DATA) out.wWidth = w;
        SQLGetData(hStmt, 3, SQL_C_SLONG, &h, 0, &cb3);
        if (cb3 != SQL_NULL_DATA) out.wHeight = h;
        SQLGetData(hStmt, 4, SQL_C_SLONG, &t, 0, &cb4);
        if (cb4 != SQL_NULL_DATA) out.bType = t;
        found = true;
    });
    return found;
}

void GameDataDB::GetLinkMapList(DWORD dwMapID, std::vector<LinkMapEntry>& out) {
    std::string q = "SELECT dwLinkMapID, wPosX, wPosY, wWidth, wHeight, bLinkType, ISNULL(wStartPosX,0), ISNULL(wStartPosY,0) FROM LINKMAPLIST WHERE dwMapID = " + std::to_string(dwMapID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        LinkMapEntry lm; SQLLEN c[8];
        SQLGetData(hStmt, 1, SQL_C_ULONG, &lm.dwLinkMapID, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_USHORT, &lm.wPortalPosX, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_USHORT, &lm.wPortalPosY, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_USHORT, &lm.wPortalWidth, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_USHORT, &lm.wPortalHeight, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_TINYINT, &lm.bLinkType, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_USHORT, &lm.wStartPosX, 0, &c[6]);
        SQLGetData(hStmt, 8, SQL_C_USHORT, &lm.wStartPosY, 0, &c[7]);
        out.push_back(lm);
    });
}

bool GameDataDB::GetPortalDest(DWORD dwMapID, DWORD dwLinkMapID, PortalDest& out) {
    bool found = false;
    std::string q = "SELECT dwDestMapID, wDestX, wDestY FROM LINKPORTAL WHERE dwMapID = " + std::to_string(dwMapID) + " AND dwLinkMapID = " + std::to_string(dwLinkMapID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c[3];
        SQLGetData(hStmt, 1, SQL_C_ULONG, &out.dwDestMapID, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_USHORT, &out.wDestX, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_USHORT, &out.wDestY, 0, &c[2]);
        found = true;
    });
    return found;
}

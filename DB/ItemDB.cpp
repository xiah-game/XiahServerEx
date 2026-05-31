#include "ItemDB.h"
#include "../DBHelper.h"
#include "../ServerCore.h"

DWORD ItemDB::CreateItemFromTemplate(WORD wRefID) {
    DWORD newItemID = 0;
    std::string q = "SET NOCOUNT ON; "
                    "INSERT INTO ITEM (wRefID, bType, bKind, wVisualID, szName, dwCost, wLevel, bCharType, wAmount, "
                    "nBasicData1, nBasicData2, nBasicData3, nBasicData4, nBasicData5) "
                    "SELECT wRefID, bType, bKind, wVisualID, szName, dwCost, wLevel, bCharType, wAmount, "
                    "nBasicData1, nBasicData2, nBasicData3, nBasicData4, nBasicData5 "
                    "FROM ITEMTEMPLATE WHERE wRefID = " + std::to_string(wRefID) + "; "
                    "SELECT @@IDENTITY;";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_ULONG, &newItemID, 0, &c);
    });
    return newItemID;
}

void ItemDB::DeleteItem(DWORD dwItemID) {
    DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEM WHERE dwItemID = " + std::to_string(dwItemID));
}

void ItemDB::DeleteItemData(DWORD dwItemID) {
    DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwItemID));
}

void ItemDB::DeleteItemCascade(DWORD dwItemID) {
    std::string id = std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteUpdate("DELETE FROM SACKITEM WHERE dwItemID = " + id);
    DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEMDATA WHERE dwItemID = " + id);
    DBHelper::GetInstance().ExecuteUpdate("DELETE FROM BANKITEM WHERE dwItemID = " + id);
    DBHelper::GetInstance().ExecuteUpdate("DELETE FROM MAPITEM WHERE dwItemID = " + id);
    DBHelper::GetInstance().ExecuteUpdate("DELETE FROM ITEM WHERE dwItemID = " + id);
}

WORD ItemDB::GetItemRefID(DWORD dwItemID) {
    WORD refID = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT wRefID FROM ITEM WHERE dwItemID = " + std::to_string(dwItemID),
        [&](SQLHSTMT hStmt) {
            SQLLEN c;
            SQLGetData(hStmt, 1, SQL_C_USHORT, &refID, 0, &c);
        });
    return refID;
}

void ItemDB::InsertItemData(DWORD dwItemID, const int nData[25]) {
    std::string vals = "";
    for (int i = 0; i < 25; i++) {
        vals += std::to_string(nData[i]);
        if (i < 24) vals += ", ";
    }
    std::string q = "INSERT INTO ITEMDATA (dwItemID, nData1, nData2, nData3, nData4, nData5, "
                    "nData6, nData7, nData8, nData9, nData10, nData11, nData12, nData13, nData14, "
                    "nData15, nData16, nData17, nData18, nData19, nData20, nData21, nData22, "
                    "nData23, nData24, nData25) VALUES (" + std::to_string(dwItemID) + ", " + vals + ")";
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void ItemDB::UpdateItemData(DWORD dwItemID, int fieldIndex, int value) {
    if (fieldIndex < 1 || fieldIndex > 25) return;
    std::string q = "UPDATE ITEMDATA SET nData" + std::to_string(fieldIndex) + " = " + std::to_string(value)
        + " WHERE dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void ItemDB::UpdateItemDataFields(DWORD dwItemID, const std::map<int, int>& fields) {
    if (fields.empty()) return;
    std::string q = "UPDATE ITEMDATA SET ";
    bool first = true;
    for (auto& kv : fields) {
        if (kv.first < 1 || kv.first > 25) continue;
        if (!first) q += ", ";
        q += "nData" + std::to_string(kv.first) + " = " + std::to_string(kv.second);
        first = false;
    }
    q += " WHERE dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

bool ItemDB::GetItemData(DWORD dwItemID, ItemDataRow& out) {
    std::string q = "SELECT nData1,nData2,nData3,nData4,nData5,nData6,nData7,nData8,nData9,nData10,"
                    "nData11,nData12,nData13,nData14,nData15,nData16,nData17,nData18,nData19,nData20,"
                    "nData21,nData22,nData23,nData24,nData25 FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwItemID);
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c;
        for (int i = 0; i < 25; i++) {
            SQLGetData(hStmt, (SQLUSMALLINT)(i + 1), SQL_C_SLONG, &out.nData[i], 0, &c);
        }
        found = true;
    });
    return found;
}

void ItemDB::AddToSack(DWORD dwCharID, BYTE bSackPos, DWORD dwItemID) {
    std::string q = "INSERT INTO SACKITEM (dwCharID, bSackPos, dwItemID) VALUES ("
        + std::to_string(dwCharID) + ", " + std::to_string(bSackPos) + ", " + std::to_string(dwItemID) + ")";
    DBHelper::GetInstance().ExecuteQuery(q, [](SQLHSTMT){});
}

void ItemDB::RemoveFromSack(DWORD dwCharID, DWORD dwItemID) {
    std::string q = "DELETE FROM SACKITEM WHERE dwCharID = " + std::to_string(dwCharID)
        + " AND dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void ItemDB::RemoveFromSackByPos(DWORD dwCharID, BYTE bSackPos) {
    std::string q = "DELETE FROM SACKITEM WHERE dwCharID = " + std::to_string(dwCharID)
        + " AND bSackPos = " + std::to_string(bSackPos);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void ItemDB::UpdateSackPos(DWORD dwCharID, DWORD dwItemID, BYTE newPos) {
    std::string q = "UPDATE SACKITEM SET bSackPos = " + std::to_string(newPos)
        + " WHERE dwCharID = " + std::to_string(dwCharID) + " AND dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

DWORD ItemDB::GetItemAtSackPos(DWORD dwCharID, BYTE bSackPos) {
    DWORD itemID = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwItemID FROM SACKITEM WHERE dwCharID = " + std::to_string(dwCharID) + " AND bSackPos = " + std::to_string(bSackPos),
        [&](SQLHSTMT hStmt) {
            SQLLEN c;
            SQLGetData(hStmt, 1, SQL_C_ULONG, &itemID, 0, &c);
        });
    return itemID;
}

bool ItemDB::IsSackPosOccupied(DWORD dwCharID, BYTE bSackPos) {
    return GetItemAtSackPos(dwCharID, bSackPos) != 0;
}

void ItemDB::EquipItem(DWORD dwCharID, BYTE bEquipPos, DWORD dwItemID) {
    std::string q = "INSERT INTO EQUIPITEM (dwCharID, bEquipPos, dwItemID) VALUES ("
        + std::to_string(dwCharID) + ", " + std::to_string(bEquipPos) + ", " + std::to_string(dwItemID) + ")";
    DBHelper::GetInstance().ExecuteQuery(q, [](SQLHSTMT){});
}

void ItemDB::UnequipItem(DWORD dwCharID, BYTE bEquipPos) {
    std::string q = "DELETE FROM EQUIPITEM WHERE dwCharID = " + std::to_string(dwCharID)
        + " AND bEquipPos = " + std::to_string(bEquipPos);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

DWORD ItemDB::GetEquippedItem(DWORD dwCharID, BYTE bEquipPos) {
    DWORD itemID = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwItemID FROM EQUIPITEM WHERE dwCharID = " + std::to_string(dwCharID) + " AND bEquipPos = " + std::to_string(bEquipPos),
        [&](SQLHSTMT hStmt) {
            SQLLEN c;
            SQLGetData(hStmt, 1, SQL_C_ULONG, &itemID, 0, &c);
        });
    return itemID;
}

void ItemDB::AddToBank(DWORD dwCharID, BYTE bBankPos, DWORD dwItemID) {
    std::string q = "INSERT INTO BANKITEM (dwCharID, bBankPos, dwItemID) VALUES ("
        + std::to_string(dwCharID) + ", " + std::to_string(bBankPos) + ", " + std::to_string(dwItemID) + ")";
    DBHelper::GetInstance().ExecuteQuery(q, [](SQLHSTMT){});
}

void ItemDB::RemoveFromBank(DWORD dwCharID, DWORD dwItemID) {
    std::string q = "DELETE FROM BANKITEM WHERE dwCharID = " + std::to_string(dwCharID)
        + " AND dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void ItemDB::AddToBankByAccount(const std::string& account, BYTE bBankPos, DWORD dwItemID) {
    std::string q = "INSERT INTO BANKITEM (szAccount, bSackPos, dwItemID) VALUES ('"
        + account + "', " + std::to_string(bBankPos) + ", " + std::to_string(dwItemID) + ")";
    DBHelper::GetInstance().ExecuteQuery(q, nullptr);
}

void ItemDB::RemoveFromBankByAccount(const std::string& account, DWORD dwItemID) {
    std::string q = "DELETE FROM BANKITEM WHERE szAccount = '" + account + "' AND dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteQuery(q, nullptr);
}

void ItemDB::RemoveFromMallByAccount(const std::string& account, DWORD dwItemID) {
    std::string q = "DELETE FROM MALLITEM WHERE szAccount = '" + account + "' AND dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteQuery(q, nullptr);
}

void ItemDB::UpdateSackPos(DWORD dwItemID, BYTE bSackPos) {
    std::string q = "UPDATE SACKITEM SET bSackPos = " + std::to_string(bSackPos)
        + " WHERE dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void ItemDB::UpdateItemAmount(DWORD dwItemID, WORD wAmount) {
    std::string q = "UPDATE ITEM SET wAmount = " + std::to_string(wAmount)
        + " WHERE dwItemID = " + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteUpdate(q);
}

void ItemDB::DecrementItemAmount(DWORD dwItemID) {
    DBHelper::GetInstance().ExecuteUpdate(
        "UPDATE ITEM SET wAmount = wAmount - 1 WHERE dwItemID = " + std::to_string(dwItemID));
}

void ItemDB::DecrementItemAmountBy(DWORD dwItemID, WORD amount) {
    DBHelper::GetInstance().ExecuteUpdate(
        "UPDATE ITEM SET wAmount = wAmount - " + std::to_string(amount) + " WHERE dwItemID = " + std::to_string(dwItemID));
}

void ItemDB::UpdateItemName(DWORD dwItemID, const std::string& name) {
    DBHelper::GetInstance().ExecuteUpdate(
        "UPDATE ITEM SET szName = '" + name + "' WHERE dwItemID = " + std::to_string(dwItemID));
}

DWORD ItemDB::InsertItemFromTemplate(DWORD dwCharID, WORD wRefID, BYTE bSackPos) {
    // Step A: Insert ITEM from ITEMTEMPLATE
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO ITEM (wRefID, bType, bKind, wVisualID, szName, dwCost, wLevel, bCharType, wAmount, nBasicData1, nBasicData2, nBasicData3, nBasicData4, nBasicData5) "
        "SELECT wRefID, bType, bKind, wVisualID, szName, 0, wLevel, bCharType, 1, nBasicData1, nBasicData2, nBasicData3, nBasicData4, nBasicData5 "
        "FROM ITEMTEMPLATE WHERE wRefID = " + std::to_string(wRefID));
    // Step B: Get new dwItemID
    DWORD dwNewItemID = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT @@IDENTITY AS NewItemID",
        [&](SQLHSTMT hStmt) { SQLLEN cb; int id = 0; SQLGetData(hStmt, 1, SQL_C_SLONG, &id, 0, &cb); dwNewItemID = (DWORD)id; });
    if (dwNewItemID == 0) return 0;
    // Step C: Insert ITEMDATA from ITEMTEMPLATE
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO ITEMDATA (dwItemID, nData1, nData2, nData3, nData4, nData5, nData6, nData7, nData8) "
        "SELECT " + std::to_string(dwNewItemID) + ", nData1, nData2, nData3, nData4, nData5, nData6, nData7, nData8 FROM ITEMTEMPLATE WHERE wRefID = " + std::to_string(wRefID));
    // Step D: Insert SACKITEM
    DBHelper::GetInstance().ExecuteUpdate(
        "INSERT INTO SACKITEM (dwCharID, bSackPos, dwItemID) VALUES (" + std::to_string(dwCharID) + ", " + std::to_string(bSackPos) + ", " + std::to_string(dwNewItemID) + ")");
    return dwNewItemID;
}

void ItemDB::UpsertRebuildData(DWORD dwItemID, int d4, int d5, int d9, int d14, int d15, int d17) {
    DBHelper::GetInstance().ExecuteUpdate(
        "IF EXISTS (SELECT 1 FROM ITEMDATA WHERE dwItemID = " + std::to_string(dwItemID) + ") "
        "UPDATE ITEMDATA SET nData4 = " + std::to_string(d4) + ", nData5 = " + std::to_string(d5) +
        ", nData9 = " + std::to_string(d9) + ", nData14 = " + std::to_string(d14) +
        ", nData15 = " + std::to_string(d15) + ", nData17 = " + std::to_string(d17) +
        " WHERE dwItemID = " + std::to_string(dwItemID) + " "
        "ELSE INSERT INTO ITEMDATA (dwItemID, nData4, nData5, nData9, nData14, nData15, nData17) VALUES (" +
        std::to_string(dwItemID) + ", " + std::to_string(d4) + ", " + std::to_string(d5) +
        ", " + std::to_string(d9) + ", " + std::to_string(d14) + ", " + std::to_string(d15) +
        ", " + std::to_string(d17) + ")");
}

WORD ItemDB::GetItemAmount(DWORD dwItemID) {
    WORD amount = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT wAmount FROM ITEM WHERE dwItemID = " + std::to_string(dwItemID),
        [&](SQLHSTMT hStmt) {
            SQLLEN c;
            SQLGetData(hStmt, 1, SQL_C_USHORT, &amount, 0, &c);
        });
    return amount;
}

DWORD ItemDB::InsertItem(WORD wRefID, BYTE bType, BYTE bKind, WORD wVisualID, const std::string& szName,
                          DWORD dwCost, WORD wLevel, BYTE bCharType, DWORD wAmount) {
    DWORD newId = 0;
    std::string q = "SET NOCOUNT ON; INSERT INTO ITEM (wRefID, bType, bKind, wVisualID, szName, dwCost, wLevel, bCharType, wAmount) VALUES ("
        + std::to_string(wRefID) + ", " + std::to_string(bType) + ", " + std::to_string(bKind) + ", "
        + std::to_string(wVisualID) + ", '" + szName + "', " + std::to_string(dwCost) + ", "
        + std::to_string(wLevel) + ", " + std::to_string(bCharType) + ", " + std::to_string(wAmount) + "); SELECT @@IDENTITY;";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c; SQLGetData(hStmt, 1, SQL_C_ULONG, &newId, 0, &c);
    });
    return newId;
}

bool ItemDB::GetItemBasicInfo(DWORD dwItemID, ItemBasicInfo& out) {
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT wRefID, wAmount, dwCost FROM ITEM WHERE dwItemID = " + std::to_string(dwItemID),
        [&](SQLHSTMT hStmt) {
            SQLLEN c1, c2, c3;
            SQLGetData(hStmt, 1, SQL_C_USHORT, &out.wRefID, 0, &c1);
            SQLGetData(hStmt, 2, SQL_C_ULONG, &out.wAmount, 0, &c2);
            SQLGetData(hStmt, 3, SQL_C_ULONG, &out.dwCost, 0, &c3);
            found = true;
        });
    return found;
}

int ItemDB::GetRebuildBonusSum(const std::string& attrColumn, int reqLevel, int maxRebuildLevel) {
    int result = 0;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT ISNULL(SUM(" + attrColumn + " * " + std::to_string(reqLevel) + " * LevelMultiplier / 100), 0) FROM REBUILD_CONFIG WHERE RebuildLevel <= " + std::to_string(maxRebuildLevel),
        [&](SQLHSTMT hStmt) { SQLLEN c; SQLGetData(hStmt, 1, SQL_C_SLONG, &result, 0, &c); });
    return result;
}

std::vector<BYTE> ItemDB::GetBankUsedPositions(const std::string& account) {
    std::vector<BYTE> used;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT bSackPos FROM BANKITEM WHERE szAccount = '" + account + "'",
        [&](SQLHSTMT hStmt) { int p=0; SQLLEN c; SQLGetData(hStmt, 1, SQL_C_SLONG, &p, 0, &c); used.push_back((BYTE)p); });
    return used;
}

bool ItemDB::IsBankPosOccupied(const std::string& account, BYTE bBankPos) {
    bool occ = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwItemID FROM BANKITEM WHERE szAccount = '" + account + "' AND bSackPos = " + std::to_string(bBankPos),
        [&](SQLHSTMT) { occ = true; });
    return occ;
}

bool ItemDB::IsSackItemOwned(DWORD dwCharID, DWORD dwItemID) {
    bool owned = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwItemID FROM SACKITEM WHERE dwCharID = " + std::to_string(dwCharID) + " AND dwItemID = " + std::to_string(dwItemID),
        [&](SQLHSTMT) { owned = true; });
    return owned;
}

int ItemDB::GetEquippedItemDataValue(DWORD dwCharID, BYTE bKind, int dataIndex) {
    int value = 0;
    if (dataIndex < 1 || dataIndex > 25) return 0;
    
    std::string columnName = "T.nData" + std::to_string(dataIndex);
    std::string q = 
        "SELECT TOP 1 " + columnName + " "
        "FROM SACKITEM S "
        "INNER JOIN ITEM I ON S.dwItemID = I.dwItemID "
        "INNER JOIN ITEMTEMPLATE T ON I.wRefID = T.wRefID "
        "WHERE S.dwCharID = " + std::to_string(dwCharID) + " "
        "  AND S.bSackPos < 20 "
        "  AND T.bType = 8 "
        "  AND T.bKind = " + std::to_string(bKind);
        
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_LONG, &value, 0, &c);
    });
    return value;
}

bool ItemDB::IsItemInStorage(const std::string& tableName, const std::string& account, DWORD dwItemID) {
    bool found = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwItemID FROM " + tableName + " WHERE szAccount = '" + account + "' AND dwItemID = " + std::to_string(dwItemID),
        [&](SQLHSTMT) { found = true; });
    return found;
}

bool ItemDB::IsSackPosOccupiedAbs(DWORD dwCharID, BYTE absolutePos) {
    bool occ = false;
    DBHelper::GetInstance().ExecuteQuery(
        "SELECT dwItemID FROM SACKITEM WHERE dwCharID = " + std::to_string(dwCharID) + " AND bSackPos = " + std::to_string(absolutePos),
        [&](SQLHSTMT) { occ = true; });
    return occ;
}

void ItemDB::GetEquippedItemStats(DWORD dwCharID, std::vector<EquipStatRow>& out) {
    std::string q = "SELECT I.wRefID, ISNULL(D.nData4, -9999), ISNULL(D.nData5, -9999), ISNULL(D.nData6, -9999), ISNULL(D.nData7, -9999), ISNULL(D.nData13, -9999), ISNULL(D.nData9, -9999), ISNULL(D.nData10, -9999), ISNULL(D.nData11, -9999), ISNULL(D.nData12, -9999) FROM SACKITEM S JOIN ITEM I ON S.dwItemID = I.dwItemID LEFT JOIN ITEMDATA D ON S.dwItemID = D.dwItemID WHERE S.dwCharID = " + std::to_string(dwCharID) + " AND S.bSackPos < 20";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        EquipStatRow r; SQLLEN c[10];
        SQLGetData(hStmt, 1, SQL_C_USHORT, &r.wRefID, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &r.d4, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &r.d5, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &r.d6, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &r.d7, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_SLONG, &r.d13, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &r.d9, 0, &c[6]);
        SQLGetData(hStmt, 8, SQL_C_SLONG, &r.d10, 0, &c[7]);
        SQLGetData(hStmt, 9, SQL_C_SLONG, &r.d11, 0, &c[8]);
        SQLGetData(hStmt, 10, SQL_C_SLONG, &r.d12, 0, &c[9]);
        out.push_back(r);
    });
}

void ItemDB::GetSackOccupancy(DWORD dwCharID, int startPos, int endPos, std::vector<SackOccupancy>& out) {
    std::string q = "SELECT S.bSackPos, I.wRefID, S.dwItemID FROM SACKITEM S JOIN ITEM I ON S.dwItemID = I.dwItemID WHERE S.dwCharID = " + std::to_string(dwCharID) + " AND S.bSackPos >= " + std::to_string(startPos) + " AND S.bSackPos <= " + std::to_string(endPos);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        SackOccupancy r; SQLLEN c1, c2, c3;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &r.bSackPos, 0, &c1);
        SQLGetData(hStmt, 2, SQL_C_USHORT, &r.wRefID, 0, &c2);
        SQLGetData(hStmt, 3, SQL_C_ULONG, &r.dwItemID, 0, &c3);
        out.push_back(r);
    });
}

static void ParseFullItemRow(SQLHSTMT hStmt, ItemDB::FullItemRow& r, int colOffset) {
    SQLLEN c[37] = {0};
    int col = colOffset;
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.wVisualID, 0, &c[0]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.bType, 0, &c[1]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.bKind, 0, &c[2]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.wLevel, 0, &c[3]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.dwCost, 0, &c[4]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.nData18, 0, &c[5]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.nData19, 0, &c[6]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.wRefID, 0, &c[7]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.wAmount, 0, &c[8]);
    for (int i = 0; i < 17; i++) SQLGetData(hStmt, col++, SQL_C_SLONG, &r.d[i], 0, &c[9+i]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.nData20, 0, &c[26]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.nData21, 0, &c[27]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.nData25, 0, &c[28]);
    SQLGetData(hStmt, col++, SQL_C_CHAR, r.szName, sizeof(r.szName), &c[29]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.nBasicData1, 0, &c[30]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.nBasicData2, 0, &c[31]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.nBasicData3, 0, &c[32]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.nBasicData4, 0, &c[33]);
    SQLGetData(hStmt, col++, SQL_C_SLONG, &r.nBasicData5, 0, &c[34]);
}

static const char* FULL_ITEM_COLS = "I.wVisualID, I.bType, I.bKind, I.wLevel, I.dwCost, ISNULL(D.nData18,0), ISNULL(D.nData19,0), I.wRefID, I.wAmount, ISNULL(D.nData1,-9999), ISNULL(D.nData2,-9999), ISNULL(D.nData3,-9999), ISNULL(D.nData4,-9999), ISNULL(D.nData5,-9999), ISNULL(D.nData6,-9999), ISNULL(D.nData7,-9999), ISNULL(D.nData8,-9999), ISNULL(D.nData9,-9999), ISNULL(D.nData10,-9999), ISNULL(D.nData11,-9999), ISNULL(D.nData12,-9999), ISNULL(D.nData13,-9999), ISNULL(D.nData14,-9999), ISNULL(D.nData15,-9999), ISNULL(D.nData16,-9999), ISNULL(D.nData17,-9999), ISNULL(D.nData20,0), ISNULL(D.nData21,0), ISNULL(D.nData25,0), I.szName, ISNULL(I.nBasicData1,0), ISNULL(I.nBasicData2,0), ISNULL(I.nBasicData3,0), ISNULL(I.nBasicData4,0), ISNULL(I.nBasicData5,0)";

bool ItemDB::GetFullItemData(DWORD dwItemID, FullItemRow& out) {
    bool found = false;
    std::string q = std::string("SELECT ") + FULL_ITEM_COLS + " FROM ITEM I LEFT JOIN ITEMDATA D ON I.dwItemID=D.dwItemID WHERE I.dwItemID=" + std::to_string(dwItemID);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        out.dwItemID = dwItemID;
        ParseFullItemRow(hStmt, out, 1);
        found = true;
    });
    return found;
}

void ItemDB::GetFullSackItems(DWORD dwCharID, const std::string& posCond, std::vector<FullItemRow>& out) {
    std::string q = "SELECT S.bSackPos, S.dwItemID, " + std::string(FULL_ITEM_COLS) + " FROM SACKITEM S JOIN ITEM I ON S.dwItemID=I.dwItemID LEFT JOIN ITEMDATA D ON S.dwItemID=D.dwItemID WHERE S.dwCharID=" + std::to_string(dwCharID) + " AND " + posCond;
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        FullItemRow r; SQLLEN c1, c2;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &r.bSackPos, 0, &c1);
        SQLGetData(hStmt, 2, SQL_C_ULONG, &r.dwItemID, 0, &c2);
        ParseFullItemRow(hStmt, r, 3);
        out.push_back(r);
    });
}

void ItemDB::GetFullBankItems(const std::string& account, const std::string& tableName, std::vector<FullItemRow>& out) {
    std::string q = "SELECT B.bSackPos, B.dwItemID, " + std::string(FULL_ITEM_COLS) + " FROM " + tableName + " B JOIN ITEM I ON B.dwItemID=I.dwItemID LEFT JOIN ITEMDATA D ON B.dwItemID=D.dwItemID WHERE B.szAccount='" + account + "'";
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        FullItemRow r; SQLLEN c1, c2;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &r.bSackPos, 0, &c1);
        SQLGetData(hStmt, 2, SQL_C_ULONG, &r.dwItemID, 0, &c2);
        ParseFullItemRow(hStmt, r, 3);
        out.push_back(r);
    });
}

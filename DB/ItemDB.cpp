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
    DBHelper::GetInstance().ExecuteQuery(q, [](SQLHSTMT){});
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

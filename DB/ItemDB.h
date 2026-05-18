#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <map>

// ItemDB: Abstracts all ITEM, ITEMDATA, SACKITEM, EQUIPITEM, BANKITEM queries.
class ItemDB {
public:
    static ItemDB& GetInstance() {
        static ItemDB instance;
        return instance;
    }

    // ---- ITEM table ----
    
    // Insert a new ITEM from ITEMTEMPLATE and return the new dwItemID
    DWORD CreateItemFromTemplate(WORD wRefID);

    // Delete an item from ITEM table
    void DeleteItem(DWORD dwItemID);

    // Delete item data from ITEMDATA table
    void DeleteItemData(DWORD dwItemID);

    // Cascade delete: SACKITEM + ITEMDATA + BANKITEM + MAPITEM + ITEM
    void DeleteItemCascade(DWORD dwItemID);

    // Get item's wRefID
    WORD GetItemRefID(DWORD dwItemID);

    // ---- ITEMDATA table ----

    // Insert ITEMDATA row with nData values
    void InsertItemData(DWORD dwItemID, const int nData[25]);

    // Update a specific nData field
    void UpdateItemData(DWORD dwItemID, int fieldIndex, int value);

    // Update multiple nData fields at once
    void UpdateItemDataFields(DWORD dwItemID, const std::map<int, int>& fields);

    // Get all nData for an item
    struct ItemDataRow {
        int nData[25] = {0};
    };
    bool GetItemData(DWORD dwItemID, ItemDataRow& out);

    // ---- SACKITEM (Backpack) ----
    
    // Insert item into backpack
    void AddToSack(DWORD dwCharID, BYTE bSackPos, DWORD dwItemID);

    // Remove item from backpack
    void RemoveFromSack(DWORD dwCharID, DWORD dwItemID);

    // Remove item from backpack by position
    void RemoveFromSackByPos(DWORD dwCharID, BYTE bSackPos);

    // Move item within backpack (update position)
    void UpdateSackPos(DWORD dwCharID, DWORD dwItemID, BYTE newPos);

    // Get item at a specific sack position
    DWORD GetItemAtSackPos(DWORD dwCharID, BYTE bSackPos);

    // Check if a sack position is occupied
    bool IsSackPosOccupied(DWORD dwCharID, BYTE bSackPos);

    // ---- EQUIPITEM ----

    // Equip an item
    void EquipItem(DWORD dwCharID, BYTE bEquipPos, DWORD dwItemID);

    // Unequip an item
    void UnequipItem(DWORD dwCharID, BYTE bEquipPos);

    // Get equipped item at position
    DWORD GetEquippedItem(DWORD dwCharID, BYTE bEquipPos);

    // ---- BANKITEM (Warehouse) ----
    
    // Deposit item to bank
    void AddToBank(DWORD dwCharID, BYTE bBankPos, DWORD dwItemID);

    // Withdraw item from bank
    void RemoveFromBank(DWORD dwCharID, DWORD dwItemID);

    // Bank by account name (used by BankHandler)
    void AddToBankByAccount(const std::string& account, BYTE bBankPos, DWORD dwItemID);
    void RemoveFromBankByAccount(const std::string& account, DWORD dwItemID);
    void RemoveFromMallByAccount(const std::string& account, DWORD dwItemID);

    // ---- Item Amount ----

    // Update sack item position
    void UpdateSackPos(DWORD dwItemID, BYTE bSackPos);

    // Update item amount/stack
    void UpdateItemAmount(DWORD dwItemID, WORD wAmount);

    // Decrement item amount by 1
    void DecrementItemAmount(DWORD dwItemID);

    // Decrement item amount by N
    void DecrementItemAmountBy(DWORD dwItemID, WORD amount);

    // Update item name
    void UpdateItemName(DWORD dwItemID, const std::string& name);

    // Create item from template: INSERT ITEM + ITEMDATA from ITEMTEMPLATE, INSERT SACKITEM
    // Returns new dwItemID, or 0 on failure
    DWORD InsertItemFromTemplate(DWORD dwCharID, WORD wRefID, BYTE bSackPos);

    // Insert new item and return dwItemID
    DWORD InsertItem(WORD wRefID, BYTE bType, BYTE bKind, WORD wVisualID, const std::string& szName,
                     DWORD dwCost, WORD wLevel, BYTE bCharType, DWORD wAmount);

    // Get basic item info for sell operations
    struct ItemBasicInfo { WORD wRefID = 0; DWORD wAmount = 0; DWORD dwCost = 0; };
    bool GetItemBasicInfo(DWORD dwItemID, ItemBasicInfo& out);

    // Upsert ITEMDATA (rebuild stats) in ITEMDATA
    void UpsertRebuildData(DWORD dwItemID, int d4, int d5, int d9, int d14, int d15, int d17);

    // Get cumulative rebuild bonus (Wujing or Sujing)
    int GetRebuildBonusSum(const std::string& attrColumn, int reqLevel, int maxRebuildLevel);

    // Get item amount
    WORD GetItemAmount(DWORD dwItemID);

private:
    ItemDB() {}
    ~ItemDB() {}
};

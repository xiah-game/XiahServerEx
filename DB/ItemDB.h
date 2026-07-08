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

    // 改造系统 v2：通用属性覆盖写入（fields: nData字段号 → 值）
    void UpsertRebuildData(DWORD dwItemID, const std::map<int, int>& fields);

    // 旧版兼容（已有调用点可能使用此签名）
    void UpsertRebuildData(DWORD dwItemID, int d4, int d5, int d9, int d14, int d15, int d17);

    // Get cumulative rebuild bonus (Wujing or Sujing)
    int GetRebuildBonusSum(const std::string& attrColumn, int reqLevel, int maxRebuildLevel);

    // 血晶降级：更新 ITEM.nBasicData1（穿戴等级需求）
    void UpdateItemNBasicData1(DWORD dwItemID, int newValue);

    // Get all used bank positions for account
    std::vector<BYTE> GetBankUsedPositions(const std::string& account);

    // Check if bank pos is occupied
    bool IsBankPosOccupied(const std::string& account, BYTE bBankPos);

    // Check if character owns item in SACKITEM
    bool IsSackItemOwned(DWORD dwCharID, DWORD dwItemID);

    // Check if character has equipped an item of specific kind (bType == 8, bKind == specific)
    int GetEquippedItemDataValue(DWORD dwCharID, BYTE bKind, int dataIndex);

    // Check if item exists in a storage table (BANKITEM/MALLITEM) by account
    bool IsItemInStorage(const std::string& tableName, const std::string& account, DWORD dwItemID);

    // Check if sack position is occupied (by absolute pos)
    bool IsSackPosOccupiedAbs(DWORD dwCharID, BYTE absolutePos);

    // Get equipped item stats (wRefID + nData4-7,9-13) for all equipped items (bSackPos < 20)
    struct EquipStatRow { WORD wRefID = 0; int d4=0, d5=0, d6=0, d7=0, d9=0, d10=0, d11=0, d12=0, d13=0; int d1=0, d2=0; };
    void GetEquippedItemStats(DWORD dwCharID, std::vector<EquipStatRow>& out);

    // Get sack occupancy data (bSackPos + wRefID) for a range
    struct SackOccupancy { int bSackPos = 0; WORD wRefID = 0; DWORD dwItemID = 0; };
    void GetSackOccupancy(DWORD dwCharID, int startPos, int endPos, std::vector<SackOccupancy>& out);

    // Full item row: ITEM + ITEMDATA columns (30+ columns)
    struct FullItemRow {
        int bSackPos=0; DWORD dwItemID=0;
        int wVisualID=0, bType=0, bKind=0, wLevel=0, dwCost=0;
        int nData18=0, nData19=0, wRefID=0, wAmount=0;
        int d[18] = {0}; // d[0]=nData1..d[16]=nData17, d[17] unused
        int nData20=0, nData21=0, nData25=0;
        char szName[128] = {0};
        int nBasicData1=0, nBasicData2=0, nBasicData3=0, nBasicData4=0, nBasicData5=0;
    };
    // Get full item data for a single item (by dwItemID)
    bool GetFullItemData(DWORD dwItemID, FullItemRow& out);
    // Get full sack items for inventory display
    void GetFullSackItems(DWORD dwCharID, const std::string& posCond, std::vector<FullItemRow>& out);
    // Get full item data from BANKITEM/MALLITEM for bank display
    void GetFullBankItems(const std::string& account, const std::string& tableName, std::vector<FullItemRow>& out);

    // Get item amount
    WORD GetItemAmount(DWORD dwItemID);

    // ---- 号角物品(bType=32)辅助函数 ----

    // 读取 ITEM 表的 nBasicData 字段值（fieldIndex: 1-5）
    int GetItemNBasicData(DWORD dwItemID, int fieldIndex);

    // 将 ITEM.nBasicData3 减 1（号角耐久扣减），返回扣减后的值
    int DecrementNBasicData3(DWORD dwItemID);

private:
    ItemDB() {}
    ~ItemDB() {}
};

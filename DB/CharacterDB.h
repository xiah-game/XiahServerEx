#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>
#include <vector>
#include <functional>

// CharacterDB: Abstracts all CHAR_DATA, CHAR_POWER, CHAR_BASIC, CHAR_STATUS queries.
// Eliminates raw SQL from Handlers for character-related operations.
class CharacterDB {
public:
    static CharacterDB& GetInstance() {
        static CharacterDB instance;
        return instance;
    }

    // ---- CHAR_POWER (Stats / Level / EXP) ----

    struct CharPower {
        WORD wLevel = 0, wStr = 0, wSus = 0, wDex = 0, wVit = 0;
        BYTE bCharType = 0;
        WORD wIpMax = 0, wIpCur = 0;
        DWORD dwHpMax = 0, dwHpCur = 0;
        long long int dwExp = 0;
        DWORD dwTotalSp = 0, dwTotalTp = 0;
        WORD wRemainSp = 0, wRemainTp = 0;
        DWORD dwMoney = 0, dwFame = 0;
    };

    struct CharBasic {
        DWORD dwCharID = 0;
        BYTE bCharType = 0;
        std::string szCharName;
    };

    // Authenticate user login (returns accountId, sets exists/passMatch)
    DWORD AuthenticateUser(const std::string& username, const std::string& password,
                           const std::string& dbAccount, bool& accountExists, bool& passMatch);

    // Fetch full CHAR_DATA view (joins CHAR_POWER + CHAR_BASIC)
    bool GetCharData(DWORD dwCharID, CharPower& out);

    // Fetch CHAR_BASIC info
    bool GetCharBasic(DWORD dwCharID, CharBasic& out);

    // Update stat points after SP spend
    void UpdateStatPoints(DWORD dwCharID, WORD wStr, WORD wSus, WORD wDex, WORD wVit, WORD wRemainSp);

    // Set absolute money
    void SetMoney(DWORD dwCharID, DWORD dwMoney);

    // Relative money operations
    void AddMoney(DWORD dwCharID, DWORD amount);
    void SubtractMoney(DWORD dwCharID, DWORD amount);

    // Get current money
    INT64 GetMoney(DWORD dwCharID);

    // Update HP/IP in database
    void UpdateHpIp(DWORD dwCharID, DWORD dwHpCur, WORD wIpCur);

    // Restore HP/IP to max in database (used on level-up)
    void RestoreHpIpToMax(DWORD dwCharID);

    // Update EXP + Level (used by ExpSystem)
    void UpdateExpAndLevel(DWORD dwCharID, long long int newExp, WORD wLevel,
                           WORD wRemainSp, DWORD dwTotalSp,
                           WORD wRemainTp, DWORD dwTotalTp,
                           bool levelUp, bool tpUp);

    // Fetch EXP data for GrantExp (from CHAR_POWER + CHAR_BASIC)
    struct ExpData {
        long long int dwExp = 0;
        WORD wLevel = 0;
        DWORD dwTotalTp = 0;
        WORD wRemainTp = 0, wRemainSp = 0;
        DWORD dwTotalSp = 0;
        BYTE bCharType = 1;
    };
    bool GetExpData(DWORD dwCharID, ExpData& out);

    // Get current position from CHAR_STATUS
    bool GetCharPosition(DWORD dwCharID, int& wPosX, int& wPosY, int& bHeight);

    // Get current map ID from CHAR_STATUS
    DWORD GetCharMapID(DWORD dwCharID);

    // Get account name from CHAR_ACCOUNT
    std::string GetAccountName(DWORD dwCharID);

    // Get name and char type from CHAR_VISUAL view
    bool GetCharVisual(DWORD dwCharID, std::string& szNickName, BYTE& bCharType);

    // Save position on disconnect
    void SavePosition(DWORD dwCharID, WORD wPosX, WORD wPosY, DWORD dwMapID);

    // ---- Character List / Login ----
    
    // Get all characters for an account
    struct CharListEntry {
        DWORD dwCharID = 0;
        std::string szCharName;
        BYTE bCharType = 0;
        WORD wLevel = 0;
        WORD wStr = 0, wSus = 0, wDex = 0, wVit = 0;
        DWORD dwMapID = 0;
        WORD wPosX = 0, wPosY = 0;
    };
    void GetCharacterList(const std::string& accountName, std::vector<CharListEntry>& out);

    // Full character select screen data
    struct EquipSlot { WORD wVis = 0; BYTE bRar = 0; BYTE bStx = 0; };
    struct CharSelectEntry {
        int dwCharID = 0; char szNickName[256] = {0}; char bCharType = 0;
        int dwBirthDate = 0; int dwMapID = 0; short wLevel = 0;
        int dwHpCur = 0; int dwHpMax = 0; short wIpCur = 0; short wIpMax = 0;
        short wVit = 0; short wStr = 0; short wSus = 0; short wDex = 0;
        char bRebirth = 0;
        EquipSlot items[9];
    };
    bool GetCharSelectList(const std::string& accountName, std::vector<CharSelectEntry>& out);

    // ---- Mugong (Skill Learning) ----
    
    // Deduct TP for skill learning
    void DeductTP(DWORD dwCharID, WORD amount);

    // Deduct TP from CHAR_DATA (wRemainTp field, used by MugongManager)
    void DeductTpFromCharData(DWORD dwCharID, DWORD amount);

    // Insert new mugong skill
    void InsertMugong(DWORD dwCharID, DWORD dwMugongID, BYTE bLevel);

    // Update mugong skill level
    void UpdateMugongLevel(DWORD dwCharID, DWORD dwMugongID, BYTE bLevel);

    // ---- Slot (Quick Bar) ----

    // Get spawn position from LOCATION table
    void GetSpawnPosition(DWORD dwMapID, int& wPosX, int& wPosY);

    // Get default stats from CHAR_DEFAULT
    struct CharDefault { int wStr, wDex, wVit, wSus, bIncHp, bIncIp; };
    bool GetCharDefault(BYTE bCharType, CharDefault& out);

    // Get slot values
    bool GetSlotValues(DWORD dwCharID, std::vector<DWORD>& slots);

    // ---- Character Lifecycle ----

    // Create full character record (CHAR_BASIC+ACCOUNT+STATUS+POWER+OPTION+RANK)
    // Returns new dwCharID, or 0 on failure
    DWORD CreateCharacterRecord(const std::string& accountName, const std::string& nickName, BYTE bCharType,
                                WORD wStr, WORD wSus, WORD wDex, WORD wVit,
                                int wHp, int wIp, WORD wPosX, WORD wPosY);

    // Soft-delete character (set bActive=9)
    bool SoftDeleteCharacter(DWORD dwCharID, const std::string& accountName);

    // Check if account owns character
    bool AccountOwnsCharacter(DWORD dwCharID, const std::string& accountName);

    // Count active characters for account
    int CountActiveCharacters(const std::string& accountName);

    // Check duplicate nickname
    bool NicknameExists(const std::string& nickName);

    // ---- Slot (Quick Bar) ----
    
    // Initialize empty slot row
    void InitializeSlot(DWORD dwCharID);

    // Set a specific slot value (bSlot 1-10)
    void SetSlotValue(DWORD dwCharID, BYTE bSlot, DWORD dwValue);

    // Get mugong level for a character's skill
    int GetMugongLevel(DWORD dwCharID, DWORD dwMugongID);

private:
    CharacterDB() {}
    ~CharacterDB() {}
};

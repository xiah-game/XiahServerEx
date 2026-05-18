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

    // ---- Mugong (Skill Learning) ----
    
    // Deduct TP for skill learning
    void DeductTP(DWORD dwCharID, WORD amount);

    // Deduct TP from CHAR_DATA (wRemainTp field, used by MugongManager)
    void DeductTpFromCharData(DWORD dwCharID, DWORD amount);

    // Insert new mugong skill
    void InsertMugong(DWORD dwCharID, DWORD dwMugongID, BYTE bLevel);

    // Update mugong skill level
    void UpdateMugongLevel(DWORD dwCharID, DWORD dwMugongID, BYTE bLevel);

private:
    CharacterDB() {}
    ~CharacterDB() {}
};

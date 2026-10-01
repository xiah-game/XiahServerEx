#pragma once
#include <winsock2.h>
#include <windows.h>
#include <unordered_map>
#include <mutex>
#include <string>
#include "../DBHelper.h"
#include "../ServerCore.h"

struct sPetLevelData {
    int wLevel = 0;
    long long i64NeedExp = 0;
    int wAtkGrowth = 0;
    int wDefGrowth = 0;
    int dwHpGrowth = 0;
};

class PetLevelExpManager {
public:
    static PetLevelExpManager& GetInstance() {
        static PetLevelExpManager instance;
        return instance;
    }

    bool LoadFromDB() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_levelMap.clear();

        std::string query = "SELECT wLevel, i64NeedExp, wAtkGrowth, wDefGrowth, dwHpGrowth FROM PET_LEVEL_EXP";
        bool success = DBHelper::GetInstance().ExecuteQuery(query, [&](SQLHSTMT hStmt) {
            sPetLevelData data;
            SQLGetData(hStmt, 1, SQL_INTEGER, &data.wLevel, 0, NULL);
            SQLGetData(hStmt, 2, SQL_C_SBIGINT, &data.i64NeedExp, 0, NULL);
            SQLGetData(hStmt, 3, SQL_INTEGER, &data.wAtkGrowth, 0, NULL);
            SQLGetData(hStmt, 4, SQL_INTEGER, &data.wDefGrowth, 0, NULL);
            SQLGetData(hStmt, 5, SQL_INTEGER, &data.dwHpGrowth, 0, NULL);
            m_levelMap[data.wLevel] = data;
        });

        LOG("[PetLevelExpManager] Loaded " + std::to_string(m_levelMap.size()) + " levels data from DB.");
        return success;
    }

    long long GetNeedExp(int level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_levelMap.find(level);
        return (it != m_levelMap.end()) ? it->second.i64NeedExp : 0;
    }

    long long GetLevelStartExp(int level) {
        std::lock_guard<std::mutex> lock(m_mutex);
        long long sum = 0;
        for (int i = 1; i < level; ++i) {
            auto it = m_levelMap.find(i);
            if (it != m_levelMap.end()) {
                sum += it->second.i64NeedExp;
            }
        }
        return sum;
    }

    bool GetLevelData(int level, sPetLevelData& out) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto it = m_levelMap.find(level);
        if (it != m_levelMap.end()) {
            out = it->second;
            return true;
        }
        return false;
    }

    int GetMaxLevel() {
        std::lock_guard<std::mutex> lock(m_mutex);
        int maxLvl = 0;
        for (const auto& pair : m_levelMap) {
            if (pair.first > maxLvl) maxLvl = pair.first;
        }
        return maxLvl > 0 ? maxLvl : 100;
    }

private:
    std::unordered_map<int, sPetLevelData> m_levelMap;
    std::mutex m_mutex;
    PetLevelExpManager() = default;
};


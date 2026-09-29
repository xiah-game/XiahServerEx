#include "ServerCore.h"
#include "DBHelper.h"
#include "DB/ItemDB.h"
#include "DB/CharacterDB.h"
#include "DB/GameDataDB.h"
#include "GameObjects/DropManager.h"
#include "GameObjects/MugongManager.h"
#include "GameObjects/MapInstance.h"
#include "GameObjects/QuestManager.h"
#include <unordered_map>
#include <memory>

static std::mutex g_logMutex;

static std::mutex g_SendMutexMapLock;
static std::unordered_map<SOCKET, std::shared_ptr<std::mutex>> g_SocketSendMutexes;

std::shared_ptr<std::mutex> GetSocketMutex(SOCKET s) {
    std::lock_guard<std::mutex> lock(g_SendMutexMapLock);
    if (g_SocketSendMutexes.find(s) == g_SocketSendMutexes.end()) {
        g_SocketSendMutexes[s] = std::make_shared<std::mutex>();
    }
    return g_SocketSendMutexes[s];
}

void RemoveSocketMutex(SOCKET s) {
    std::lock_guard<std::mutex> lock(g_SendMutexMapLock);
    g_SocketSendMutexes.erase(s);
}

int SafeSend(SOCKET s, const char* buf, int len, int flags) {
    if (s == INVALID_SOCKET) return SOCKET_ERROR;
    auto mtx = GetSocketMutex(s);
    std::lock_guard<std::mutex> lock(*mtx);
    int totalSent = 0;
    while (totalSent < len) {
        int sent = send(s, buf + totalSent, len - totalSent, flags);
        if (sent == SOCKET_ERROR || sent == 0) return SOCKET_ERROR;
        totalSent += sent;
    }
    return totalSent;
}

sServerConfig g_Config;

void LoadConfig() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string path(exePath);
    size_t lastSlash = path.find_last_of("\\/");
    std::string iniPath = path.substr(0, lastSlash) + "\\config.ini";

    char buf[1024];
    
    GetPrivateProfileStringA("Network", "ExternalIP", "127.0.0.1", buf, sizeof(buf), iniPath.c_str());
    g_Config.externalIP = buf;
    
    GetPrivateProfileStringA("Network", "InternalIP", "0.0.0.0", buf, sizeof(buf), iniPath.c_str());
    g_Config.internalIP = buf;
    
    g_Config.authPort = GetPrivateProfileIntA("Network", "AuthPort", 9001, iniPath.c_str());
    g_Config.unitPort = GetPrivateProfileIntA("Network", "UnitPort", 9002, iniPath.c_str());
    
    GetPrivateProfileStringA("Database", "ServerName", "127.0.0.1", buf, sizeof(buf), iniPath.c_str());
    g_Config.dbServer = buf;
    
    GetPrivateProfileStringA("Database", "UserID", "sa", buf, sizeof(buf), iniPath.c_str());
    g_Config.dbUser = buf;
    
    GetPrivateProfileStringA("Database", "Password", "123456", buf, sizeof(buf), iniPath.c_str());
    g_Config.dbPassword = buf;
    
    GetPrivateProfileStringA("Database", "GameDB", "xiah", buf, sizeof(buf), iniPath.c_str());
    g_Config.dbGame = buf;
    
    GetPrivateProfileStringA("Database", "AccountDB", "xiah_account", buf, sizeof(buf), iniPath.c_str());
    g_Config.dbAccount = buf;
    
    GetPrivateProfileStringA("Database", "ServerDB", "xiah_server", buf, sizeof(buf), iniPath.c_str());
    g_Config.dbServerInfo = buf;
    
    GetPrivateProfileStringA("Database", "LogDB", "xiah_log", buf, sizeof(buf), iniPath.c_str());
    g_Config.dbLog = buf;
    
    g_Config.channelPorts.clear();
    for (int i = 1; i <= 20; i++) {
        char key[16];
        sprintf(key, "%d", i);
        int port = GetPrivateProfileIntA("ChannelPorts", key, 0, iniPath.c_str());
        if (port > 0) {
            g_Config.channelPorts[i] = (WORD)port;
        }
    }
    
    // 客户端最低版本号从数据库 xiah_server.VersionInfo 加载（在 LoadGameData 阶段执行）
    // g_Config.requiredClientVersion 默认值 1081，DB 加载后覆盖

    std::cout << "[Config] Loaded configuration from " << iniPath << std::endl;
}

void LOG(const std::string& msg) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << msg << std::endl;
    std::ofstream ofs("server_debug.log", std::ios::app);
    ofs << msg << std::endl;
}

void EncryptPacket(BYTE* pHead, BYTE bKey) {
    WORD wPayloadSize = *(WORD*)(pHead + 2);
    WORD wTotalSize = wPayloadSize + 4;
    BYTE bPrev = 0;
    pHead[0] += bPrev + bKey + (wTotalSize & 0xFF);
    bPrev = pHead[0];
    pHead[1] += bPrev + bKey + (wTotalSize & 0xFF);
    bPrev = pHead[1];
    for (int i = 0; i < wPayloadSize; i++) {
        pHead[i+4] += bPrev + bKey + (wTotalSize & 0xFF);
        bPrev = pHead[i+4];
    }
}

void DecryptPacket(BYTE* pHead, BYTE bKey) {
    WORD wPayloadSize = *(WORD*)(pHead + 2);
    WORD wTotalSize = wPayloadSize + 4;
    BYTE bPrevKey = 0;
    BYTE bPrev = pHead[0];
    pHead[0] -= bPrevKey + bKey + (wTotalSize & 0xFF);
    bPrevKey = bPrev;
    bPrev = pHead[1];
    pHead[1] -= bPrevKey + bKey + (wTotalSize & 0xFF);
    bPrevKey = bPrev;
    for (int i = 0; i < wPayloadSize; i++) {
        bPrev = pHead[i+4];
        pHead[i+4] -= bPrevKey + bKey + (wTotalSize & 0xFF);
        bPrevKey = bPrev;
    }
}

std::map<BYTE, sNpcTemplate> g_NpcTemplates;
std::map<DWORD, sWorldMap> g_WorldMaps;
std::map<DWORD, CMapInstance*> g_MapInstances;
std::map<WORD, sItemTemplate> g_ItemTemplates;
std::map<WORD, std::map<BYTE, sLevelTemplate>> g_LevelTemplates;
std::map<int, sRebuildConfig> g_RebuildConfig;

void LoadGameData() {
    auto& dao = GameDataDB::GetInstance();

    // Load REBUILD_CONFIG into memory
    dao.LoadRebuildConfig(g_RebuildConfig);
    LOG("[ServerCore] Loaded " + std::to_string(g_RebuildConfig.size()) + " REBUILD_CONFIG entries");

    g_ItemTemplates.clear();
    g_LevelTemplates.clear();
    g_NpcTemplates.clear();
    
    // Load Realm and Channels into memory
    dao.LoadRealm(g_Config.dbServerInfo, g_Config.realmName);
    dao.LoadChannels(g_Config.dbServerInfo, g_Config.channelPorts, g_Config.cachedChannels);
    LOG("[Server] Realm Info Loaded. RealmName: " + g_Config.realmName + " | Channels: " + std::to_string(g_Config.cachedChannels.size()));

    // 从 xiah_server.VersionInfo 表加载客户端最低版本号
    g_Config.requiredClientVersion = dao.LoadRequiredClientVersion();
    LOG("[Server] Required Client Version: " + std::to_string(g_Config.requiredClientVersion));

    DropManager::GetInstance()->LoadDropTables();
    MugongManager::GetInstance()->LoadMugongData();
    
    // Load LEVELTEMPLATE
    dao.LoadLevelTemplates(g_LevelTemplates);
    LOG("[Server] Loaded " + std::to_string(g_LevelTemplates.size()) + " Levels from LEVELTEMPLATE.");

    // Load ITEMTEMPLATE
    dao.LoadItemTemplates(g_ItemTemplates);
    LOG("[Server] Loaded " + std::to_string(g_ItemTemplates.size()) + " Items from ITEMTEMPLATE.");

    // Load NPCTEMPLATE
    dao.LoadNpcTemplates(g_NpcTemplates);
    
    // Load NPC_MUGONGTEMPLATE
    dao.LoadNpcMugongTemplates(g_NpcTemplates);

    // Load Quest System Templates
    QuestManager::GetInstance().Initialize();

    // Write debug info for ranged NPCs
    for (auto& pair : g_NpcTemplates) {
        auto& tpl = pair.second;
        if (tpl.wShotAtkRangeInit > 0 || tpl.wMeleeAtkRangeInit > 20) {
            std::ofstream out("npc_debug.txt", std::ios::app);
            out << "NPC_ID_" << (int)pair.first << " Walk: " << tpl.wWalkSpeed100 << " Melee: " << tpl.wMeleeAtkRangeInit << " Shot: " << tpl.wShotAtkRangeInit << "\n";
            out.close();
        }
    }

    // Load Map Collision Data
    char modulePath[MAX_PATH];
    GetModuleFileNameA(NULL, modulePath, MAX_PATH);
    std::string exePath = modulePath;
    size_t lastSlash = exePath.find_last_of("\\/");
    std::string mapDir = (lastSlash != std::string::npos ? exePath.substr(0, lastSlash) : ".") + "\\MAP\\";
    
    WIN32_FIND_DATAA fd;
    HANDLE hFind = FindFirstFileA((mapDir + "*.mm").c_str(), &fd);
    if (hFind != INVALID_HANDLE_VALUE) {
        do {
            std::string fileName = fd.cFileName;
            DWORD mapId = std::stoi(fileName.substr(0, 4));
            
            std::ifstream ifs(mapDir + fileName, std::ios::binary);
            if (ifs.is_open()) {
                ifs.seekg(18, std::ios::beg);
                int width = 0, height = 0;
                ifs.read((char*)&width, 4);
                ifs.read((char*)&height, 4);
                
                int pixelOffset = 62;
                ifs.seekg(10, std::ios::beg);
                ifs.read((char*)&pixelOffset, 4);
                
                ifs.seekg(pixelOffset, std::ios::beg);
                bool isBottomUp = (height > 0);
                int absHeight = std::abs(height);
                sWorldMap wm; wm.dwMapID = mapId; wm.width = width; wm.height = absHeight;
                wm.collisionGrid.resize(width * absHeight);
                int rowPadding = (4 - (width & 3)) & 3;

                for (int r = 0; r < absHeight; ++r) {
                    int targetRow = isBottomUp ? (absHeight - 1 - r) : r;
                    ifs.read((char*)&wm.collisionGrid[targetRow * width], width);
                    if (rowPadding > 0) {
                        ifs.seekg(rowPadding, std::ios::cur);
                    }
                }
                g_WorldMaps[mapId] = wm;
                g_MapInstances[mapId] = new CMapInstance(mapId, width, absHeight, wm.collisionGrid);
                ifs.close();
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    
    LOG("[Server] Game Data Loaded (NPCs: " + std::to_string(g_NpcTemplates.size()) + ", Maps: " + std::to_string(g_WorldMaps.size()) + ")");
}

void LoadWorldObjects() {
    auto& dao = GameDataDB::GetInstance();
    std::vector<MonsterData> g_WorldObjects;
    
    // Load Functional NPCs
    dao.LoadFunctionalNpcs(g_WorldObjects);

    // Load Monsters / Regular NPCs
    dao.LoadNpcList(g_WorldObjects, g_NpcTemplates);
    
    LOG("[Server] Loaded " + std::to_string(g_WorldObjects.size()) + " Spawn Points into memory.");
    
    // Load FunctionalNpcItem
    dao.LoadFunctionalNpcItems(g_WorldObjects);

    // Transfer all loaded objects to their respective map instances
    for (auto& obj : g_WorldObjects) {
        if (g_MapInstances.count(obj.dwMapID)) {
            g_MapInstances[obj.dwMapID]->AddMonster(obj);
        } else {
            LOG("[Server] WARNING: Spawned object " + std::to_string(obj.dwObjectID) + " on unknown MapID " + std::to_string(obj.dwMapID));
        }
    }
}

BYTE FindFreeSackPos(DWORD charID, BYTE sackID, BYTE bCX, BYTE bCY) {
    if (bCX < 1) bCX = 1;
    if (bCY < 1) bCY = 1;
    
    // 如果是非 VIP 玩家，绝对不允许物品自动生成、分配或拾取落入第三页背包中
    if (sackID == 3) {
        if (CharacterDB::GetInstance().GetVipLevel(charID) == 0) {
            return 255; 
        }
    }
    
    int startPos = (sackID == 1) ? 20 : (sackID == 2) ? 60 : 100;
    int endPos = startPos + 35;
    
    // Build 6x6 occupancy grid: grid[row][col]
    bool grid[6][6] = {false};
    
    std::vector<ItemDB::SackOccupancy> occ;
    ItemDB::GetInstance().GetSackOccupancy(charID, startPos, endPos, occ);
    for (auto& r : occ) {
        int x = (r.bSackPos - startPos) % 6;
        int y = (r.bSackPos - startPos) / 6;
        int cx = 1, cy = 1;
        if (g_ItemTemplates.count(r.wRefID)) {
            cx = g_ItemTemplates[r.wRefID].bCX;
            cy = g_ItemTemplates[r.wRefID].bCY;
        }
        if (cx < 1) cx = 1;
        if (cy < 1) cy = 1;
        for (int dy = 0; dy < cy; dy++)
            for (int dx = 0; dx < cx; dx++)
                if (y + dy < 6 && x + dx < 6)
                    grid[y + dy][x + dx] = true;
    }
    
    // Find first position where bCX x bCY fits
    for (int y = 0; y <= 6 - bCY; y++) {
        for (int x = 0; x <= 6 - bCX; x++) {
            bool fit = true;
            for (int dy = 0; dy < bCY && fit; dy++)
                for (int dx = 0; dx < bCX && fit; dx++)
                    if (grid[y + dy][x + dx]) fit = false;
            if (fit) return (BYTE)(startPos + y * 6 + x);
        }
    }
    return 255; // No space
}

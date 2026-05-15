#include "ServerCore.h"
#include "DBHelper.h"
#include "GameObjects/DropManager.h"
#include "GameObjects/MugongManager.h"
#include "GameObjects/MapInstance.h"
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
    return send(s, buf, len, flags);
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

void LoadGameData() {
    g_NpcTemplates.clear();
    g_ItemTemplates.clear();
    g_LevelTemplates.clear();
    
    // Load Realm and Channels into memory
    g_Config.realmName = "XiahEmu Local";
    std::string qRealm = "SELECT TOP 1 szworldname FROM " + g_Config.dbServerInfo + ".dbo.worldlist";
    DBHelper::GetInstance().ExecuteQuery(qRealm, [&](SQLHSTMT hStmt) {
        char buf[128] = {0}; SQLLEN cb = 0;
        if (SQL_SUCCEEDED(SQLGetData(hStmt, 1, SQL_C_CHAR, buf, sizeof(buf), &cb)) && cb > 0 && cb != SQL_NULL_DATA) {
            g_Config.realmName = buf;
        }
    });

    g_Config.cachedChannels.clear();
    std::string qChan = "SELECT bchannelid, szChannelName, szChannelDes, wmaxuser, btype, bage FROM " + g_Config.dbServerInfo + ".dbo.CHANNELLIST";
    DBHelper::GetInstance().ExecuteQuery(qChan, [&](SQLHSTMT hStmt) {
        sChannelInfo ci;
        SQLLEN cbId=0, cbName=0, cbDesc=0, cbCap=0, cbPvp=0, cbAge=0;
        char nameBuf[128] = {0}; char descBuf[128] = {0};
        SQLGetData(hStmt, 1, SQL_C_LONG, &ci.id, 0, &cbId);
        SQLGetData(hStmt, 2, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &cbName);
        SQLGetData(hStmt, 3, SQL_C_CHAR, descBuf, sizeof(descBuf), &cbDesc);
        SQLGetData(hStmt, 4, SQL_C_LONG, &ci.capacity, 0, &cbCap);
        SQLGetData(hStmt, 5, SQL_C_LONG, &ci.pvp, 0, &cbPvp);
        SQLGetData(hStmt, 6, SQL_C_LONG, &ci.age, 0, &cbAge);
        if (cbName != SQL_NULL_DATA && cbName > 0) ci.name = nameBuf;
        if (cbDesc != SQL_NULL_DATA && cbDesc > 0) ci.desc = descBuf;
        
        if (g_Config.channelPorts.find(ci.id) != g_Config.channelPorts.end()) {
            g_Config.cachedChannels.push_back(ci);
        }
    });

    LOG("[Server] Realm Info Loaded. RealmName: " + g_Config.realmName + " | Channels: " + std::to_string(g_Config.cachedChannels.size()));

    DropManager::GetInstance()->LoadDropTables();
    MugongManager::GetInstance()->LoadMugongData();
    
    // Load LEVELTEMPLATE
    std::string qLvl = "SELECT wLevel, bCharType, dwNeedExp, bSp, bTp FROM LEVELTEMPLATE";
    auto lvlCallback = [&](SQLHSTMT hStmt) {
        WORD wLevel=0; BYTE bCharType=0, bSp=0, bTp=0; long long int dwNeedExp=0; SQLLEN c;
        SQLGetData(hStmt, 1, SQL_C_USHORT, &wLevel, 0, &c);
        SQLGetData(hStmt, 2, SQL_C_UTINYINT, &bCharType, 0, &c);
        SQLGetData(hStmt, 3, SQL_C_SBIGINT, &dwNeedExp, 0, &c);
        SQLGetData(hStmt, 4, SQL_C_UTINYINT, &bSp, 0, &c);
        SQLGetData(hStmt, 5, SQL_C_UTINYINT, &bTp, 0, &c);
        
        sLevelTemplate tpl;
        tpl.wLevel = wLevel; tpl.bCharType = bCharType; tpl.dwNeedExp = dwNeedExp; tpl.bSp = bSp; tpl.bTp = bTp;
        g_LevelTemplates[wLevel][bCharType] = tpl;
    };
    DBHelper::GetInstance().ExecuteQuery(qLvl, lvlCallback);
    LOG("[Server] Loaded " + std::to_string(g_LevelTemplates.size()) + " Levels from LEVELTEMPLATE.");

    // Load ITEMTEMPLATE
    std::string qItem = "SELECT wRefID, bType, bKind, wVisualID, szName, dwCost, wLevel, bCharType, wAmount, nData1, nData2, nData3, nData4, nData5, nData6, nData7, nData8, nData9, nData10, nData13, nBasicData1, nBasicData2, nBasicData3, nBasicData4, nBasicData5, bCX, bCY FROM ITEMTEMPLATE";
    auto itemCallback = [&](SQLHSTMT hStmt) {
        WORD ref=0, vis=0, lvl=0, amt=0; BYTE type=0, kind=0, charType=0, bCX=1, bCY=1; DWORD cost=0; char nameBuf[64]; SQLLEN c[27];
        int d1=0, d2=0, d3=0, d4=0, d5=0, d6=0, d7=0, d8=0, d9=0, d10=0, d13=0;
        int b1=0, b2=0, b3=0, b4=0, b5=0;
        memset(nameBuf, 0, sizeof(nameBuf));
        SQLGetData(hStmt, 1, SQL_C_USHORT, &ref, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_UTINYINT, &type, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_UTINYINT, &kind, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_USHORT, &vis, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c[4]);
        SQLGetData(hStmt, 6, SQL_C_ULONG, &cost, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_USHORT, &lvl, 0, &c[6]);
        SQLGetData(hStmt, 8, SQL_C_UTINYINT, &charType, 0, &c[7]);
        SQLGetData(hStmt, 9, SQL_C_USHORT, &amt, 0, &c[8]);
        SQLGetData(hStmt, 10, SQL_C_SLONG, &d1, 0, &c[9]);
        SQLGetData(hStmt, 11, SQL_C_SLONG, &d2, 0, &c[10]);
        SQLGetData(hStmt, 12, SQL_C_SLONG, &d3, 0, &c[11]);
        SQLGetData(hStmt, 13, SQL_C_SLONG, &d4, 0, &c[12]);
        SQLGetData(hStmt, 14, SQL_C_SLONG, &d5, 0, &c[13]);
        SQLGetData(hStmt, 15, SQL_C_SLONG, &d6, 0, &c[14]);
        SQLGetData(hStmt, 16, SQL_C_SLONG, &d7, 0, &c[15]);
        SQLGetData(hStmt, 17, SQL_C_SLONG, &d8, 0, &c[16]);
        SQLGetData(hStmt, 18, SQL_C_SLONG, &d9, 0, &c[17]);
        SQLGetData(hStmt, 19, SQL_C_SLONG, &d10, 0, &c[18]);
        SQLGetData(hStmt, 20, SQL_C_SLONG, &d13, 0, &c[19]);
        SQLGetData(hStmt, 21, SQL_C_SLONG, &b1, 0, &c[20]);
        SQLGetData(hStmt, 22, SQL_C_SLONG, &b2, 0, &c[21]);
        SQLGetData(hStmt, 23, SQL_C_SLONG, &b3, 0, &c[22]);
        SQLGetData(hStmt, 24, SQL_C_SLONG, &b4, 0, &c[23]);
        SQLGetData(hStmt, 25, SQL_C_SLONG, &b5, 0, &c[24]);
        SQLGetData(hStmt, 26, SQL_C_UTINYINT, &bCX, 0, &c[25]);
        SQLGetData(hStmt, 27, SQL_C_UTINYINT, &bCY, 0, &c[26]);
        
        sItemTemplate tpl;
        tpl.wRefID = ref; tpl.bType = type; tpl.bKind = kind; tpl.wVisualID = vis;
        if (c[4] != SQL_NULL_DATA) tpl.szName = nameBuf;
        tpl.dwCost = cost; tpl.wLevel = lvl; tpl.bCharType = charType; tpl.wAmount = amt;
        tpl.bCX = bCX; tpl.bCY = bCY;
        if (tpl.bCX == 0) tpl.bCX = 1;
        if (tpl.bCY == 0) tpl.bCY = 1;
        tpl.nData1 = d1; tpl.nData2 = d2; tpl.nData3 = d3; tpl.nData4 = d4; tpl.nData5 = d5;
        tpl.nData6 = d6; tpl.nData7 = d7; tpl.nData8 = d8; tpl.nData9 = d9; tpl.nData10 = d10; tpl.nData13 = d13;
        tpl.nBasicData1 = b1; tpl.nBasicData2 = b2; tpl.nBasicData3 = b3; tpl.nBasicData4 = b4; tpl.nBasicData5 = b5;
        g_ItemTemplates[ref] = tpl;
    };
    DBHelper::GetInstance().ExecuteQuery(qItem, itemCallback);
    LOG("[Server] Loaded " + std::to_string(g_ItemTemplates.size()) + " Items from ITEMTEMPLATE.");

    // Load NPCTEMPLATE
    std::string q = "SELECT bType, dwHpInit, dwPwrInit, dwDefInit, wWalkSpeed100, wSightRangeInit, wWanderRangeInit, wMeleeAtkRangeInit, wShotAtkRangeInit, szName, wAtkRatio, wAvoidRatio, wRegen, dwExpInit, wHealPoint, bIdleRatio, bLevel, bWalkSpeed, wAtkInterval, wStaggerTime FROM NPCTEMPLATE";
    auto npcCallback = [&](SQLHSTMT hStmt) {
        int t=0, hp=0, pwr=0, def=0, wspeed=0, sight=0, wander=0, melee=0, shot=0, atkratio=0, avoidratio=0, regen=0, expinit=0, healpoint=0, idleratio=0, lvl=0, walkspdbyte=0, atkinterval=1500, staggertime=500; 
        char nameBuf[64] = {0}; 
        SQLLEN c[20] = {0};
        SQLGetData(hStmt, 1, SQL_C_SLONG, &t, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &hp, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &pwr, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &def, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &wspeed, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_SLONG, &sight, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &wander, 0, &c[6]);
        SQLGetData(hStmt, 8, SQL_C_SLONG, &melee, 0, &c[7]);
        SQLGetData(hStmt, 9, SQL_C_SLONG, &shot, 0, &c[8]);
        SQLGetData(hStmt, 10, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c[9]);
        SQLGetData(hStmt, 11, SQL_C_SLONG, &atkratio, 0, &c[10]);
        SQLGetData(hStmt, 12, SQL_C_SLONG, &avoidratio, 0, &c[11]);
        SQLGetData(hStmt, 13, SQL_C_SLONG, &regen, 0, &c[12]);
        SQLGetData(hStmt, 14, SQL_C_SLONG, &expinit, 0, &c[13]);
        SQLGetData(hStmt, 15, SQL_C_SLONG, &healpoint, 0, &c[14]);
        SQLGetData(hStmt, 16, SQL_C_SLONG, &idleratio, 0, &c[15]);
        SQLGetData(hStmt, 17, SQL_C_SLONG, &lvl, 0, &c[16]);
        SQLGetData(hStmt, 18, SQL_C_SLONG, &walkspdbyte, 0, &c[17]);
        SQLGetData(hStmt, 19, SQL_C_SLONG, &atkinterval, 0, &c[18]);
        SQLGetData(hStmt, 20, SQL_C_SLONG, &staggertime, 0, &c[19]);
        
        sNpcTemplate tpl;
        tpl.bType = t; tpl.dwHpInit = hp; tpl.dwPwrInit = pwr; tpl.dwDefInit = def;
        if (c[16] != SQL_NULL_DATA) tpl.bLevel = lvl; else tpl.bLevel = 1;
        if (c[17] != SQL_NULL_DATA) tpl.bWalkSpeed = walkspdbyte; else tpl.bWalkSpeed = 8;
        if (c[13] != SQL_NULL_DATA) tpl.dwExpInit = expinit; else tpl.dwExpInit = 0;
        tpl.wWalkSpeed100 = wspeed; tpl.wSightRangeInit = sight; tpl.wWanderRangeInit = wander;
        tpl.wMeleeAtkRangeInit = melee; tpl.wShotAtkRangeInit = shot;
        if (c[10] != SQL_NULL_DATA) tpl.wAtkRatio = atkratio; else tpl.wAtkRatio = 0;
        if (c[11] != SQL_NULL_DATA) tpl.wAvoidRatio = avoidratio; else tpl.wAvoidRatio = 0;
        if (c[12] != SQL_NULL_DATA) tpl.wRegen = regen; else tpl.wRegen = 10;
        if (c[14] != SQL_NULL_DATA) tpl.wHealPoint = healpoint; else tpl.wHealPoint = 0;
        if (c[15] != SQL_NULL_DATA) tpl.bIdleRatio = idleratio; else tpl.bIdleRatio = 50;
        if (c[18] != SQL_NULL_DATA) tpl.wAtkInterval = atkinterval; else tpl.wAtkInterval = 1500;
        if (c[19] != SQL_NULL_DATA) tpl.wStaggerTime = staggertime; else tpl.wStaggerTime = 500;
        if (c[9] != SQL_NULL_DATA) tpl.szName = nameBuf;
        if (tpl.wShotAtkRangeInit > 0 || tpl.wMeleeAtkRangeInit > 20) {
            std::ofstream out("npc_debug.txt", std::ios::app);
            out << "NPC_ID_" << t << " Walk: " << tpl.wWalkSpeed100 << " Melee: " << tpl.wMeleeAtkRangeInit << " Shot: " << tpl.wShotAtkRangeInit << "\n";
            out.close();
        }
        g_NpcTemplates[t] = tpl;
    };
    DBHelper::GetInstance().ExecuteQuery(q, npcCallback);
    
    // Load NPC_MUGONGTEMPLATE
    std::string qMugong = "SELECT bNpcType, dwMugongID, bMugongLevel, bSelectID, wSelectParam1, wSelectParam2, wAttackRange FROM NPC_MUGONGTEMPLATE";
    auto mugongCallback = [&](SQLHSTMT hStmt) {
        int t, mid, mlvl, sel, p1, p2, arange; SQLLEN c[7];
        SQLGetData(hStmt, 1, SQL_C_SLONG, &t, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &mid, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &mlvl, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &sel, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &p1, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_SLONG, &p2, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &arange, 0, &c[6]);
        
        if (g_NpcTemplates.count(t)) {
            sNpcMugong mg;
            mg.dwMugongID = mid; mg.bMugongLevel = mlvl; mg.bSelectID = sel;
            mg.wSelectParam1 = p1; mg.wSelectParam2 = p2; mg.wAttackRange = arange;
            g_NpcTemplates[t].mugongs.push_back(mg);
        }
    };
    DBHelper::GetInstance().ExecuteQuery(qMugong, mugongCallback);

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
                sWorldMap wm; wm.dwMapID = mapId; wm.width = width; wm.height = height;
                wm.collisionGrid.resize(width * height);
                ifs.read((char*)wm.collisionGrid.data(), width * height);
                g_WorldMaps[mapId] = wm;
                g_MapInstances[mapId] = new CMapInstance(mapId, width, height, wm.collisionGrid);
                ifs.close();
            }
        } while (FindNextFileA(hFind, &fd));
        FindClose(hFind);
    }
    
    LOG("[Server] Game Data Loaded (NPCs: " + std::to_string(g_NpcTemplates.size()) + ", Maps: " + std::to_string(g_WorldMaps.size()) + ")");
}

void LoadWorldObjects() {
    std::vector<sServerObject> g_WorldObjects;
    
    // Load Functional NPCs
    std::string qNpc = "SELECT dwID, dwMapID, wPosX, wPosY, bHeight, bType, szName FROM FunctionalNpcList";
    auto npcCallback = [&](SQLHSTMT hStmt) {
        int id, mapid, x, y, h, t; char nameBuf[64]; SQLLEN c[7];
        SQLGetData(hStmt, 1, SQL_C_SLONG, &id, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &mapid, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &x, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &y, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &h, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_SLONG, &t, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c[6]);
        
        sServerObject obj;
        obj.dwObjectID = id + 100000;
        obj.bObjectType = 5;
        obj.dwMapID = mapid; 
        obj.wPosX = x; obj.wPosY = y; obj.fPosX = (float)x; obj.fPosY = (float)y; obj.wSpawnX = x; obj.wSpawnY = y; obj.bHeight = h; obj.bPropType = t;
        if (c[6] != SQL_NULL_DATA) obj.szName = nameBuf;
        g_WorldObjects.push_back(obj);
    };
    DBHelper::GetInstance().ExecuteQuery(qNpc, npcCallback);

    // Load Monsters / Regular NPCs
    std::string qMob = "SELECT dwID, dwMapID, wPosX, wPosY, bNpcType, szName, dwMovePattern, dwAttackPattern, wWanderRangeInc, wCount, wPosRange, wSightRangeInc, wHpInc, wPwrInc, wDefInc, dwExpInc, wMeleeAtkRangeInc, wShotAtkRangeInc, wRootItem, wRootMoney, wRootRes, wRootBook, wLevel, bGroupOrder, wAtkRatioInc, wAvoidRatioInc FROM NPCLIST";
    auto mobCallback = [&](SQLHSTMT hStmt) {
        int id=0, mapid=0, x=0, y=0, t=0, mp=0, ap=0, wander=0, count=0, range=0, sightInc=0, hpInc=0, pwrInc=0, defInc=0, expInc=0, meleeInc=0, shotInc=0, rItem=0, rMoney=0, rRes=0, rBook=0, npcLevel=0, groupOrder=0, atkRatioInc=0, avoidRatioInc=0; 
        char nameBuf[64] = {0}; 
        SQLLEN c[26] = {0};
        SQLGetData(hStmt, 1, SQL_C_SLONG, &id, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &mapid, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &x, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &y, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &t, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_CHAR, nameBuf, sizeof(nameBuf), &c[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &mp, 0, &c[6]);
        SQLGetData(hStmt, 8, SQL_C_SLONG, &ap, 0, &c[7]);
        SQLGetData(hStmt, 9, SQL_C_SLONG, &wander, 0, &c[8]);
        SQLGetData(hStmt, 10, SQL_C_SLONG, &count, 0, &c[9]);
        SQLGetData(hStmt, 11, SQL_C_SLONG, &range, 0, &c[10]);
        SQLGetData(hStmt, 12, SQL_C_SLONG, &sightInc, 0, &c[11]);
        SQLGetData(hStmt, 13, SQL_C_SLONG, &hpInc, 0, &c[12]);
        SQLGetData(hStmt, 14, SQL_C_SLONG, &pwrInc, 0, &c[13]);
        SQLGetData(hStmt, 15, SQL_C_SLONG, &defInc, 0, &c[14]);
        SQLGetData(hStmt, 16, SQL_C_SLONG, &expInc, 0, &c[15]);
        SQLGetData(hStmt, 17, SQL_C_SLONG, &meleeInc, 0, &c[16]);
        SQLGetData(hStmt, 18, SQL_C_SLONG, &shotInc, 0, &c[17]);
        SQLGetData(hStmt, 19, SQL_C_SLONG, &rItem, 0, &c[18]);
        SQLGetData(hStmt, 20, SQL_C_SLONG, &rMoney, 0, &c[19]);
        SQLGetData(hStmt, 21, SQL_C_SLONG, &rRes, 0, &c[20]);
        SQLGetData(hStmt, 22, SQL_C_SLONG, &rBook, 0, &c[21]);
        SQLGetData(hStmt, 23, SQL_C_SLONG, &npcLevel, 0, &c[22]);
        SQLGetData(hStmt, 24, SQL_C_SLONG, &groupOrder, 0, &c[23]);
        SQLGetData(hStmt, 25, SQL_C_SLONG, &atkRatioInc, 0, &c[24]);
        SQLGetData(hStmt, 26, SQL_C_SLONG, &avoidRatioInc, 0, &c[25]);
        
        if (c[9] == SQL_NULL_DATA || count <= 0) count = 1;
        if (c[10] == SQL_NULL_DATA) range = 0;
        
        for (int i = 0; i < count; ++i) {
            sServerObject obj;
            // Generate a unique ID for each instance by combining DB ID and index
            obj.dwObjectID = id + 200000 + (i * 1000000); 
            obj.bObjectType = 3;
            obj.dwMapID = mapid; 
            
            // Xiah Spawn Scatter Logic (wCount + wPosRange):
            // Instead of defining 10 database rows for 10 boars, the design uses 1 row with wCount=10.
            // This loop generates all instances, scattering them randomly within a radius (wPosRange)
            // around the center coordinates (wPosX, wPosY).
            int offsetX = 0, offsetY = 0;
            if (range > 0) {
                offsetX = (rand() % (range * 2 + 1)) - range;
                offsetY = (rand() % (range * 2 + 1)) - range;
            }
            
            obj.wPosX = x + offsetX; 
            if (obj.wPosX < 0) obj.wPosX = 0;
            obj.wPosY = y + offsetY; 
            if (obj.wPosY < 0) obj.wPosY = 0;
            obj.fPosX = (float)obj.wPosX;
            obj.fPosY = (float)obj.wPosY;
            
            obj.wSpawnX = x; obj.wSpawnY = y; 
            obj.wSpawnRange = range;
            obj.bHeight = 1; obj.bPropType = t;
            obj.dwMovePattern = mp; obj.dwAttackPattern = ap;
            
            if (c[18] != SQL_NULL_DATA) obj.wRootItem = rItem; else obj.wRootItem = 0;
            if (c[19] != SQL_NULL_DATA) obj.wRootMoney = rMoney; else obj.wRootMoney = 0;
            if (c[20] != SQL_NULL_DATA) obj.wRootRes = rRes; else obj.wRootRes = 0;
            if (c[21] != SQL_NULL_DATA) obj.wRootBook = rBook; else obj.wRootBook = 0;
            
            if (c[5] != SQL_NULL_DATA) obj.szName = nameBuf;
            if (obj.szName.empty() && g_NpcTemplates.count(t)) obj.szName = g_NpcTemplates[t].szName;
            
            if (g_NpcTemplates.count(t)) { 
                sNpcTemplate& tpl = g_NpcTemplates[t];
                
                // Xiah Stat Calculation Logic (Init + Inc):
                // `Init` is the base template property (Species genetic baseline).
                // `Inc` is the spawn instance modifier (Zone-specific buffs).
                // Final value is always (Base + Modifier). This allows reusing models 
                // while dynamically tuning their difficulty per spawn zone.
                obj.dwHpMax = tpl.dwHpInit + (c[12] != SQL_NULL_DATA ? hpInc : 0);
                obj.dwHpCur = obj.dwHpMax;
                
                obj.wWepAtk = tpl.dwPwrInit + (c[13] != SQL_NULL_DATA ? pwrInc : 0);
                obj.wWepDef = tpl.dwDefInit + (c[14] != SQL_NULL_DATA ? defInc : 0);
                
                obj.wSightRange = tpl.wSightRangeInit + (c[11] != SQL_NULL_DATA ? sightInc : 0);
                obj.wWanderRange = tpl.wWanderRangeInit + (c[8] != SQL_NULL_DATA ? wander : 0);
                
                obj.wMeleeAtkRange = tpl.wMeleeAtkRangeInit + (c[16] != SQL_NULL_DATA ? meleeInc : 0);
                obj.wShotAtkRange = tpl.wShotAtkRangeInit + (c[17] != SQL_NULL_DATA ? shotInc : 0);
                
                obj.dwExp = tpl.dwExpInit + (c[15] != SQL_NULL_DATA ? expInc : 0);
                
                // wLevel: Use NPCLIST value if > 0, otherwise fallback to NPCTEMPLATE.bLevel
                if (c[22] != SQL_NULL_DATA && npcLevel > 0)
                    obj.wLevel = (WORD)npcLevel;
                else
                    obj.wLevel = (WORD)tpl.bLevel;
                
                // bGroupOrder: From NPCLIST (maps to client bOrderID, affects model scale)
                if (c[23] != SQL_NULL_DATA) obj.bGroupOrder = (BYTE)groupOrder; else obj.bGroupOrder = 0;
                
                // wAtkRatio / wAvoidRatio: Init + Inc
                obj.wAtkRatio = tpl.wAtkRatio + (c[24] != SQL_NULL_DATA ? atkRatioInc : 0);
                obj.wAvoidRatio = tpl.wAvoidRatio + (c[25] != SQL_NULL_DATA ? avoidRatioInc : 0);
                
                // bWalkSpeedByte: From NPCTEMPLATE (client display speed)
                obj.bWalkSpeedByte = tpl.bWalkSpeed;
            }
            
            g_WorldObjects.push_back(obj);
        }
    };
    DBHelper::GetInstance().ExecuteQuery(qMob, mobCallback);
    
    LOG("[Server] Loaded " + std::to_string(g_WorldObjects.size()) + " Spawn Points into memory.");
    
    // Load FunctionalNpcItem
    std::string qNpcItems = "SELECT dwObjectID, dwItemID, dwPrice, wAmount, bRarity, bPos, bSackCnt FROM FunctionalNpcItem";
    auto npcItemCallback = [&](SQLHSTMT hStmt) {
        int objid=0, itemid=0, price=0; int amt=0, rarity=0, pos=0, sackcnt=0;
        SQLLEN c[7];
        SQLGetData(hStmt, 1, SQL_C_SLONG, &objid, 0, &c[0]);
        SQLGetData(hStmt, 2, SQL_C_SLONG, &itemid, 0, &c[1]);
        SQLGetData(hStmt, 3, SQL_C_SLONG, &price, 0, &c[2]);
        SQLGetData(hStmt, 4, SQL_C_SLONG, &amt, 0, &c[3]);
        SQLGetData(hStmt, 5, SQL_C_SLONG, &rarity, 0, &c[4]);
        SQLGetData(hStmt, 6, SQL_C_SLONG, &pos, 0, &c[5]);
        SQLGetData(hStmt, 7, SQL_C_SLONG, &sackcnt, 0, &c[6]);
        
        DWORD actualObjID = objid + 100000;
        for (auto& obj : g_WorldObjects) {
            if (obj.dwObjectID == actualObjID && obj.bObjectType == 5) {
                sFunctionalNpcItem item;
                item.dwItemID = itemid;
                item.dwPrice = price;
                item.wAmount = amt;
                item.bRarity = rarity;
                item.bPos = pos;
                item.bSackCnt = sackcnt;
                obj.npcItems.push_back(item);
                break;
            }
        }
    };
    DBHelper::GetInstance().ExecuteQuery(qNpcItems, npcItemCallback);

    // [Phase 1] Transfer all loaded objects to their respective map instances
    for (auto& obj : g_WorldObjects) {
        if (g_MapInstances.count(obj.dwMapID)) {
            g_MapInstances[obj.dwMapID]->AddMonster(obj);
        } else {
            // Unmapped objects
            LOG("[Server] WARNING: Spawned object " + std::to_string(obj.dwObjectID) + " on unknown MapID " + std::to_string(obj.dwMapID));
        }
    }
}

BYTE FindFreeSackPos(DWORD charID, BYTE sackID, BYTE bCX, BYTE bCY) {
    if (bCX < 1) bCX = 1;
    if (bCY < 1) bCY = 1;
    
    int startPos = (sackID == 1) ? 20 : (sackID == 2) ? 60 : 100;
    int endPos = startPos + 35;
    
    // Build 6x6 occupancy grid: grid[row][col]
    bool grid[6][6] = {false};
    
    std::string q = "SELECT S.bSackPos, I.wRefID FROM SACKITEM S JOIN ITEM I ON S.dwItemID = I.dwItemID WHERE S.dwCharID = " + std::to_string(charID) + " AND S.bSackPos >= " + std::to_string(startPos) + " AND S.bSackPos <= " + std::to_string(endPos);
    DBHelper::GetInstance().ExecuteQuery(q, [&](SQLHSTMT hStmt) {
        int pos = 0; WORD ref = 0; SQLLEN c1, c2;
        SQLGetData(hStmt, 1, SQL_C_SLONG, &pos, 0, &c1);
        SQLGetData(hStmt, 2, SQL_C_USHORT, &ref, 0, &c2);
        int x = (pos - startPos) % 6;
        int y = (pos - startPos) / 6;
        int cx = 1, cy = 1;
        if (g_ItemTemplates.count(ref)) {
            cx = g_ItemTemplates[ref].bCX;
            cy = g_ItemTemplates[ref].bCY;
        }
        if (cx < 1) cx = 1;
        if (cy < 1) cy = 1;
        for (int dy = 0; dy < cy; dy++)
            for (int dx = 0; dx < cx; dx++)
                if (y + dy < 6 && x + dx < 6)
                    grid[y + dy][x + dx] = true;
    });
    
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

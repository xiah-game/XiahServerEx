#include "../ServerCore.h"
#include "../Network/SessionMgr.h"
#include "../DBHelper.h"
#include <ctime>
#include <map>

// CS_EV_TIME_ACK = OFFSET_CS_EV + 0 = 0x3101 + 0x1000 + 0 = 0x4101
// Payload: WORD wYear + BYTE bMonth + BYTE bDay + BYTE bHour
static const WORD OPCODE_TIME_ACK = 0x4101;

// ====================================================================
// 调试用时间覆盖：DEBUG_HOUR
// 值为 -1 时使用系统真实时间, 0~23 时固定为指定小时
// 可在 config.ini 中配置 [Server] DEBUG_HOUR=0 快速测试黑夜
// ====================================================================
static int g_debugHour = -1;

// 获取 config.ini 的绝对路径（与 exe 同目录）
static std::string GetConfigPath() {
    char exePath[MAX_PATH];
    GetModuleFileNameA(NULL, exePath, MAX_PATH);
    std::string path(exePath);
    size_t pos = path.find_last_of("\\/");
    return path.substr(0, pos) + "\\config.ini";
}

void LoadDebugHour() {
    static std::string cfgPath = GetConfigPath();
    char buf[16] = {0};
    GetPrivateProfileStringA("Server", "DEBUG_HOUR", "-1", buf, sizeof(buf), cfgPath.c_str());
    g_debugHour = atoi(buf);
    if (g_debugHour < 0 || g_debugHour > 23) {
        g_debugHour = -1;
    }
}

// 获取当前世界时间（支持调试覆盖）
static void GetWorldTime(WORD& wYear, BYTE& bMonth, BYTE& bDay, BYTE& bHour) {
    // 每次获取时间时热读配置，确保登录和广播都生效
    LoadDebugHour();
    
    time_t now = time(nullptr);
    struct tm lt;
    localtime_s(&lt, &now);

    wYear = (WORD)(lt.tm_year + 1900);
    bMonth = (BYTE)(lt.tm_mon + 1);
    bDay = (BYTE)lt.tm_mday;
    bHour = (g_debugHour >= 0) ? (BYTE)g_debugHour : (BYTE)lt.tm_hour;
}

// 构建时间包
static std::vector<BYTE> BuildTimePacket() {
    WORD wYear; BYTE bMonth, bDay, bHour;
    GetWorldTime(wYear, bMonth, bDay, bHour);

    std::vector<BYTE> pkt(sizeof(PACKET_HEADER));
    pkt.push_back(wYear & 0xFF); pkt.push_back((wYear >> 8) & 0xFF);
    pkt.push_back(bMonth);
    pkt.push_back(bDay);
    pkt.push_back(bHour);

    PACKET_HEADER* hdr = (PACKET_HEADER*)pkt.data();
    hdr->id = OPCODE_TIME_ACK;
    hdr->payloadSize = pkt.size() - sizeof(PACKET_HEADER);
    EncryptPacket(pkt.data(), 0x42);
    return pkt;
}

// 发送时间包到指定 socket
void SendWorldTimeToSocket(SOCKET s) {
    auto pkt = BuildTimePacket();
    SafeSend(s, (const char*)pkt.data(), (int)pkt.size(), 0);
}

// 广播当前时间给所有在线玩家
void BroadcastWorldTime() {
    // 每次广播时重新读取调试配置（允许热更改）
    LoadDebugHour();
    
    WORD wYear; BYTE bMonth, bDay, bHour;
    GetWorldTime(wYear, bMonth, bDay, bHour);

    auto pkt = BuildTimePacket();
    SessionMgr::GetInstance().BroadcastToAll(pkt);

    LOG("[WorldTime] 广播世界时间: " + std::to_string(wYear) + "/" + 
        std::to_string(bMonth) + "/" + std::to_string(bDay) + " " + std::to_string(bHour) + "时" +
        (g_debugHour >= 0 ? " [调试模式]" : ""));
}

// ====================================================================
// 天气系统：基于 LinkMapWeather 表的地图区域天气
// CS_EV_WEATHER_ACK = 0x4102, Payload: BYTE bWeatherType
// bWeatherType: 0=晴天, 1=雨, 2=雪, 3=其他
// ====================================================================

static const WORD OPCODE_WEATHER_ACK = 0x4102;

// 地图天气配置缓存: mapID -> weatherType
static std::map<DWORD, BYTE> g_mapWeather;
static bool g_weatherLoaded = false;

// 从数据库加载地图天气配置
void LoadMapWeatherConfig() {
    g_mapWeather.clear();
    
    // 从 xiah 库查询 LinkMapWeather 表（每张地图取一种天气类型）
    std::string sql = "SELECT DISTINCT dwMapID, bWeatherType FROM LinkMapWeather";
    DBHelper::GetInstance().ExecuteQuery(sql, [](SQLHSTMT hStmt) {
        DWORD dwMapID = 0;
        BYTE bWeatherType = 0;
        SQLLEN cb1, cb2;
        SQLGetData(hStmt, 1, SQL_C_LONG, &dwMapID, sizeof(dwMapID), &cb1);
        SQLGetData(hStmt, 2, SQL_C_TINYINT, &bWeatherType, sizeof(bWeatherType), &cb2);
        if (cb1 > 0 && dwMapID > 0) {
            g_mapWeather[dwMapID] = bWeatherType;
            LOG("[Weather] 地图天气配置: MapID=" + std::to_string(dwMapID) + 
                " WeatherType=" + std::to_string(bWeatherType) + 
                (bWeatherType == 1 ? " (雨)" : bWeatherType == 2 ? " (雪)" : " (晴)"));
        }
    });
    
    g_weatherLoaded = true;
    LOG("[Weather] 地图天气配置加载完成, 共 " + std::to_string(g_mapWeather.size()) + " 张地图有天气效果");
}

// 根据地图ID获取天气类型
BYTE GetWeatherForMap(DWORD dwMapID) {
    if (!g_weatherLoaded) LoadMapWeatherConfig();
    auto it = g_mapWeather.find(dwMapID);
    if (it != g_mapWeather.end()) return it->second;
    return 0; // 默认晴天
}

// 向指定 socket 发送天气包
void SendWeatherToSocket(SOCKET s, DWORD dwMapID) {
    BYTE bWeatherType = GetWeatherForMap(dwMapID);
    
    std::vector<BYTE> pkt(sizeof(PACKET_HEADER));
    pkt.push_back(bWeatherType);
    
    PACKET_HEADER* hdr = (PACKET_HEADER*)pkt.data();
    hdr->id = OPCODE_WEATHER_ACK;
    hdr->payloadSize = pkt.size() - sizeof(PACKET_HEADER);
    EncryptPacket(pkt.data(), 0x42);
    SafeSend(s, (const char*)pkt.data(), (int)pkt.size(), 0);
}

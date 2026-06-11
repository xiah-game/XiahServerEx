#include "ServerCore.h"
#include "AuthServer.h"
#include "UnitServer.h"
#include "Network/AuthCenter.h"
#include <thread>
#include <fstream>
#include <chrono>

#include <ctime>

void InitPacketHandlers();
void BroadcastWorldTime();
void SendWorldTimeToSocket(SOCKET s);
void LoadMapWeatherConfig();

int main() {
    // Clear log file
    std::ofstream ofs("server_debug.log", std::ios::trunc);
    ofs.close();

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) return 1;

    srand((unsigned int)time(NULL));

    LOG("===========================================");
    LOG("  MODERN C++ XIAH MONOLITHIC SERVER V4     ");
    LOG("     (NOW WITH ODBC LIVE DB FETCH)         ");
    LOG("===========================================");

    LoadConfig();
    InitPacketHandlers();
    LoadGameData();
    LoadWorldObjects();
    LoadMapWeatherConfig();
    std::thread unitThread(RunUnitSvr);

    std::thread cleanupThread([]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(30));
            AuthCenter::Get().CleanupExpired();
        }
    });
    cleanupThread.detach();

    // 世界时间广播线程：每5分钟广播一次当前时间给所有在线玩家
    std::thread worldTimeThread([]() {
        LOG("[WorldTime] 世界时间广播线程启动");
        while (true) {
            std::this_thread::sleep_for(std::chrono::minutes(5));
            BroadcastWorldTime();
        }
    });
    worldTimeThread.detach();

    RunAuthSvr();

    unitThread.join();
    WSACleanup();
    return 0;
}

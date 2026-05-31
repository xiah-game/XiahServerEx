#pragma once
#include <winsock2.h>
#include <windows.h>
#include <string>

// TitleManager: 称号系统高内聚独立管理器
// 实现称号逻辑的 100% 物理隔离与解耦，彻底避免关联 Bug，保证业务代码极致清晰浅显
class TitleManager {
public:
    // 聚合计算并累加玩家所有处于激活状态且生效的称号加成属性（固定值、百分比及特殊经验/暴率）
    static void GetTitleStats(DWORD dwCharID, 
                              int& str, int& dex, int& vit, int& sus, 
                              int& atk, int& def, int& hp, int& mp, 
                              int& hit, int& dodge, int& crit, int& rHp, int& rMp,
                              int& strPerc, int& dexPerc, int& vitPerc, int& susPerc,
                              int& atkPerc, int& defPerc, int& hpPerc, int& mpPerc,
                              int& expPerc, int& dropPerc);

    // 安全获取玩家当前激活且佩戴称号的 IconID（若未佩戴则返回 0）
    static DWORD GetActiveTitleIconID(DWORD dwCharID);

    // 处理客户端 UI: 51 称号切换倒三角按钮的点击业务请求
    static void HandleSwitchTitle(SOCKET clientSocket, DWORD dwCharID);

    // 处理客户端 UI: 50 称号信息文本框的点击查询业务请求
    static void HandleQueryTitle(SOCKET clientSocket, DWORD dwCharID);
};

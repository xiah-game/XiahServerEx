#pragma once
#include "../ServerCore.h"
#include "../DB/ItemDB.h"
#include <vector>
#include <string>

// =================================================================
// 物品序列化公共模块
// 
// 将客户端 GetItemData 协议的字节序列化逻辑集中在此，
// 所有 Handler（Item/Bank/Trade/Rebuild/Shop）统一调用，
// 避免同一段逻辑散落在 8+ 处导致的同步修改遗漏风险。
// =================================================================

// 基础序列化 helper 定义在 ItemSerializer.cpp 内部（static），
// 各 Handler 保留自己原有的同名 static 版本不受影响。

/// 序列化完整的 GetItemData 结构到缓冲区
/// 包含：dwItemID, wRefID, bType, bKind, wVisualID, szName, dwPrice, wLevel,
///       bNeedCharType, wAmount, 以及各 bType 对应的类型专属数据字段。
/// 注意：不包含前导的 bSackPos，也不包含尾部的 wRebuithValue(nData25)，
///       调用者需根据协议自行追加。
void SerializeItemData(const ItemDB::FullItemRow& row, std::vector<BYTE>& buf);

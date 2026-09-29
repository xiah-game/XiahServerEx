# Xiah 反 WPE 封包注入 设计文档

> 目标：禁止玩家通过 WPE/封包工具直接向服务端发送任意业务包，令服务端只接受**当前合法登录会话**发出的、**未被篡改、未被重放**的封包。

---

## 1. 威胁模型

| 编号 | 攻击场景 | 典型危害 |
|---|---|---|
| T1 | 跳过 AuthServer，直连 UnitServer 端口发 `CS_IT_LOGINCHECK_REQ` | 任意账号登录 / 影子账号 |
| T2 | 抓包后**重放** `CS_IM_USE_REQ`、`CS_IT_BUYITEM_REQ`、`CS_AT_*` 等 | 复制物品、刷金、连续放招、无 CD |
| T3 | 修改 payload 字段 (itemID / count / targetID / pos) 后发送 | 偷他人物品、瞬移、打非视野怪 |
| T4 | 伪造 `dwKey` / channelID / protocolVer 直接进 UnitServer | 绕过登录流程 |
| T5 | 高频发包刷服务端逻辑 (每帧 N 次攻击 / 移动) | DoS / 业务作弊 |
| T6 | 跨号操作 (A 号的 socket 发 B 号 charID 的请求) | 越权操作 |

---

## 2. 现状速诊

| 防线 | 现状文件/位置 | 问题 |
|---|---|---|
| 传输加密 | `ServerCore.cpp:101-130`、`XiahClient/NetMsg.cpp:263-296` | 累加混淆 + 固定 key `0x42`，等同明文 |
| 会话鉴权 | `AuthServer.cpp:138-144` 写 `dwKey = 46` 硬编码；`Handlers/LoginHandler.cpp:OnLoginCheckReq` 完全不读 dwKey | T1/T4 直接成立 |
| 来源校验 | `Network/PacketRouter.cpp:RoutePacket` 直接按 id 查表执行 | 任意业务包都能跑 handler |
| 包内字段 | 各 handler 直接信任 payload | T3/T6 成立 |
| 重放/序号 | 包头仅 `WORD id + WORD payloadSize`，无 seq/timestamp/nonce | T2 成立 |
| 频率限制 | 无 | T5 成立 |

> **结论**：当前不是"协议被破"，而是**根本没有协议级身份校验**。再强的加密也救不了"UnitServer 不验 token"。方案必须**先补 AuthServer↔UnitServer 的 Token 链路**，再叠加密/签名/业务校验。

---

## 3. 总体方案（分层防御）

```
┌──────────────────────────────────────────────────────────────┐
│  L4 业务校验    越权 / 字段范围 / 状态机 / 距离 / CD          │ ← 永远必做
├──────────────────────────────────────────────────────────────┤
│  L3 频率/行为   令牌桶 + 异常计分                              │
├──────────────────────────────────────────────────────────────┤
│  L2 包级签名    HMAC + Seq + Timestamp (抗重放/抗篡改)         │
├──────────────────────────────────────────────────────────────┤
│  L1 传输加密    每会话随机 Key 的对称加密                       │
├──────────────────────────────────────────────────────────────┤
│  L0 会话 Token  Auth → Unit 一次性 Ticket + 双向校验            │ ← 最先做
└──────────────────────────────────────────────────────────────┘
```

> **优先级**：L0 → L4 → L2 → L3 → L1。L1 强加密成本最大、收益最小（只挡 sniff），L0/L4 才是决定能否被 WPE 利用的关键。

---

## 4. L0 会话 Token：AuthServer ↔ UnitServer 强绑定

### 4.1 现状问题
- `AuthServer` 给客户端的 `dwKey=46` 是**硬编码常量**（`AuthServer.cpp:140`）。
- `UnitServer::OnLoginCheckReq`（`Handlers/LoginHandler.cpp:7-24`）**完全没读 dwKey**，直接 ACK 成功。
- 任意 WPE 用户可绕过 AuthServer，直连 UnitServer 即建立"合法"会话。

### 4.2 设计

新增 **AuthCenter 共享会话表**（AuthServer 与 UnitServer 同进程，参考 `main.cpp`，无需 IPC）：

```cpp
struct AuthTicket {
    uint64_t   ticketId;          // 服务器随机生成 (CryptGenRandom)
    std::string account;
    DWORD      accountId;
    BYTE       channelId;
    BYTE       sessionKey[32];    // L1 + L2 用的会话密钥
    uint32_t   issuedAt;          // unix sec
    uint32_t   expiresAt;         // issuedAt + 30s
    bool       consumed;          // 一次性
    std::string clientIp;         // 绑定来源 IP
};

class AuthCenter {
public:
    static AuthCenter& Get();
    AuthTicket Issue(const std::string& account, DWORD accountId,
                     BYTE channelId, const std::string& ip);
    // UnitServer 调用：原子 consume，返回 sessionKey
    bool Consume(uint64_t ticketId, const std::string& ip,
                 BYTE channelId, AuthTicket& out);
private:
    std::mutex mu_;
    std::unordered_map<uint64_t, AuthTicket> tickets_;
};
```

### 4.3 协议变更

**AuthServer → Client `CS_IT_LOGIN_ACK`**（改 `AuthServer.cpp:138-144`）：
- `dwKey(4B)` → `ticketId(8B) + sessionKey(32B)`

**Client → UnitServer `CS_IT_LOGINCHECK_REQ`**（改 `XiahGame_Handler_Sender.cpp:35-51`）：
- `dwKey(4B)` → `ticketId(8B)`
- 同时附 channelID、protocolVer（已存在）

**UnitServer 处理**（改 `Handlers/LoginHandler.cpp:OnLoginCheckReq`）：
```cpp
AuthTicket t;
if (!AuthCenter::Get().Consume(ticketId, peerIp, channelId, t)) {
    LOG("[Auth] ticket invalid/expired/consumed/ip-mismatch -> kick");
    closesocket(s); return;
}
SessionMgr::Get().BindSession(s, t.account, t.accountId, t.sessionKey);
```

### 4.4 关键点
- **一次性**：`Consume` 必须原子置位 `consumed=true`，二次失败。
- **短时效**：30 秒内未消费即失效。
- **绑 IP**：禁止把 ticket 转给别的机器；移动网络可放宽到 /24。
- **绑 channelID**：禁止用 ChannelA 的 ticket 进 ChannelB。
- **后台清理**：定时器扫表清理过期 / 已消费项。

> 仅 L0 落地后，T1/T4 已堵死。T2/T3 仍能在合法会话内作弊，需 L2/L4 配合。

---

## 5. L1 传输加密（替换累加混淆）

### 5.1 现状
`ServerCore.cpp:101-130` 与 `XiahClient/NetMsg.cpp:263-296` 的算法是位置相关累加，且 `bKey` 全服固定 `0x42`。等同混淆明文。

### 5.2 设计
- 握手：UnitServer `accept` 后发的 5 字节 init 包改为返回**会话随机 nonce(16B)**；客户端用 `HKDF(sessionKey, nonce)` 派生 **每方向独立的对称密钥** `Kc2s`、`Ks2c`。
- 算法：**ChaCha20**（轻量、纯 C，单文件可引入）或 AES-128-CTR（如已用 CryptoAPI）。
- 计数器：每方向单调递增的 64-bit nonce（高位 = 包序号）。
- AuthServer 链路保留旧 `EncryptPacket`（仅承载登录前 1~2 包），UnitServer 链路一律新算法。

---

## 6. L2 包级签名（核心反 WPE 手段）

> 即便 L1 加密被破，L2 仍保证：**改包必报错、重放必报错**。

### 6.1 新包格式

```
+--------+--------+----------+----------+------------+----------+
| ID (2) | Size(2)| Seq (4)  | TS (4)   | Payload(N) | HMAC[8]  |
+--------+--------+----------+----------+------------+----------+
```

- `Seq`：客户端单调递增序号，从 1 开始。
- `TS`：相对会话起点的 ms 计数（避绝对时钟差）。
- `HMAC`：截断 8 字节 = `HMAC-SHA256(sessionKey, ID‖Size‖Seq‖TS‖Payload)`。

### 6.2 服务端校验（在 `RoutePacket` 之前）

```cpp
bool VerifyAndUnpack(SOCKET s, std::vector<BYTE>& pkt) {
    auto* sess = SessionMgr::Get().Find(s);
    if (!sess) return false;

    // 1. 长度健全
    if (pkt.size() < HEADER + SEQ + TS + HMAC_LEN) return false;

    // 2. HMAC
    BYTE expected[8];
    HmacSha256Trunc(sess->key, pkt.data(), pkt.size() - 8, expected, 8);
    if (memcmp(expected, pkt.data() + pkt.size() - 8, 8) != 0) {
        sess->badCount++; return false;
    }

    // 3. 序号窗口（滑动位图，宽 64，参考 RFC 6479）
    if (!sess->seqWindow.Accept(seq)) return false;

    // 4. 时间戳偏差（±10s）
    if (abs((int64_t)now_ms - (int64_t)ts) > 10000) return false;

    return true;
}
```

`SeqWindow` 用 64-bit 位图，O(1) 抗重放。

### 6.3 客户端
所有 `XiahNetwork::SendNetMsg` 走同一签名出口（改 `CMsg::Encrypt` 为 `CMsg::Sign`），禁止旁路。

```cpp
void CMsg::Sign(const SessionKey& k, uint32_t& seq, uint32_t tsMs) {
    AppendU32(seq++); AppendU32(tsMs);
    BYTE mac[8]; HmacSha256Trunc(k, m_pHead, GetSize(), mac, 8);
    AppendBytes(mac, 8);
}
```

### 6.4 收益对照
| 攻击 | 是否被堵 |
|---|---|
| WPE 修改任意字节 | ✅ HMAC 失败 |
| 抓包后重放 | ✅ Seq 在窗口外 / 已用过 |
| 改另一玩家的包重发 | ✅ HMAC 用对方 sessionKey，攻击者拿不到 |
| 伪造 charID 越权 | ❌ 仅 L2 不够，需 L4 |

---

## 7. L3 频率与行为风控

### 7.1 令牌桶（在 `PacketRouter` 入口）
按 `(socket, packetId)` 维护令牌桶：

| 包类别 | 速率 | 突发 |
|---|---|---|
| 移动 (`0x430B/D/F`) | 20/s | 10 |
| 战斗 (`0x4003/13/15`) | 8/s | 4 |
| 物品操作 (`0x420D/18/19/4203`) | 4/s | 4 |
| 商城/银行 | 2/s | 2 |
| 聊天 | 5/s | 5 |
| 心跳 (`0x442B`) | 1/3s ±0.5s | 1 |

超限：丢包并 `badCount++`；阈值（如 30）触发踢线。

### 7.2 心跳异常
当前 `0x442B` 客户端固定 3s 一次（`XiahSocket.cpp:248`），服务端忽略。建议：
- 服务端记录最近一次心跳时间；连续 15s 无心跳踢线。
- 心跳间隔过短（< 1s）累计 `badCount`。

### 7.3 异常聚合
`SessionMgr` 增加 `BadnessScore`：HMAC 失败 / 序号回放 / 字段越界 / 令牌桶超限 / handler 报错 都加分；阈值踢线 + 写审计日志。

---

## 8. L4 业务校验（永远不能省）

无论协议层多强，handler 必须把这套加上，否则合法客户端被改也作弊：

| Handler | 必须校验 |
|---|---|
| `OnItemMoveReq` (0x420D) | 源/目标 slot 属于本人；slot 范围合法；item 存在 |
| `OnUseItemReq` (0x4218/4219) | 物品归属本 charID；CD；状态（非死亡/眩晕）|
| `OnBuyItemReq` (0x3D30) | 距 NPC ≤ 阈值；NPC 在线；金币足；商品 ID 在该 NPC 列表 |
| `OnSellItemReq` (0x3D58) | 同上；物品归属本人 |
| `OnItemDropReq` (0x4203) | 物品归属；非绑定/非任务；地图允许丢弃 |
| `OnPickup` (0x4201/0x4232) | 距 drop ≤ 拾取半径；drop 未被领；归属规则 |
| `OnMoveReq` (0x430B/D/F) | Δt vs Δd ≤ 速度上限 × 1.2；地图可走点 |
| `OnAttackHit` (0x4005) | 距目标 ≤ 武功射程；目标 mapID 一致；目标存活；攻方未眩晕 |
| `OnMugongLearn` (0x4023) | 武功 ID 合法；前置满足；点数足 |
| `OnExecSp` (0x401D) | 加点不超剩余点；属性枚举合法 |
| `OnChatReq` | 长度 ≤ N；频道权限；脏词过滤 |
| **所有 handler** | **目标 charID == `SessionMgr::GetCharID(socket)`**（堵 T6 越权）|

> **强约定**：handler 入口的"操作者 charID"一律来自 `SessionMgr::GetCharID(s)`，**禁止从 payload 读**；payload 只能携带"目标 charID/objectID"，并对目标做权限判定。

---

## 9. 落地步骤（建议顺序）

### 阶段 A（1-2 天，最高 ROI）
1. **L0 Ticket**：`AuthCenter` + 改 `AuthServer.cpp:CS_IT_LOGIN_ACK` + 改 `LoginHandler.cpp:OnLoginCheckReq`。
2. **L4 越权修复**：扫所有 handler，把"操作者 charID"全部改成从 `SessionMgr` 取。

> 完成 A：WPE 直连 UnitServer 已无法启动会话；重放他人封包因 charID 不匹配而失败。绝大多数实战外挂被堵。

### 阶段 B（2-3 天）
3. **L2 HMAC + Seq + TS**：定义新包格式 + `VerifyAndUnpack` + 客户端 `CMsg::Sign`。
4. **L3 令牌桶 + BadnessScore**：在 `RoutePacket` 入口集中处理。

### 阶段 C（按需）
5. **L1 ChaCha20** 替换累加混淆。
6. **L4 深化业务校验**（速度、距离、CD、状态机）。

### 阶段 D（增强）
7. 客户端反作弊：进程模块校验、关键 send 函数 hook 检测、内存校验（仅辅助，决不能作为唯一防线）。
8. 服务端审计：每会话 `badCount/dropCount` 写日志库，离线找异常号。

---

## 10. 文件改动清单

| 文件 | 改动 |
|---|---|
| 新增 `Network/AuthCenter.h/.cpp` | Ticket 表 |
| 新增 `Network/PacketCrypto.h/.cpp` | ChaCha20 / HMAC-SHA256 / SeqWindow |
| `AuthServer.cpp:138-176` | `CS_IT_LOGIN_ACK` 改写 ticketId + sessionKey |
| `Handlers/LoginHandler.cpp:OnLoginCheckReq` | 调 `AuthCenter::Consume`，绑定 sessionKey |
| `Network/SessionMgr.h/.cpp` | 增加 `sessionKey / seqWindow / badCount / lastHeartbeat / sessionStartMs` |
| `Network/PacketRouter.cpp:RoutePacket` | 入口集成 `VerifyAndUnpack` + 令牌桶 |
| `UnitServer.cpp:266-355` | 替换 `DecryptPacket` 为新管线；init 包改为发送 nonce |
| `XiahClient/XiahSocket.cpp:Send/ProcessMessage` | 用新签名/解密；保存 sessionKey + sessionStartMs |
| `XiahClient/NetMsg.cpp:Encrypt/Decrypt` | 替换为 ChaCha20 + HMAC |
| `XiahClient/AppData.h:m_dwKey` | 升为 `uint64_t m_ticketId` + `BYTE m_sessionKey[32]` |
| `XiahClient/XiahGame_Handler_Sender.cpp:SendCS_IT_LOGIN_REQ` | 发 ticketId 而非 dwKey |
| `XiahClient/XiahGame_Handler_IT_Rcv.cpp:OnCS_IT_LOGIN_ACK` | 解析新字段 |
| 全部 handler | 操作者 charID 改自 SessionMgr 取，并加业务校验 |

---

## 11. 协议向后兼容
- `PROTOCOL_VERSION` 升一档；老客户端连入立即返回 `CS_IT_LOGINCHECK_ACK = ProtocolMismatch` 并断开。
- AuthServer 与 UnitServer **同时升级**，避免半升级期 ticket 不识别。

---

## 12. 验证方法

1. **单元**
   - `AuthCenter::Consume` 一次成功、二次失败、跨 IP 失败、过期失败。
   - `SeqWindow::Accept` 边界：seq=0、回放、超前 65、超前 1<<32。
   - HMAC：篡改 1 bit 必失败。
2. **集成**
   - 用 WPE 抓正常移动包，原样发 5 次：仅第 1 次成功，2-5 被序号窗口拒。
   - 改 payload 一字节再发：HMAC 失败。
   - 不经 AuthServer 直连 UnitServer 发 `CS_IT_LOGINCHECK_REQ`：被踢。
   - A 号 socket 发 B 号 charID 的 `OnUseItemReq`：被 L4 拒。
3. **压测**
   - 1k 客户端，每秒 50 包：CPU/内存基线对比；HMAC 应 < 5% CPU。
4. **审计**
   - `BadnessScore > 阈值` 自动写日志 + 可选 ban。

---

## 13. 风险与权衡

| 风险 | 缓解 |
|---|---|
| 客户端被逆向，sessionKey 暴露 → 攻击者仍能合法签名 | sessionKey 一会话一变；服务端 L3/L4 风控；考虑 client 反调试 |
| HMAC 增加每包 8B + 计算 | 截断 8B 抗碰撞 2^64；ChaCha20+HMAC 单包 < 1µs |
| 移动端弱网时间戳偏移 | 窗口 ±10s + 仅相对会话时钟 |
| 老封包加密被外挂依赖 | 完全替换，不保留旁路 |

---

## 14. 一句话总结

**先做 L0（一次性 Ticket）+ L4（操作者 charID 强绑 SessionMgr） → 再做 L2（HMAC + Seq + TS） → 视情况补 L1/L3。**

这个顺序在工时最少时把 WPE 注入路径彻底切断。L1 强加密是锦上添花，L4 业务校验是保命底线。

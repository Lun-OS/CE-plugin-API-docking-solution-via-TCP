# TCP Cheat Engine 网络插件（TCP + CEQP）

为 Cheat Engine (CE) 提供基于 Winsock 的 TCP 收发能力，并内置 CEQP 控制协议服务端，支持同步与异步两种调用模式。异步模式通过后台接收线程与非阻塞队列避免 UI 卡顿；结合 20ms 轮询可实现端到端低延迟（取决于网络与对端）。

---

## 目录
- 简介
- 编译与安装
- Lua API
  - CEQP 控制协议服务端
  - TCP 同步 API
  - TCP 异步与非阻塞 API
- 简单调用示例
  - TCP：HTTP GET 测试
  - 异步接收 + 20ms 轮询
- 常见问题
- 文件结构
- 关键变更记录

---

## 简介
- 基于 Winsock 的 CE 插件，为 Lua 提供 TCP 网络能力，并内置 CEQP（控制程序协议）服务端。
- 支持同步与异步调用：
  - 同步：快速测试用，等待期间可能阻塞 UI。
  - 异步：后台线程接收，Lua 端非阻塞弹队列，避免 UI 卡顿。
- 延迟优化：
  - TCP 连接成功后可禁用 Nagle（TCP_NODELAY），降低小包延迟（Lua 可动态切换）。

## 编译与安装
- 环境：Windows + Visual Studio（建议 VS2022），与 CE 位数一致（x64 或 x86）。
- 已配置的关键项：
  - 链接库：`Ws2_32.lib`、`lua53-64.lib`（x64）或 `lua53-32.lib`（x86）。
  - 导出定义：`TCP_UDP.def`（导出 CE 插件入口：`CEPlugin_GetVersion`、`CEPlugin_InitializePlugin`、`CEPlugin_DisablePlugin`）。
- 步骤：
  1. 打开解决方案，选择与 CE 一致的配置（如 `Debug|x64`）。
  2. 生成 `TCP_UDP.dll`。
  3. 将 DLL 复制到 CE 的 `plugins` 目录，或使用 CE 的“插件管理器”加载。

## Lua API

### CEQP 控制协议服务端
- `QAQ(port?: int=9178) -> boolean`
  - 启动 TCP 服务端并监听指定端口（默认 9178）。
  - 控制程序通过 `CETCP.h` 定义的 CEQP 协议进行请求/响应通信。
- `stopQAQ() -> boolean`
  - 停止服务端、关闭监听与会话线程。

协议简述：
- 帧头（16 字节，1 字节对齐）：`magic[4]='CEQP'`, `version=0x01`, `type`, `flags`, `reserved`, `request_id(u32)`, `payload_len(u32)`。
- 负载为 TLV（小端）：`T(u16) L(u16) V(L)`；常用类型：`ADDR(u64)`, `LEN(u32)`, `MODNAME(string)`, `OFFSET(s64)`, `OFFSETS(s64[])`, `DATA(bytes)`。
- 支持消息类型：心跳、按地址读写、模块+偏移读写、指针链读取、模块基址查询；错误回复 `ERROR_RESP` 包含 `ERRCODE(u32)` 与 `ERRMSG(string)`。

调用示例（Lua）：
```lua
-- 启动服务端在 9178 端口
QAQ(9178)
-- ... 控制程序从外部连接并发起读写操作 ...
-- 关闭服务端
stopQAQ()
```

### TCP 同步 API
- `pluginTCPConnect(host: string, port: int) -> boolean`
- `pluginTCPSend(data: string) -> int`（返回发送字节数）
- `pluginTCPRecv(maxlen?: int=4096, timeoutMs?: int=1000) -> string`（收到的数据，可能为空）
- `pluginTCPClose() -> nil`

### TCP 异步与非阻塞 API
- `pluginTCPStartRecv() -> boolean`（启动后台接收线程）
- `pluginTCPStopRecv() -> boolean`（停止后台接收线程）
- `pluginTCPRecvNonblocking(maxlen?: int=4096) -> string`（从队列弹出一条数据，不阻塞）
- `pluginTCPSetNoDelay(enable: boolean) -> boolean`（启/禁 TCP_NODELAY，减少小包延迟）

> 线程安全说明：后台接收线程只操作 Winsock 与内部队列，不直接调用 Lua；Lua 侧通过非阻塞 API 从队列弹出数据，避免跨线程 Lua 访问导致的风险。

## 简单调用示例

### TCP：HTTP GET 测试
```lua
local host = "example.com"
local port = 80
local req = "GET / HTTP/1.1\r\nHost: example.com\r\nConnection: close\r\n\r\n"

if pluginTCPConnect(host, port) then
  pluginTCPSetNoDelay(true) -- 低延迟
  local sent = pluginTCPSend(req)
  print("sent", sent)
  local data = pluginTCPRecv(8192, 5000)
  print("recv len", #data)
  print(data)
  pluginTCPClose()
end
```

### 异步接收 + 20ms 轮询
```lua
pluginTCPStartRecv()
pluginTCPSetNoDelay(true)

local t = createTimer(nil, false)
t.Interval = 20
t.OnTimer = function()
  local tcpData = pluginTCPRecvNonblocking(8192)
  if #tcpData > 0 then print("TCP nb recv", #tcpData) end
end
t.Enabled = true
-- 停止时：t.destroy(); pluginTCPStopRecv();
```

## 常见问题
- 无回包：公网地址可能无服务或不回显；请使用 HTTP 示例或本机/局域网回显服务验证。
- 超时：提高 `timeoutMs`（如 5000–10000ms），并优先使用异步与定时器轮询，避免在 UI 线程阻塞。
- 防火墙/NAT：可能拦截回包，建议本机或局域网先验证功能。

## 文件结构
```
TCP_UDP/
├── README.md
├── TCP_UDP.def
├── TCP_UDP.vcxproj*
├── cepluginsdk.h
├── tcp_udp_plugin.cpp
├── test_ce_plugin.lua
└── x64/Debug|Release (编译产物)
```

## 关键变更记录
- 移除 UDP 相关实现与 Lua API，保留 TCP 与 CEQP。
- 新增异步接收线程与非阻塞 Lua API（TCP），避免 UI 卡顿并提升响应。
- 可通过 `pluginTCPSetNoDelay(true)` 降低小包延迟。


# 作者：Lun.  QQ:1596534228   github:Lun-OS
# CEQP 项目总览（Cheat Engine 插件 + ImGui 控制程序）

本项目由两部分组成：
- 插件（CE 端）：TCP 网络能力 + CEQP 协议服务端，供外部控制程序通过 TCP 访问 CE 的读写能力。
- 控制程序（PC 端）：基于 Win32/DX11/Dear ImGui 的图形客户端，通过 CEQP 协议进行连接、内存读写、模块操作与指针链解析。

相关文档：
- 插件说明：`plugin/README.md`
- 控制程序说明：`cpp_docking_example/README.md`

---

## 目录结构
```
我好懒啊，我不想写注释qwq
```

---

## 快速开始
1. 编译
    -略
2. 启动 CEQP 服务端：
   - 在 CE 的 Lua 窗口执行 `plugin/TCP_UDP/run.lua`（默认 `QAQ(9178)` 监听）。
3. 编译并启动控制程序（PC 端）：
   - 打开 `imgui控制程序/imgui-test.sln`，选择 `Release|x64`，生成并运行。
4. 建立连接：
   - 在控制程序的连接面板输入 Host（如 `127.0.0.1`）与 Port（默认为 `9178`），点击 Connect；可用 Test Connection 进行 Ping。
5. 使用功能：
   - Memory：按地址读/写；Module：获取基址与模块+偏移读/写；Pointer Chain：按偏移链读取；Log：查看日志与心跳状态。

---

## CEQP 协议简述
- 帧头（16 字节，1 字节对齐）：`magic='CEQP'`, `version=0x01`, `type`, `flags`, `reserved`, `request_id(u32)`, `payload_len(u32)`。
- 负载采用 TLV（小端）：`T(u16) L(u16) V(L)`；常用类型：`ADDR(u64)`, `LEN(u32)`, `MODNAME(string)`, `OFFSET(s64)`, `OFFSETS(s64[])`, `DATA(bytes)`，错误回复含 `ERRCODE(u32)` 与 `ERRMSG(string)`。
- 支持消息：心跳、按地址读写、模块+偏移读写、指针链读取、模块基址查询；单次负载最大 1MB。

---

## 构建要求与建议
- 插件：Windows + Visual Studio（建议 VS2022），链接 `Ws2_32.lib` 与 Lua 53（随工程提供）；确保与 CE 位数一致（x64/x86）。
- 控制程序：Windows 10/11 + VS2019/VS2022，使用 Windows SDK 与 d3d11；ImGui 源码已内置。
- 网络：本机或局域网优先，注意防火墙放行端口 9178。

## 常见问题
- 无法连接：服务端未启动或端口被拦截；在 CE 端确认 `QAQ(9178)` 已运行，检查防火墙与端口占用。
- 心跳失败：CE 端服务停掉或插件异常；可在 CE 端执行 `stopQAQ()` 后重启，再在控制程序中重新连接。
- 位数差异：插件需与 CE 位数一致；控制程序位数可独立（通过 TCP 通信）。
- 覆盖层交互：控制程序为覆盖层窗口，默认跟随 `windows-test.exe`，可在 `main.cpp` 修改目标进程名；必要时调整覆盖层交互策略。

---

## 许可与鸣谢
- 作者：Lun（QQ: 1596534228，GitHub: Lun-OS）。
- 仅供学习与交流使用；请遵循相关软件许可与法律法规。

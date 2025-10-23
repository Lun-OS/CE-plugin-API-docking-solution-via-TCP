# CEQP 控制程序（ImGui 客户端）

基于 Win32 + DirectX 11 + Dear ImGui 的图形化控制程序，用于通过 CEQP 协议与 Cheat Engine 插件通信，实现内存读写、模块操作、指针链读取等功能。程序以覆盖层（Layered TopMost 窗口）方式工作，可跟随目标进程窗口移动。

---

## 功能概览
- 连接管理：Connect / Disconnect、Ping 测试，内置 2 秒心跳保活（失败自动断开并记录日志）。
- 内存读写：按地址读取/写入，支持十进制与十六进制输入；写入支持多种类型（u8/u16/u32/u64、i8/i16/i32/i64、float/double、或十六进制字节序列）。最大负载 1MB。
- 模块操作：查询模块基址（如 kernel32.dll），按模块+偏移读取/写入，偏移支持负数与 0x 前缀。
- 指针链读取：基址可为十六进制地址或“模块名+偏移”；偏移链每项可独立选择十进制或十六进制，最终按长度读取数据。
- 数据解析展示：对最近一次读取的数据，按 Int8/UInt8、Int32/UInt32/Float、Int64/UInt64/Double 多种格式解析预览。
- 日志面板：时间戳 + 颜色区分的信息/成功/错误日志，支持清空与自动滚动。

---

## 构建环境
- Windows 10/11，Visual Studio 2019/2022（建议 VS2022）。
- Windows SDK（含 d3d11），无需额外依赖，ImGui 源码已包含于 `imgui控制程序/ImGui/`。

## 编译步骤
1. 打开 `imgui控制程序/imgui-test.sln`。
2. 选择合适的配置（如 `Release|x64`）。
3. 生成并运行（可直接调试启动）。

> 注意：控制程序与 CE 插件位数可独立（通过 TCP 通信），但 CE 自身需加载与其位数匹配的插件 DLL（见插件说明）。

## 运行步骤（对接 CE 插件）
1. 在 CE 中加载并运行插件服务器：参考 `plugin/TCP_UDP/README.md`，或在 CE 的 Lua 窗口执行 `plugin/TCP_UDP/run.lua`（默认 `QAQ(9178)` 启动）。
2. 启动本控制程序，连接面板中填写 Host（如 `127.0.0.1`）和 Port（默认 `9178`），点击 Connect。
3. 连接成功后，可切换各标签页执行读取/写入等操作；必要时可先点击 “Test Connection” 进行 Ping 验证。

---

## 界面与操作说明
- Memory 标签页：
  - Address 输入支持 `0x` 前缀的十六进制或十进制；Length 同理。
  - Read Memory 读取后会把十六进制结果填入下方 Value 文本框；Write Memory 可选择写入类型或直接写入十六进制字节序列。
  - 十六进制字节序列需为偶数长度且仅含 0-9A-F（忽略空白）。
- Module 标签页：
  - 填写模块名（如 `kernel32.dll`），点击 Get Module Base 获取基址（右侧显示）。
  - Read/Write Module+Offset：输入 Offset（支持负数与 `0x` 前缀），Length 或 Value 即可读取/写入。
- Pointer Chain 标签页：
  - Base Address 支持纯地址或 `模块名+偏移`（如 `kernel32.dll+0x1234`）。
  - Offset Chain：通过滑块设置数量，每项可选 Decimal 或 Hexadecimal，并分别输入偏移。
  - 点击 Read Pointer Chain 后，于共享数据区显示读取结果与解析。
- Log 标签页：
  - 清空日志、打开/关闭自动滚动；所有操作的提示、错误与心跳状态均在此记录。
- 共享数据区：
  - 在非 Log 标签页时显示，展示最近一次读取的数据，并按常见数值类型解析预览。

---

## 覆盖层与窗口跟随
- 程序创建 TopMost 的 Layered 窗口，使用黑色作为颜色键透明；默认尝试跟随名为 `windows-test.exe` 的进程主窗口位置。
- 若未找到目标窗口，则窗口全屏显示作为覆盖层。
- 修改目标进程：在 `main.cpp` 主循环附近，将 `FindProcessId(L"windows-test.exe")` 改为实际进程名（例如游戏或应用的可执行文件名）。
- 点击穿透：当前覆盖层可交互（未启用点击穿透）。若需要点击穿透，可考虑添加 `WS_EX_TRANSPARENT` 或命中测试调整（需自行扩展）。

---

## 输入与注意事项
- 长度与负载限制：单次读取/写入负载最大 1MB。
- 地址/偏移解析：支持十进制与十六进制（可带 `0x` 前缀），偏移支持负数。
- 写入类型：选择 Decimal/Hexadecimal 影响数值解析基数；float/double 按 IEEE754 小端编码写入。
- 心跳：连接后每 2 秒自动 Ping，一旦失败会断开并提示错误。

## 常见问题
- 连接失败：确认 CE 插件服务已启动、端口开放、防火墙未拦截；Host/Port 填写正确。
- 无响应或心跳失败：检查 CE 端 `stopQAQ()` 是否被调用、插件是否仍在运行；必要时重启服务端并重新连接。
- 覆盖层遮挡：若覆盖层覆盖目标窗口导致操作不便，可缩小窗口或修改跟随逻辑。

## 目录结构（控制程序）
```
imgui控制程序/
├── ImGui/            (Dear ImGui 源码与 Win32/DX11 后端)
├── CETCP.h/.cpp      (CEQP 客户端实现，TLV 编码/解码)
├── main.cpp          (Win32 + DX11 初始化、覆盖层窗口、ImGui UI)
├── MyImGui.*         (自定义 ImGui 辅助)
├── imgui-test.sln    (解决方案)
└── 其他示例/配置文件
```

## 版权与作者
- 作者：Lun（QQ: 1596534228，GitHub: Lun-OS）
- 本仓库仅供学习与交流使用，请遵循相关软件许可与法律法规。

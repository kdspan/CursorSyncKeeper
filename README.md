# CursorSyncKeeper（光标同步守护者）

轻量级 Windows 工具：监听**系统底层显示与光标状态变化事件**，在以下场景把系统**强制保持在「软件鼠标」状态**，无需重启电脑：

- **多显卡分别输出不同显示器、且部分显示器被旋转（竖屏）时**，硬件光标的绘制坐标系与图像显示坐标系分离，光标被画在与实际位置不符的地方；
- 全屏 / 无边框游戏运行或退出，把光标从软件路径切回硬件路径（副屏光标丢失、跨屏异常）；
- 分辨率切换 / 显示器热插拔 / 扩展坞视频输出变动；
- 睡眠唤醒后显示驱动重置，软件鼠标不再恢复；
- 移动硬盘、U 盘、键鼠等无关 USB 插拔**不触发**（见下文「防误触」）。

核心目标：**让光标始终绘制在正确的位置**——尤其解决「多 GPU 分别接屏 + 旋转副屏」下硬件光标坐标与图像坐标错位这一根本问题；并顺带保证游戏运行时副屏也可正常用软件光标。

## 问题根因

### 直接症状：多显卡 + 旋转屏下硬件光标错位

当系统中**多个 GPU 各自独立输出到不同显示器**（例如一张卡接主屏、另一张卡接竖屏副屏）时，Windows 默认使用 **硬件鼠标光标**（GPU 的 Multiplane Overlay，MPO 平面）绘制指针。硬件光标由**各个显示控制器（GPU）在自己的叠加平面上直接合成**，并不经过 DWM 的桌面坐标系：

- 当某块显示器被**旋转**（竖屏 / 朝左 / 朝右）时，该 GPU 的**硬件光标扫描坐标系**与 DWM 用来摆放图像/窗口的**桌面显示坐标系**发生分离——旋转需要的坐标变换只作用在「图像平面」上，而硬件光标平面往往不被一致地变换；
- 结果是：硬件光标被绘制在**与实际光标逻辑位置不符**的地方（竖屏上光标偏移、跨屏移动时跳变、卡在屏幕边界等），而软件光标与图像始终共享同一坐标系，位置永远准确。
- 这一错位在「多 GPU 分别接屏 + 至少有一块旋转屏」的组合下尤其稳定复现，是硬件光标路径的固有缺陷，与具体游戏/应用无关。

### 诱因：软件光标被切回硬件路径后不会自动恢复

即使系统此前处于软件光标路径，以下情形会把它切回硬件光标，且之后**不会自动恢复**：

- 全屏 / 无边框游戏进入或退出（游戏会强制走硬件光标路径）；
- 分辨率切换 / 显示器热插拔 / 扩展坞视频输出变动；
- 睡眠唤醒后显示驱动重置；
- 某些驱动或应用通过 `SystemParametersInfo(SPI_SETMOUSETRAILS, 0)` 把 `MouseTrails` 改回硬件路径。

> 仅改注册表把鼠标设为软件并**不够**：注册表只是持久值，必须让驱动 / 系统真正重新评估光标合成路径才会生效——过去这意味着重启。本工具通过两条互补机制在线完成这件事。

## 两条核心机制（如何真正锁定软件光标）

### 1. 关闭 MPO 叠加层（驱动级，`HKLM`）

写入 `HKLM\SOFTWARE\Microsoft\Windows\DWM\OverlayTestMode = 5`，禁用 Multiplane Overlay，逼 DWM 把光标合成进主平面（软件光标）。此键位于 `HKLM`，**需管理员权限**。它从驱动层面保证了「无硬件光标平面可用」。

> 局限：当游戏以**真·独占全屏（exclusive fullscreen）**运行时，DWM 被该显示器绕过，此机制对游戏主屏无效（Windows 设计如此）。但副屏仍由 DWM 合成，因此同样受益。无边框 / 窗口化游戏 DWM 始终在线，此机制完全生效。

### 2. `MouseTrails = -1` 哨兵（运行时级，`HKCU`）

向系统写入 `HKCU\Control Panel\Desktop\MouseTrails = "-1"`（`-1` 是「启用软件光标路径、且不显示任何可见拖尾」的哨兵值）。原理：硬件光标平面无法绘制拖尾，于是当 `MouseTrails` 为负时，OS 被迫退回软件光标路径。

这是**真正能击败游戏反复改写**的运行时杠杆——比单纯依赖 MPO 更稳，因为：

- 它是 `SystemParametersInfo(SPI_SETMOUSETRAILS)` 的运行时值，重设成本低（一次系统调用 + 重载光标方案），**无黑闪**；
- 它能在游戏**运行中**持续保活，使副屏光标始终走软件路径；
- 持久值写为字符串 `"-1"`（不用 `SPIF_UPDATEINIFILE`，否则会被存成无符号十进制 `4294967295`）。

两条机制互补：MPO 关闭是「地基」，哨兵是「运行时守门员」。

## 三检测链（何时触发修复）

| 链路 | 触发源 | 动作 | 是否黑闪 |
|---|---|---|---|
| **拓扑链** | `WM_DISPLAYCHANGE` / `WM_DEVICECHANGE`（仅显示类设备）/ `WM_POWERBROADCAST`（唤醒） | 延迟 500ms 平复抖动后比对完整拓扑快照，若真有变化调用 `Apply()`（含显卡驱动重置） | 是（真实拓扑变化，可接受） |
| **哨兵链·事件** | `WM_SETTINGCHANGE`（游戏用 `SPIF_SENDCHANGE` 改写鼠标设置）/ `SetWinEventHook(EVENT_SYSTEM_FOREGROUND)`（**仅当全屏 / 无边框游戏**获得前台时） | 延迟 1.2s 后调用 `VerifyCursorSentinel()` | 否 |
| **哨兵链·看门狗** | 周期定时器（桌面 15s / 游戏前台 3s） | 同上 | 否 |

**哨兵链**专门覆盖「无边框 / 窗口化游戏」——它们**不改分辨率、不插拔设备**，拓扑链永远不触发；但只要哨兵被改回 `0`（硬件路径），看门狗或前台切换事件会立刻重新拉回 `-1`。

`VerifyCursorSentinel()` 逻辑：

1. `EnsureOverlayTestModeAlive()`：检查 `OverlayTestMode` 是否仍为 5，仅当它漂移时重写——**非闪烁保活**，不死循环、不重置驱动；
2. 若 `MouseTrails == -1`（哨兵完好），直接返回（绝大多数 tick 到此为止，零开销）；
3. 否则 `ReassertSoftwareCursor()`（重设 `-1` + 重载光标方案），**不重置驱动、零黑闪**。

## 设计要点

- **事件驱动为主，廉价轮询兜底**：拓扑链纯事件驱动（CPU≈0）；哨兵链事件 + 看门狗兜底，看门狗在桌面仅 15s 一次。
- **自适应看门狗**：`IsLikelyFullscreenForegroundWindow()` 用几何启发式判定前台是否为无边框 / 全屏游戏（覆盖整块主屏且无标题栏 / 边框的 `WS_POPUP` 窗口）。`SHQueryUserNotificationState` **不会**标记无边框游戏，故用此启发式；命中后看门狗提速到 3s，使顽固游戏最多 3s 内被夺回软件光标。
- **前台钩子仅在游戏时触发修复**：`EVENT_SYSTEM_FOREGROUND` 钩子**只在** `IsLikelyFullscreenForegroundWindow()` 为真（全屏 / 无边框游戏获得前台）时才调度哨兵校验。普通窗口的前台切换（如 Telegram、浏览器、资源管理器在登录时启动）**从不改写 `MouseTrails`**，因此不再触发"修复"——避免守护进程对从未损坏的光标反复做无意义操作（看门狗仍会兜底任何真实的漂移）。
- **精准触发显示类设备**：`RegisterDeviceNotification` 只订阅 `GUID_DEVINTERFACE_MONITOR` / `GUID_DEVINTERFACE_DISPLAY_ADAPTER`，USB-C 显示器 / 扩展坞 / USB 显卡插拔才触发；U 盘 / 移动硬盘 / 键鼠不触发。
- **延时判定防抖动误触**：拓扑事件只（重）置 500ms 定时器，抖动平复后再比对**完整拓扑快照**（设备名 + 坐标矩形 + 主屏标志）。游戏退出改分辨率（同设备名、不同几何）会被识别；移动硬盘引发的 GPU 瞬时抖动因快照已恢复而被静默丢弃——不闪、不重复。
- **单实例互斥锁**：`WinMain` 中 `Global\CursorSyncKeeperDaemon` 互斥锁，重复启动静默退出，避免多个守护进程叠加驱动重置造成反复黑闪。
- **自触发抑制**：`Apply()` 自身的 `WM_DISPLAYCHANGE` 由冷却窗口切断「修复 → 触发 → 再修复」自激循环。
- **关机前持久化**：守护进程处理 `WM_QUERYENDSESSION` / `WM_ENDSESSION`，在系统保存每用户设置（HKCU）前最后重设一次 `MouseTrails=-1`（无驱动重置、无黑闪）。否则若关机前一刻 `MouseTrails` 被某进程改回 `0`（或守护进程当时未运行），Windows 会把 `0` 持久化，**下次开机鼠标即落在硬件路径**——该处理保证被保存的是软件光标哨兵值。
- **开机自启（管理员）**：通过**登录触发的计划任务**（`LogonTrigger` + 交互令牌 + 最高权限）注册守护进程，登录即以管理员、在交互会话中运行，可随时重写 `HKLM`。详见下文「计划任务的功能与必要性」。

## 开机自启：计划任务的功能与必要性

守护进程必须在**每次登录 / 重启后自动运行**，否则系统一旦切回硬件光标（驱动重置、游戏退出、睡眠唤醒）就无人修复——这正是本工具存在的意义。因此"开机自启"不是可选项，而是核心功能。

**为什么是计划任务（Task Scheduler），而不是其它常见自启方式：**

- **Run / RunOnce 注册表键**：只能以**当前用户、非提权**启动。本守护进程需要管理员权限（写 `HKLM\...\DWM\OverlayTestMode` 并持续保活 MPO），非提权启动会让这部分静默失败。计划任务可用 `RunLevel=HighestAvailable` 在登录时**无 UAC 弹窗地提权**。
- **Windows 服务（Session 0）**：服务运行在 Session 0，**无法接收桌面广播消息**（`WM_DISPLAYCHANGE`、前台切换钩子），也就无法感知显示变化去修复光标。守护进程必须活在**交互会话**里。
- **启动文件夹快捷方式**：同样非提权、且依赖 explorer 加载，可靠性不如计划任务。

因此唯一同时满足"**提权 + 无 UAC + 交互会话**"三条件的内置机制就是**登录触发的计划任务**（`LogonTrigger` + `InteractiveToken` 主体 + `HighestAvailable` 运行级别，且 `RunOnlyIfLoggedOn` 默认 `true` 以保证留在交互会话）。

**实现可靠性**：计划任务通过**导入 XML 定义**（`schtasks /create /xml`）创建，而非易错的 `schtasks /create /sc onlogon /rl highest` 命令行——后者因引号 / 主体解析问题，常出现"任务已创建却从不启动守护进程"的失效情况。XML 方式显式锁定上述全部设置（并遵循 Task Scheduler 1.2 schema 的严格元素顺序），在导入失败时回退到命令行方式，最大化可靠性。

## 构建

需要 MSVC（已验证 Visual Studio 2022 Community）+ CMake + Ninja。

```bat
:: 在 "Developer Command Prompt for VS" 或初始化 vcvarsall 后执行
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

产物：
- `build\CursorSyncKeeper.exe` — 守护进程（无外部依赖）
- `build\CursorSyncKeeperPanel.exe` — 控制面板
- `dist\CursorSyncKeeper_Setup.exe` — **自包含安装向导（最终分发物）**

> 打包完全由 CMake 闭环完成，**无需任何外部打包脚本**：`Setup.rc` 对两个内嵌 exe 声明了 `OBJECT_DEPENDS` 显式依赖，守护进程/面板任一重编译，安装包都会自动重新嵌入并重新链接，再自动发布到 `dist\`，保证安装包内永远是最新二进制。

## 安装包（安装向导，需管理员）

`CursorSyncKeeper_Setup.exe` 是一个**安装向导**：欢迎 → 选择安装位置 → 确认 → 完成。可执行**安装**（首次）或**重装**（已存在时刷新并重新注册）。

它是**单文件自包含**的安装程序：守护进程 `CursorSyncKeeper.exe` 与控制面板 `CursorSyncKeeperPanel.exe` 已作为二进制资源内嵌其中，安装时自解压到目标目录，因此**只需分发这一个 exe 即可**。

- **安装 / 重装**：复制文件到所选目录（默认 `Program Files\CursorSyncKeeper`，可自定义），**写入 `HKLM` 禁用 MPO**，**注册登录自启计划任务**，启动守护进程，并**立即执行一次完整修复**（约 1 秒屏幕黑闪，属正常）。
- 安装后的软件由两个独立程序组成：
  - **守护进程** `CursorSyncKeeper.exe`：精简的纯事件驱动监控器，只负责运行时修复与监听显示 / 光标状态事件；**不包含**任何安装 / 卸载 / 写注册表 / 注册计划任务的代码（这些由下方程序负责）。
  - **控制面板** `CursorSyncKeeperPanel.exe`：用于**单次修复**或**卸载**（卸载不执行修复，不黑屏）。

> 写 `HKLM` / 注册计划任务 / 删除 `Program Files` 这类提权操作集中在安装向导与控制面板共用的 `AdminOps` 模块中；守护进程本身不链接该模块，保持轻量。

> 安装向导自带 `requireAdministrator` 清单，始终以管理员运行，故安装过程不会重复弹 UAC。

## 命令行

守护进程（`CursorSyncKeeper.exe`）：

- 不带参数运行即为后台事件驱动的修复监控器（由安装向导或登录计划任务以管理员权限启动），本身**不提供** `/install`、`/uninstall`、`/fix` 等命令。

安装向导（`CursorSyncKeeper_Setup.exe`，需管理员）：

```bat
CursorSyncKeeper_Setup.exe           :: 打开安装向导（安装 / 重装，可选位置）
```

控制面板（`CursorSyncKeeperPanel.exe`，需管理员）：

```bat
CursorSyncKeeperPanel.exe           :: 打开控制面板（单次修复 / 卸载）
CursorSyncKeeperPanel.exe /uninstall :: 静默卸载（供“程序和功能”调用）
```

> 写 `HKLM` / `Program Files` 需管理员权限。非管理员运行时程序会自动以 `runas` 重新启动并弹出 UAC 提示。

## 控制面板与卸载

- **单次修复**：立即执行一次软件鼠标修复（约 1 秒屏幕黑闪，属正常）。
- **卸载**：停止守护进程、移除登录自启计划任务、移除 `HKLM` 注册表项（`OverlayTestMode`）、**还原 `MouseTrails` 为 `0`**、删除程序文件与日志。卸载过程不执行修复，因此不会出现屏幕黑闪。
- “程序和功能”中的 `CursorSyncKeeper` 项会启动控制面板（或静默 `/uninstall`）。

## 验证

安装后：
- `HKLM\SOFTWARE\Microsoft\Windows\DWM\OverlayTestMode` == 5
- `HKCU\Control Panel\Desktop\MouseTrails` == "-1"
- 计划任务 `CursorSyncKeeper` 存在于 Task Scheduler，登录触发、最高权限
- 进程 `CursorSyncKeeper.exe` 在后台常驻（事件监听，CPU≈0%）

游戏场景验证：
- 以**无边框 / 窗口模式**运行游戏，进游戏后 `MouseTrails` 应稳定为 `-1`，副屏鼠标正常可用；
- 退出 / Alt+Tab 切出后约 1~3 秒内光标恢复，全程无黑闪。

多显卡 + 旋转屏场景验证：
- 在「一张显卡接主屏、另一张显卡接竖屏副屏」的布局下，安装前竖屏上的硬件光标会出现偏移 / 跳变；安装并修复后，光标与图像共享同一坐标系，竖屏上光标位置应准确跟随。
- 旋转副屏的分辨率 / 方向变更、或拔插该屏后，守护进程会在拓扑链触发时重新应用修复（约 1 秒黑闪属正常），光标坐标关系不被破坏。

## 为什么不需要轮询式服务？

显示模式 / 驱动变化是确定事件（广播消息），监听这些消息比周期扫描更高效、更省电。哨兵链仅在桌面以 15s 低频兜底，游戏前台提速到 3s——仍属廉价系统调用，对正常使用无任何可感知开销。

## 文件与存储位置（符合 Windows 规范）

- 程序文件：`C:\Program Files\CursorSyncKeeper\`（二进制 + 自带安装包）
- 运行日志：`C:\ProgramData\CursorSyncKeeper\install.log`（安装/修复/卸载操作记录）
- 系统修复键值：`HKLM\SOFTWARE\Microsoft\Windows\DWM\OverlayTestMode = 5`
- 用户光标哨兵：`HKCU\Control Panel\Desktop\MouseTrails = "-1"`
- 自启方式：计划任务 `CursorSyncKeeper`（`onlogon` + 最高权限）
- 卸载入口：控制面板“程序和功能”中的 `CursorSyncKeeper` 项

> 安装包自带 `requireAdministrator` 清单并会在未提权时自动以 UAC 重新启动，因此写入 Program Files 与 HKLM 不会因权限不足而静默失败。

## 故障排查

若安装后光标仍异常：
1. 查看 `C:\ProgramData\CursorSyncKeeper\install.log` 中是否有 `CopyFiles / InstallScheduledTask / WriteARP / StartMenu` 失败记录。
2. 确认 `HKLM\SOFTWARE\Microsoft\Windows\DWM\OverlayTestMode == 5` 且 `HKCU\Control Panel\Desktop\MouseTrails == "-1"`。
3. 以管理员运行 `CursorSyncKeeper_Setup.exe` 点击「立即修复」手动触发一次。
4. 游戏**独占全屏**时主屏光标由游戏 / 驱动控制（DWM 被绕过），本工具对此主屏无能为力；改用**无边框 / 窗口模式**即可让副屏与游戏内光标均走软件路径。
5. 修复会触发约 1 秒屏幕黑闪（显卡驱动重置），属正常现象；游戏运行中的哨兵保活**不**黑闪。

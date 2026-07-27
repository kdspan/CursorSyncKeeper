# CursorSyncKeeper（光标同步守护者）

轻量级 Windows 工具：监听**系统底层显示变化事件**，在全屏游戏退出 / 分辨率切换 /
显示器热插拔 / 睡眠唤醒导致系统从「软件鼠标」切回「硬件鼠标」并**不再恢复**时，
自动重新启用**软件鼠标**，无需重启电脑。

## 问题根因

Windows 默认使用 **硬件鼠标光标**（GPU 的 Multiplane Overlay，MPO）来绘制指针。
某些全屏应用或显示驱动重置会把系统切回硬件鼠标路径，并且之后**不会自动恢复**；
而软件鼠标（由 DWM 合成）才是稳定可用的方案。注册表里手动改为软件鼠标后，
**必须重启**驱动才会生效——这正是“改注册表 + 重启才恢复”现象的来源。

## 核心修复

通过禁用 DWM 叠加层强制软件鼠标：

1. 写入 `HKLM\SOFTWARE\Microsoft\Windows\DWM\OverlayTestMode = 5`
   （禁用 MPO → 强制 DWM 软件合成光标）。此键位于 `HKLM`，**需管理员权限**。
2. 对鼠标设置做一次**状态切换**（`SystemParametersInfo` 切到相反值再写回），
   让系统真正「重新读取并应用」设置（同值写回是 no-op，不会恢复）。
3. **触发显卡驱动重置**（`Win+Ctrl+Shift+B` 热键），使驱动立即重新加载
   `OverlayTestMode`，**无需重启**即可丢弃硬件光标、切回软件鼠标。
4. 重载整套光标方案（`SPI_SETCURSORS`）。

## 设计要点

- **纯事件驱动，无轮询**：程序 99.9% 时间阻塞在 `GetMessage`，CPU 占用为 0。
- **精准触发**：仅 `WM_DISPLAYCHANGE` / `WM_DEVICECHANGE` / `WM_POWERBROADCAST`
  时动作，用一次性 `SetTimer(500ms)` 延迟到驱动完成重置后再修复。
- **开机自启（管理员）**：通过计划任务（`schtasks`，`onlogon` + 最高权限）
  注册守护进程，登录即自动以管理员权限运行，可随时重写 `HKLM`。

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
- `build\CursorSyncKeeper_Setup.exe` — **自包含安装向导**：安装包内已内嵌守护进程与控制面板，
  单独分发 `CursorSyncKeeper_Setup.exe` 即可完成安装，无需附带其他文件。

## 安装包（安装向导，需管理员）

`CursorSyncKeeper_Setup.exe` 是一个**安装向导**：欢迎 → 选择安装位置 → 确认
→ 完成。可执行**安装**（首次）或**重装**（已存在时刷新并重新注册）。

它是**单文件自包含**的安装程序：守护进程 `CursorSyncKeeper.exe` 与控制面板
`CursorSyncKeeperPanel.exe` 已作为二进制资源内嵌其中，安装时自解压到目标目录，
因此**只需分发这一个 exe 即可**，无需附带其他文件。

- **安装 / 重装**：复制文件到所选目录（默认 `Program Files\CursorSyncKeeper`，
  可自定义），**写入 `HKLM` 禁用 MPO（由安装向导自身完成）**，**注册登录自启
  计划任务（由安装向导自身完成）**，启动守护进程，并**立即执行一次修复**
  （约 1 秒屏幕黑闪，属正常）。
- 安装后的软件由两个独立程序组成：
  - **守护进程** `CursorSyncKeeper.exe`：精简的纯事件驱动监控器，只负责运行时
    修复与监听显示变化事件；**不包含**任何安装 / 卸载 / 写注册表 / 注册计划任务的
    代码（这些均由下方两个程序负责）。
  - **控制面板** `CursorSyncKeeperPanel.exe`：用于**单次修复**或**卸载**（卸载不
    执行修复，不黑屏）。

> 写 `HKLM` / 注册计划任务 / 删除 `Program Files` 这类提权操作集中在安装向导与
> 控制面板共用的 `AdminOps` 模块中；守护进程本身不链接该模块，保持轻量。

> 安装向导自带 `requireAdministrator` 清单，始终以管理员运行，故安装过程
> 不会重复弹 UAC。

## 命令行

守护进程本体（`CursorSyncKeeper.exe`）：

- 不带参数运行即为后台事件驱动的修复监控器（由安装向导或登录计划任务以
  管理员权限启动），本身**不提供** `/install`、`/uninstall`、`/fix` 等命令。
- 运行时修复（含写 `HKLM\DWM\OverlayTestMode`）是修复功能本身的一部分，仍由
  `CursorFixer` 完成。

安装向导（`CursorSyncKeeper_Setup.exe`，需管理员）：

```bat
CursorSyncKeeper_Setup.exe           :: 打开安装向导（安装 / 重装，可选位置）
```

控制面板（`CursorSyncKeeperPanel.exe`，需管理员）：

```bat
CursorSyncKeeperPanel.exe           :: 打开控制面板（单次修复 / 卸载）
CursorSyncKeeperPanel.exe /uninstall :: 静默卸载（供“程序和功能”调用）
```

> 写 `HKLM` / `Program Files` 需管理员权限。非管理员运行时程序会自动以 `runas`
> 重新启动并弹出 UAC 提示。

## 控制面板与卸载

- **单次修复**：立即执行一次软件鼠标修复（约 1 秒屏幕黑闪，属正常）。
- **卸载**：停止守护进程、移除登录自启计划任务、移除 `HKLM` 注册表项、删除程序
  文件与日志。**卸载过程不执行修复，因此不会出现屏幕黑闪。**
- “程序和功能”中的 `CursorSyncKeeper` 项会启动控制面板（或静默 `/uninstall`）。

## 验证

安装后：
- `HKLM\SOFTWARE\Microsoft\Windows\DWM\OverlayTestMode` == 5
- 计划任务 `CursorSyncKeeper` 存在于 Task Scheduler，登录触发、最高权限
- 进程 `CursorSyncKeeper.exe` 在后台常驻（事件监听，CPU≈0%）

## 为什么不需要轮询式服务？

显示模式/驱动变化是确定事件（广播消息）。监听这些消息比周期扫描更高效、更省电，
守护进程以管理员计划任务常驻即可满足“轻量 + 可重写 HKLM”的诉求。

## 文件与存储位置（符合 Windows 规范）

- 程序文件：`C:\Program Files\CursorSyncKeeper\`（二进制 + 自带安装包）
- 运行日志：`C:\ProgramData\CursorSyncKeeper\install.log`（安装/修复/卸载操作记录）
- 系统修复键值：`HKLM\SOFTWARE\Microsoft\Windows\DWM\OverlayTestMode = 5`
- 自启方式：计划任务 `CursorSyncKeeper`（`onlogon` + 最高权限）
- 卸载入口：控制面板“程序和功能”中的 `CursorSyncKeeper` 项

> 安装包自带 `requireAdministrator` 清单并会在未提权时自动以 UAC 重新启动，
> 因此写入 Program Files 与 HKLM 不会因权限不足而静默失败。

## 故障排查

若安装后光标仍异常：
1. 查看 `C:\ProgramData\CursorSyncKeeper\install.log` 中是否有
   `CopyFiles / InstallScheduledTask / WriteARP / StartMenu` 失败的记录。
2. 确认 `HKLM\SOFTWARE\Microsoft\Windows\DWM\OverlayTestMode == 5`。
3. 以管理员运行 `CursorSyncKeeper_Setup.exe` 点击「立即修复」手动触发一次。
4. 修复会触发约 1 秒屏幕黑闪（显卡驱动重置），属正常现象。

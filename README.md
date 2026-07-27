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

产物：`build\CursorSyncKeeper.exe`（无外部依赖）。

## 安装包（推荐，需管理员）

`CursorSyncKeeper_Setup.exe` 是图形化安装程序，提供 **安装 / 重装 / 卸载**
三个操作，并带有 **立即修复** 按钮：

- **安装**：复制文件到 `Program Files\CursorSyncKeeper`，写入 `HKLM` 禁用 MPO，
  注册登录自启计划任务，启动守护进程，并**立即执行一次修复**（见下）。
- **重装**：停止守护进程、刷新程序文件、重新注册并**再执行一次修复**。
- **卸载**：移除计划任务、`HKLM` 项、程序文件与“添加/删除程序”条目。
- **立即修复**：随时手动触发一次软件鼠标修复（无需重装）。

> 安装包自带 `requireAdministrator` 清单，始终以管理员运行，故安装/卸载过程
> 不会重复弹 UAC。

## 命令行（守护进程本体，需管理员）

```bat
CursorSyncKeeper.exe /install     :: 禁用 MPO + 注册计划任务 + 启动守护进程
CursorSyncKeeper.exe /uninstall   :: 删除计划任务 + 移除 HKLM 项
CursorSyncKeeper.exe /fix         :: 单次执行软件鼠标修复后退出（不驻留）
```

> 安装/卸载/修复会请求 UAC 提权（写 `HKLM` 必须管理员）。非管理员运行时程序会
> 自动以 `runas` 重新启动并弹出 UAC 提示。

## 安装后自动修复

每次 **安装 / 重装** 完成后，安装程序都会自动调用一次 `CursorSyncKeeper.exe /fix`，
在无需等待任何事件的前提下，立即把系统切回软件鼠标（含一次显卡驱动重置，约 1 秒
屏幕黑闪属正常）。守护进程启动后也会在首次运行时修复一次，并对后续显示变化事件
持续守护。

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
   `CopyFiles / RunDaemon / WriteARP` 失败的记录。
2. 确认 `HKLM\SOFTWARE\Microsoft\Windows\DWM\OverlayTestMode == 5`。
3. 以管理员运行 `CursorSyncKeeper_Setup.exe` 点击「立即修复」手动触发一次。
4. 修复会触发约 1 秒屏幕黑闪（显卡驱动重置），属正常现象。

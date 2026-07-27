# CursorSyncKeeper（光标同步守护者）

轻量级 Windows 工具：监听**系统底层显示变化事件**，在游戏退出 / 分辨率切换 /
显示器热插拔 / 睡眠唤醒导致鼠标拖影（`MouseTrails`）丢失时，自动将其恢复。

## 设计要点

- **纯事件驱动，无轮询**：程序 99.9% 时间阻塞在 `GetMessage`，CPU 占用为 0。
- **精准触发**：仅当 `WM_DISPLAYCHANGE` / `WM_DEVICECHANGE` / `WM_POWERBROADCAST`
  发生时才动作，不扫描、不 `Sleep` 轮询。
- **延迟修复**：收到事件后用**一次性 `SetTimer`**（500ms）代替阻塞式 `Sleep`，
  确保显示驱动完成上下文重置后再写回内核状态。
- **根治原理**：从 `HKCU\Control Panel\Mouse\MouseTrails` 读取用户已保存的值，
  调用 `SystemParametersInfo(SPI_SETMOUSETRAILS, ...)` 强制内核重新加载该标志，
  覆盖驱动重置导致的“内核状态丢失”。

## 构建

需要 MSVC（已验证 Visual Studio 2022 Community）+ CMake + Ninja。

```bat
:: 在 "Developer Command Prompt for VS" 或初始化 vcvarsall 后执行
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

产物：`build\CursorSyncKeeper.exe`（约 30~50 KB，无外部依赖）。

## 安装 / 卸载（开机自启，无需管理员）

```bat
CursorSyncKeeper.exe /install     :: 写入 HKCU\...\Run，登录后自动运行
CursorSyncKeeper.exe /uninstall   :: 删除该注册表项
```

> 设计为写入 `HKCU\Run` 而非系统服务（Session 0），因为 `MouseTrails` 是**用户
> 会话级**设置，运行在 `HKCU` 上下文的后台进程即可正确操作，且无需管理员权限。

## 为什么不需要轮询式服务？

`MouseTrails` 是会话级设置，改动它的事件是确定的（显示模式变化会广播消息）。
监听这些消息比周期扫描更高效、更省电，也完全满足“轻量常驻守护”的诉求。

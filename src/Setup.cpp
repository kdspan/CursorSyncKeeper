#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <objbase.h>
#include <string>
#include "resource.h"
#include "AdminOps.h"
#include "CursorFixer.h"

// ===========================================================================
// CursorSyncKeeper_Setup  --  install / reinstall WIZARD
//
//   A step-by-step wizard (Welcome -> Choose location -> Confirm -> Finish).
//   Performs a fresh INSTALL or a REINSTALL (over an existing install) into the
//   user-chosen directory.
//
//   All privileged, install-time work -- writing the HKLM registry value
//   (OverlayTestMode) and registering the logon auto-start scheduled task --
//   is done HERE (and the matching uninstall lives in the control panel).
//   The lightweight daemon (CursorSyncKeeper.exe) contains NONE of this; it is
//   only started so it can run the runtime fix and monitor display events.
//
//   The "control panel" program (CursorSyncKeeperPanel.exe) owns SINGLE-FIX and
//   UNINSTALL, so those buttons are intentionally NOT in this wizard.
//
//   This program carries a requireAdministrator manifest, so it always runs
//   elevated and can write Program Files + HKLM without repeated UAC prompts.
// ===========================================================================

// ---- helpers -------------------------------------------------------------

static std::wstring GetInstallDirDefault() {
    wchar_t buf[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, buf);
    return std::wstring(buf) + L"\\CursorSyncKeeper";
}

// Extract a binary resource (RT_RCDATA) into a file. This is how Setup ships
// the daemon + control panel without needing those files on disk next to it.
static bool ExtractResource(WORD resId, const std::wstring& outPath) {
    HMODULE hMod = GetModuleHandleW(nullptr);
    // RT_RCDATA is declared as LPSTR; widen it for the *W API.
    HRSRC hRes = FindResourceW(hMod, MAKEINTRESOURCEW(resId),
                               reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (!hRes) {
        AdminOps::Log(L"[Extract] FindResource failed for " + std::to_wstring(resId));
        return false;
    }
    const HGLOBAL hGlob = LoadResource(hMod, hRes);
    if (!hGlob) return false;
    const DWORD size = SizeofResource(hMod, hRes);
    LPVOID p = LockResource(hGlob);
    if (!p || size == 0) return false;

    HANDLE hFile = CreateFileW(outPath.c_str(), GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        AdminOps::Log(L"[Extract] CreateFile failed for " + outPath +
                      L" (err " + std::to_wstring(GetLastError()) + L")");
        return false;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(hFile, p, size, &written, nullptr);
    CloseHandle(hFile);
    if (!ok || written != size) {
        AdminOps::Log(L"[Extract] WriteFile short for " + outPath +
                      L" (" + std::to_wstring(written) + L"/" +
                      std::to_wstring(size) + L")");
        return false;
    }
    return true;
}

static bool CopyFiles(const std::wstring& dir) {
    if (!CreateDirectoryW(dir.c_str(), nullptr)) {
        const DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            AdminOps::Log(L"[CopyFiles] cannot create dir " + dir + L" (err " +
                          std::to_wstring(e) + L")");
            return false;
        }
    }
    bool ok = true;
    // Self-extract the daemon + control panel from resources embedded in this
    // Setup.exe, so the installer is a single standalone file with no external
    // dependencies at install time.
    if (!ExtractResource(IDR_DAEMON_BIN, dir + L"\\CursorSyncKeeper.exe")) {
        AdminOps::Log(L"[CopyFiles] daemon extract failed");
        ok = false;
    }
    if (!ExtractResource(IDR_PANEL_BIN, dir + L"\\CursorSyncKeeperPanel.exe")) {
        AdminOps::Log(L"[CopyFiles] panel extract failed");
        ok = false;
    }
    // Also drop a copy of this Setup.exe into the install dir (for parity /
    // reinstall convenience). It is read from the running process itself.
    wchar_t self[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    if (!CopyFileW(self, (dir + L"\\CursorSyncKeeper_Setup.exe").c_str(), FALSE)) {
        AdminOps::Log(L"[CopyFiles] setup self-copy failed (err " +
                      std::to_wstring(GetLastError()) + L")");
        ok = false;
    }
    AdminOps::Log(L"[CopyFiles] -> " + std::wstring(ok ? L"ok" : L"FAILED"));
    return ok;
}

// ---- operations ----------------------------------------------------------

// The actual install steps. All privileged work happens here, not in the
// daemon. The daemon is only launched at the end so it can run the runtime
// fix (Apply) once and then monitor; it never writes the install registry or
// registers the scheduled task itself.
static std::wstring DoInstall(const std::wstring& dir) {
    bool ok = true;
    std::wstring detail;
    AdminOps::StopDaemon();   // ensure only one monitoring daemon (avoid duplicate flashes)

    if (!CopyFiles(dir)) { detail += L"• 复制文件到安装目录失败（权限不足？）\n"; ok = false; }

    // (1) persistently enable software mouse (write HKLM OverlayTestMode=5).
    if (!CursorFixer::EnableSoftwareMouseRegistry()) {
        detail += L"• 写入 HKLM 注册表 (OverlayTestMode) 失败\n"; ok = false;
    }
    // (2) register the logon auto-start scheduled task (elevated, no UAC).
    if (!AdminOps::InstallScheduledTask(dir + L"\\CursorSyncKeeper.exe")) {
        detail += L"• 注册登录自启计划任务失败\n"; ok = false;
    }
    // (3) launch the daemon (no args) so it applies the fix once + monitors.
    AdminOps::RunNoWait(L"\"" + dir + L"\\CursorSyncKeeper.exe\"");
    // The daemon's startup Apply() performs the live fix (one screen flash),
    // so we must NOT call /fix here (that would add a redundant 2nd flash).
    if (!AdminOps::WriteARP(dir)) { detail += L"• 写入卸载信息失败\n"; ok = false; }
    // (4) standard Start Menu shortcuts (control panel + uninstall), like any
    // normal Windows program.
    if (!AdminOps::CreateStartMenuShortcuts(dir)) {
        detail += L"• 创建开始菜单快捷方式失败\n"; ok = false;
    }

    if (ok) {
        return L"安装完成。\n"
               L"• 安装位置：" + dir + L"\n"
               L"• 软件鼠标 (MPO) 已启用 (OverlayTestMode=5)\n"
               L"• 已立即执行一次修复（约 1 秒屏幕黑闪，属正常）\n"
               L"• 已注册登录自启计划任务\n"
               L"• 已创建开始菜单程序项（控制面板 / 卸载）\n"
               L"• 守护进程已在后台常驻 (事件驱动, CPU≈0%)\n\n"
               L"开始菜单「CursorSyncKeeper」中可打开控制面板进行单次修复或卸载。";
    }
    return L"安装未完全成功，请查看日志：\n" + AdminOps::LogDir() + L"\\install.log\n" + detail;
}

static std::wstring DoReinstall(const std::wstring& dir) {
    bool ok = true;
    std::wstring detail;
    AdminOps::StopDaemon();
    if (!CopyFiles(dir)) { detail += L"• 刷新文件失败\n"; ok = false; }
    if (!CursorFixer::EnableSoftwareMouseRegistry()) { detail += L"• 重写注册表失败\n"; ok = false; }
    if (!AdminOps::InstallScheduledTask(dir + L"\\CursorSyncKeeper.exe")) {
        detail += L"• 重新注册计划任务失败\n"; ok = false;
    }
    AdminOps::RunNoWait(L"\"" + dir + L"\\CursorSyncKeeper.exe\"");
    if (!AdminOps::WriteARP(dir)) { detail += L"• 更新卸载信息失败\n"; ok = false; }
    if (!AdminOps::CreateStartMenuShortcuts(dir)) {
        detail += L"• 重建开始菜单快捷方式失败\n"; ok = false;
    }

    if (ok) {
        return L"重装完成。\n"
               L"已刷新文件、重新注册，并立即执行一次修复。\n"
               L"位置：" + dir + L"\n"
               L"开始菜单「CursorSyncKeeper」程序项已刷新。";
    }
    return L"重装未完成，请查看日志：\n" + AdminOps::LogDir() + L"\\install.log\n" + detail;
}

// ---- wizard --------------------------------------------------------------

struct WizardState {
    std::wstring installDir;
    bool installed = false;
    int  page = 0;
    std::wstring result;
};
static WizardState g_wiz;

static bool BrowseForFolder(HWND owner, std::wstring& out) {
    BROWSEINFO bi = {};
    bi.hwndOwner = owner;
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    PIDLIST_ABSOLUTE pid = SHBrowseForFolder(&bi);
    if (!pid) return false;
    wchar_t buf[MAX_PATH] = {0};
    const bool ok = (SHGetPathFromIDListW(pid, buf) != FALSE);
    CoTaskMemFree(pid);
    if (ok) out = buf;
    return ok;
}

// Show one wizard page; hide controls belonging to the others.
static void ShowPage(HWND hwnd, int page) {
    g_wiz.page = page;

    const int hideIds[] = { IDC_W0_TEXT, IDC_W1_PROMPT, IDC_PATH,
                            IDC_BROWSE,   IDC_W2_TEXT,   IDC_W3_TEXT };
    for (int id : hideIds) {
        HWND c = GetDlgItem(hwnd, id);
        if (c) ShowWindow(c, SW_HIDE);
    }

    HWND back   = GetDlgItem(hwnd, IDC_BACK);
    HWND next   = GetDlgItem(hwnd, IDC_NEXT);
    HWND cancel = GetDlgItem(hwnd, IDC_CANCEL);

    auto setText = [&](int id, const std::wstring& t) {
        SetDlgItemTextW(hwnd, id, t.c_str());
    };

    switch (page) {
        case 0: // Welcome
            ShowWindow(GetDlgItem(hwnd, IDC_W0_TEXT), SW_SHOW);
            setText(IDC_W0_TEXT,
                L"欢迎使用 CursorSyncKeeper 安装向导。\n\n"
                L"本工具监听系统显示变化（全屏退出 / 分辨率切换 /\n"
                L"显卡驱动重置 / 睡眠唤醒），在系统切回硬件鼠标时\n"
                L"自动重新启用软件鼠标，无需重启。\n\n"
                L"点击「下一步」继续。");
            ShowWindow(back, SW_HIDE);
            ShowWindow(cancel, SW_SHOW);
            SetWindowTextW(next, L"下一步");
            ShowWindow(next, SW_SHOW);
            break;

        case 1: // Choose location
            ShowWindow(GetDlgItem(hwnd, IDC_W1_PROMPT), SW_SHOW);
            ShowWindow(GetDlgItem(hwnd, IDC_PATH), SW_SHOW);
            ShowWindow(GetDlgItem(hwnd, IDC_BROWSE), SW_SHOW);
            setText(IDC_W1_PROMPT, L"选择安装位置：");
            SetDlgItemTextW(hwnd, IDC_PATH, g_wiz.installDir.c_str());
            ShowWindow(back, SW_SHOW);
            ShowWindow(cancel, SW_SHOW);
            SetWindowTextW(next, L"下一步");
            ShowWindow(next, SW_SHOW);
            break;

        case 2: // Confirm
            ShowWindow(GetDlgItem(hwnd, IDC_W2_TEXT), SW_SHOW);
            if (g_wiz.installed) {
                setText(IDC_W2_TEXT,
                    L"检测到已安装版本（位于 " + g_wiz.installDir + L"）。\n"
                    L"将执行【重装】：刷新文件、重新注册并立即修复一次。\n"
                    L"如需完全移除，请使用控制面板的「卸载」。\n\n"
                    L"点击「重装」继续。");
            } else {
                setText(IDC_W2_TEXT,
                    L"将安装到以下位置：\n" + g_wiz.installDir + L"\n\n"
                    L"安装后会立即执行一次修复（约 1 秒屏幕黑闪，属正常），\n"
                    L"并注册登录自启计划任务。\n\n"
                    L"点击「安装」开始。");
            }
            ShowWindow(back, SW_SHOW);
            ShowWindow(cancel, SW_SHOW);
            SetWindowTextW(next, g_wiz.installed ? L"重装" : L"安装");
            ShowWindow(next, SW_SHOW);
            break;

        case 3: // Finish
            ShowWindow(GetDlgItem(hwnd, IDC_W3_TEXT), SW_SHOW);
            setText(IDC_W3_TEXT, g_wiz.result);
            ShowWindow(back, SW_HIDE);
            ShowWindow(cancel, SW_HIDE);
            SetWindowTextW(next, L"完成");
            ShowWindow(next, SW_SHOW);
            break;
    }
}

static INT_PTR CALLBACK WizardProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            // Show the embedded program icon in the title bar / taskbar.
            if (HICON hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr),
                    MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON, 0, 0,
                    LR_DEFAULTSIZE | LR_SHARED)) {
                SendMessageW(hwnd, WM_SETICON, ICON_SMALL, (LPARAM)hIcon);
                SendMessageW(hwnd, WM_SETICON, ICON_BIG,   (LPARAM)hIcon);
            }
            SetWindowTextW(hwnd, L"CursorSyncKeeper 安装向导");
            SetDlgItemTextW(hwnd, IDC_WIZ_TITLE,
                            L"CursorSyncKeeper —— 软件鼠标守护者 安装向导");
            // Real (Chinese) button labels -- the .rc only holds English
            // placeholders because rc.exe is ANSI-only.
            SetDlgItemTextW(hwnd, IDC_CANCEL, L"取消");
            SetDlgItemTextW(hwnd, IDC_BACK,   L"上一步");
            SetDlgItemTextW(hwnd, IDC_BROWSE, L"浏览...");
            g_wiz.installed = AdminOps::IsInstalled();
            g_wiz.installDir = g_wiz.installed ? AdminOps::ReadInstallLocation()
                                               : GetInstallDirDefault();
            if (g_wiz.installDir.empty()) g_wiz.installDir = GetInstallDirDefault();
            ShowPage(hwnd, 0);
            return TRUE;
        }

        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_BACK:
                    if (g_wiz.page > 0) ShowPage(hwnd, g_wiz.page - 1);
                    return TRUE;

                case IDC_CANCEL:
                    EndDialog(hwnd, 0);
                    return TRUE;

                case IDC_NEXT:
                    if (g_wiz.page == 0) {
                        ShowPage(hwnd, 1);
                    } else if (g_wiz.page == 1) {
                        wchar_t buf[MAX_PATH] = {0};
                        GetDlgItemTextW(hwnd, IDC_PATH, buf, MAX_PATH);
                        g_wiz.installDir = buf;
                        if (g_wiz.installDir.empty())
                            g_wiz.installDir = GetInstallDirDefault();
                        ShowPage(hwnd, 2);
                    } else if (g_wiz.page == 2) {
                        g_wiz.result = g_wiz.installed
                            ? DoReinstall(g_wiz.installDir)
                            : DoInstall(g_wiz.installDir);
                        ShowPage(hwnd, 3);
                    } else if (g_wiz.page == 3) {
                        EndDialog(hwnd, 0);
                    }
                    return TRUE;

                case IDC_BROWSE: {
                    std::wstring sel;
                    if (BrowseForFolder(hwnd, sel) && !sel.empty())
                        SetDlgItemTextW(hwnd, IDC_PATH, sel.c_str());
                    return TRUE;
                }
            }
            return FALSE;

        case WM_CLOSE:
            EndDialog(hwnd, 0);
            return TRUE;
    }
    return FALSE;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::wstring args;
    if (lpCmdLine) {
        int len = MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, nullptr, 0);
        if (len > 0) {
            args.resize(len - 1);
            MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, &args[0], len);
        }
    }

    // The installer must be elevated to write Program Files + HKLM. The embedded
    // manifest normally handles this; re-launch elevated as a safety net.
    if (!AdminOps::IsElevated()) {
        AdminOps::RelaunchElevated(args);
        CoUninitialize();
        return 0;
    }

    DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_WIZARD), nullptr, WizardProc, 0);

    CoUninitialize();
    return 0;
}

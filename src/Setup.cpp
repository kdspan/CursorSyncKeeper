#include <windows.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include <fstream>
#include "resource.h"

// ===========================================================================
// CursorSyncKeeper_Setup  --  installer package (install / reinstall / uninstall)
//
//   GUI (default) : dialog with Install / Reinstall / Uninstall buttons.
//   /uninstall    : silent uninstall (used by the Control Panel "Programs"
//                   and Features" entry).
//
// The installer copies the daemon (CursorSyncKeeper.exe) and itself into
// %ProgramFiles%\CursorSyncKeeper, asks the daemon to enable software mouse
// (HKLM MPO disable + scheduled task), and registers an ARP uninstall entry.
// It carries a requireAdministrator manifest so it always runs elevated.
// ===========================================================================

static const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CursorSyncKeeper";
static const wchar_t* kTaskName = L"CursorSyncKeeper";

// ---- helpers -------------------------------------------------------------

static bool IsElevated() {
    SID_IDENTIFIER_AUTHORITY ntAuth = SECURITY_NT_AUTHORITY;
    PSID adminGroup = nullptr;
    AllocateAndInitializeSid(&ntAuth, 2, SECURITY_BUILTIN_DOMAIN_RID,
                             DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                             &adminGroup);
    BOOL isMember = FALSE;
    if (adminGroup) {
        CheckTokenMembership(nullptr, adminGroup, &isMember);
        FreeSid(adminGroup);
    }
    return isMember != FALSE;
}

// Re-launch the current executable elevated (UAC prompt) with the given args.
// Used as a safety net in case the embedded manifest did not elevate us.
static void RelaunchElevated(const std::wstring& params) {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

// Logs install operations to %ProgramData%\CursorSyncKeeper\install.log.
// (Per Windows conventions, runtime data goes to ProgramData, not Program Files.)
static std::wstring LogDir() {
    wchar_t buf[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, buf);
    return std::wstring(buf) + L"\\CursorSyncKeeper";
}

static void Log(const std::wstring& msg) {
    std::wstring dir = LogDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring file = dir + L"\\install.log";
    std::wstring line = msg + L"\r\n";
    std::ofstream f(file, std::ios::app | std::ios::binary);
    if (f) f.write(reinterpret_cast<const char*>(line.c_str()),
                  static_cast<std::streamsize>(line.size() * sizeof(wchar_t)));
}

static std::wstring ExeDir() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring s(path);
    const size_t pos = s.find_last_of(L'\\');
    if (pos != std::wstring::npos) s = s.substr(0, pos);
    return s;
}

static std::wstring InstallDir() {
    wchar_t buf[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_PROGRAM_FILES, nullptr, 0, buf);
    return std::wstring(buf) + L"\\CursorSyncKeeper";
}

static bool RunWait(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmd;
    if (!CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE, 0,
                        nullptr, nullptr, &si, &pi)) {
        return false;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code == 0;
}

// Ask the installed daemon to enable/disable software mouse (it writes HKLM
// and registers/removes the scheduled task). It inherits our elevation, so no
// extra UAC prompt. Returns true if the daemon exited with code 0.
static bool RunDaemon(const std::wstring& verb, bool silent) {
    const std::wstring exe = InstallDir() + L"\\CursorSyncKeeper.exe";
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"[RunDaemon] daemon missing at " + exe);
        return false;
    }
    const std::wstring params = L"/" + verb + (silent ? L" /silent" : L"");
    const bool ok = RunWait(L"\"" + exe + L"\" " + params);
    Log(L"[RunDaemon] " + verb + (ok ? L" -> ok" : L" -> FAILED"));
    return ok;
}

static void StopDaemon() {
    RunWait(L"taskkill /f /im CursorSyncKeeper.exe");
}

static bool CopyFiles() {
    const std::wstring src = ExeDir();
    const std::wstring dst = InstallDir();
    if (!CreateDirectoryW(dst.c_str(), nullptr)) {
        const DWORD e = GetLastError();
        if (e != ERROR_ALREADY_EXISTS) {
            Log(L"[CopyFiles] cannot create dir " + dst + L" (err " + std::to_wstring(e) + L")");
            return false;
        }
    }
    bool ok = true;
    if (!CopyFileW((src + L"\\CursorSyncKeeper.exe").c_str(),
                   (dst + L"\\CursorSyncKeeper.exe").c_str(), FALSE)) {
        Log(L"[CopyFiles] CursorSyncKeeper.exe failed (err " + std::to_wstring(GetLastError()) + L")");
        ok = false;
    }
    if (!CopyFileW((src + L"\\CursorSyncKeeper_Setup.exe").c_str(),
                   (dst + L"\\CursorSyncKeeper_Setup.exe").c_str(), FALSE)) {
        Log(L"[CopyFiles] CursorSyncKeeper_Setup.exe failed (err " + std::to_wstring(GetLastError()) + L")");
        ok = false;
    }
    Log(L"[CopyFiles] -> " + std::wstring(ok ? L"ok" : L"FAILED"));
    return ok;
}

static void RemoveFiles() {
    const std::wstring dst = InstallDir();
    RunWait(L"cmd /c rmdir /s /q \"" + dst + L"\"");
}

static bool WriteARP() {
    const std::wstring dst = InstallDir();
    HKEY hKey = nullptr;
    DWORD disp = 0;
    const LONG res = RegCreateKeyExW(
        HKEY_LOCAL_MACHINE, kUninstallKey, 0, nullptr, REG_OPTION_NON_VOLATILE,
        KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &hKey, &disp);
    if (res != ERROR_SUCCESS) {
        Log(L"[WriteARP] RegCreateKeyEx failed (err " + std::to_wstring(res) + L")");
        return false;
    }

    const std::wstring display = L"CursorSyncKeeper";
    RegSetValueExW(hKey, L"DisplayName", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(display.c_str()),
                   static_cast<DWORD>((display.size() + 1) * sizeof(wchar_t)));

    const std::wstring uninst = L"\"" + dst + L"\\CursorSyncKeeper_Setup.exe\" /uninstall";
    RegSetValueExW(hKey, L"UninstallString", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(uninst.c_str()),
                   static_cast<DWORD>((uninst.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(hKey, L"QuietUninstallString", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(uninst.c_str()),
                   static_cast<DWORD>((uninst.size() + 1) * sizeof(wchar_t)));

    const std::wstring icon = dst + L"\\CursorSyncKeeper.exe";
    RegSetValueExW(hKey, L"DisplayIcon", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(icon.c_str()),
                   static_cast<DWORD>((icon.size() + 1) * sizeof(wchar_t)));

    const std::wstring loc = dst;
    RegSetValueExW(hKey, L"InstallLocation", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(loc.c_str()),
                   static_cast<DWORD>((loc.size() + 1) * sizeof(wchar_t)));

    const DWORD noModify = 1;
    RegSetValueExW(hKey, L"NoModify", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&noModify), sizeof(DWORD));
    RegSetValueExW(hKey, L"NoRepair", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&noModify), sizeof(DWORD));
    RegCloseKey(hKey);
    Log(L"[WriteARP] ok");
    return true;
}

static void RemoveARP() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                      0, KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        RegDeleteKeyW(hKey, L"CursorSyncKeeper");
        RegCloseKey(hKey);
    }
}

static bool IsInstalled() {
    HKEY hKey = nullptr;
    const LONG res = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0,
                                   KEY_READ | KEY_WOW64_64KEY, &hKey);
    if (res == ERROR_SUCCESS) { RegCloseKey(hKey); return true; }
    return false;
}

// ---- operations ----------------------------------------------------------

static void SetStatus(HWND hwnd, const std::wstring& msg) {
    SetDlgItemTextW(hwnd, IDC_STATUS, msg.c_str());
}

static void DoInstall(HWND hwnd) {
    bool ok = true;
    std::wstring detail;
    if (!CopyFiles()) {
        detail += L"• 复制文件到 Program Files 失败（权限不足？）\n";
        ok = false;
    }
    if (!RunDaemon(L"install", true)) {
        detail += L"• 注册软件鼠标 (HKLM) / 计划任务失败\n";
        ok = false;
    }
    if (!RunDaemon(L"fix", true)) {
        detail += L"• 立即修复执行失败\n";
        ok = false;
    }
    if (!WriteARP()) {
        detail += L"• 写入“添加/删除程序”信息失败\n";
        ok = false;
    }

    if (ok && IsInstalled()) {
        SetStatus(hwnd,
            L"安装完成。\n"
            L"• 已安装到 " + InstallDir() + L"\n"
            L"• 软件鼠标 (MPO) 已启用 (OverlayTestMode=5)\n"
            L"• 已立即执行一次修复（约 1 秒屏幕黑闪属正常）\n"
            L"• 计划任务已注册，登录后自动运行\n"
            L"• 守护进程已在后台常驻 (事件驱动, CPU≈0%)");
    } else {
        SetStatus(hwnd,
            L"安装未完全成功，请查看日志：\n" + LogDir() + L"\\install.log\n" + detail);
    }
}

static void DoReinstall(HWND hwnd) {
    bool ok = true;
    std::wstring detail;
    StopDaemon();
    if (!CopyFiles()) { detail += L"• 刷新文件失败\n"; ok = false; }
    if (!RunDaemon(L"install", true)) { detail += L"• 重新注册失败\n"; ok = false; }
    if (!RunDaemon(L"fix", true)) { detail += L"• 重新修复失败\n"; ok = false; }
    if (!WriteARP()) { detail += L"• 更新卸载信息失败\n"; ok = false; }

    SetStatus(hwnd, ok
        ? L"重装完成。\n已刷新文件、重新注册，并立即执行一次修复。"
        : L"重装未完成，请查看日志：\n" + LogDir() + L"\\install.log\n" + detail);
}

static void DoFix(HWND hwnd) {
    if (!IsInstalled()) {
        SetStatus(hwnd,
            L"尚未安装，无法执行修复。\n请先点击「安装」（或「重装」）。");
        return;
    }
    if (RunDaemon(L"fix", true)) {
        SetStatus(hwnd,
            L"已执行一次软件鼠标修复。\n"
            L"如光标问题依旧，可再次点击「立即修复」。\n"
            L"（修复会触发约 1 秒的屏幕黑闪，这是显卡驱动重置的正常表现）");
    } else {
        SetStatus(hwnd,
            L"修复执行失败，请查看日志：\n" + LogDir() + L"\\install.log");
    }
}

static void DoUninstall(HWND hwnd) {
    RunDaemon(L"uninstall", true); // remove HKLM value + scheduled task
    StopDaemon();                  // kill the running daemon
    RemoveFiles();                 // delete %ProgramFiles%\CursorSyncKeeper
    RemoveARP();                   // remove the Programs-and-Features entry
    // Clean up our ProgramData log directory.
    RemoveDirectoryW(LogDir().c_str());
    SetStatus(hwnd, L"卸载完成。所有文件、计划任务与注册表项均已移除。");
}

static void DoUninstallSilent() {
    StopDaemon();
    RunDaemon(L"uninstall", true);
    RemoveFiles();
    RemoveARP();
    RemoveDirectoryW(LogDir().c_str());
}

// ---- dialog --------------------------------------------------------------

static INT_PTR CALLBACK DialogProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM) {
    switch (msg) {
        case WM_INITDIALOG: {
            SetWindowTextW(hwnd, L"CursorSyncKeeper 安装程序");
            SetDlgItemTextW(hwnd, IDC_TITLE, L"CursorSyncKeeper —— 软件鼠标守护者");
            SetDlgItemTextW(hwnd, IDC_HINT,
                L"监听显示变化事件，在全屏退出 / 驱动重置导致系统切回硬件鼠标"
                L"时，自动重新启用软件鼠标（无需重启）。");
            SetDlgItemTextW(hwnd, IDC_BTN_INSTALL, L"安装");
            SetDlgItemTextW(hwnd, IDC_BTN_REINSTALL, L"重装");
            SetDlgItemTextW(hwnd, IDC_BTN_UNINSTALL, L"卸载");
            SetDlgItemTextW(hwnd, IDC_BTN_FIX, L"立即修复");
            SetDlgItemTextW(hwnd, IDC_BTN_EXIT, L"退出");

            const std::wstring s = IsInstalled()
                ? L"当前状态：已安装。\n可点击「重装」刷新文件与注册，或「卸载」移除。"
                : L"当前状态：未安装。\n点击「安装」开始。";
            SetDlgItemTextW(hwnd, IDC_STATUS, s.c_str());
            return TRUE;
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_BTN_INSTALL:     DoInstall(hwnd);   return TRUE;
                case IDC_BTN_REINSTALL:   DoReinstall(hwnd); return TRUE;
                case IDC_BTN_UNINSTALL:   DoUninstall(hwnd); return TRUE;
                case IDC_BTN_FIX:         DoFix(hwnd);       return TRUE;
                case IDC_BTN_EXIT:        EndDialog(hwnd, 0); return TRUE;
            }
            return FALSE;
        case WM_CLOSE:
            EndDialog(hwnd, 0);
            return TRUE;
    }
    return FALSE;
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR lpCmdLine, int) {
    std::wstring args;
    if (lpCmdLine) {
        int len = MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, nullptr, 0);
        if (len > 0) {
            args.resize(len - 1);
            MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, &args[0], len);
        }
    }

    // The installer must be elevated to write Program Files + HKLM. The embedded
    // manifest normally handles this; if it did not (e.g. overridden), re-launch
    // ourselves elevated via UAC as a safety net.
    if (!IsElevated()) {
        RelaunchElevated(args);
        return 0;
    }

    // Control-Panel "Uninstall" launches us with /uninstall -> do it quietly.
    if (args.find(L"uninstall") != std::wstring::npos) {
        DoUninstallSilent();
        return 0;
    }

    DialogBoxParamW(hInst, MAKEINTRESOURCEW(IDD_SETUP_DIALOG), nullptr,
                    DialogProc, 0);
    return 0;
}

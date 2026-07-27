#include "HiddenWindow.h"
#include "CursorFixer.h"
#include <windows.h>
#include <shellapi.h>
#include <string>

// ===========================================================================
// CursorSyncKeeper
//
//   /install   - (requires admin) disable MPO (software mouse), register a
//                elevated scheduled task that auto-starts the daemon at logon,
//                then launch the daemon immediately.
//   /uninstall - (requires admin) remove the task and the HKLM value.
//   (no args)  - run as the event-driven daemon (hidden window).
// ===========================================================================

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
static void RelaunchElevated(const wchar_t* params) {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.lpParameters = params;
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

// Run a command and wait for it to finish. Returns true if exit code == 0.
static bool RunWait(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    // CreateProcess needs a mutable command line.
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

// Launch a command without waiting (used for the long-lived daemon).
static void RunNoWait(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmd;
    if (CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

static std::wstring GetExePath() {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    return path;
}

static const wchar_t* kTaskName = L"CursorSyncKeeper";

static bool InstallScheduledTask() {
    // Run at every logon, with highest privileges (admin), no UAC prompt.
    std::wstring exe = GetExePath();
    std::wstring tr = L"\"" + exe + L"\"";
    std::wstring cmd = L"schtasks /create /tn \"" + std::wstring(kTaskName) +
                       L"\" /tr \"" + tr + L"\" /sc onlogon /rl highest /f";
    return RunWait(cmd);
}

static bool RemoveScheduledTask() {
    std::wstring cmd = L"schtasks /delete /tn \"" +
                       std::wstring(kTaskName) + L"\" /f";
    return RunWait(cmd);
}

static void StartDaemonNow() {
    // Launch our own binary (no args) without waiting; it becomes the daemon.
    RunNoWait(L"\"" + GetExePath() + L"\"");
}

static void Install() {
    if (!IsElevated()) {
        RelaunchElevated(L"/install");
        return;
    }
    const bool okReg  = CursorFixer::EnableSoftwareMouseRegistry();
    const bool okTask = InstallScheduledTask();
    StartDaemonNow();

    MessageBoxW(nullptr,
        okReg && okTask
            ? L"CursorSyncKeeper installed.\nSoftware mouse (MPO) enabled and "
              L"daemon registered to start at logon."
            : L"Installation completed with warnings.\nCheck that you ran this "
              L"as Administrator.",
        L"CursorSyncKeeper", MB_OK | MB_ICONINFORMATION);
}

static void Uninstall() {
    if (!IsElevated()) {
        RelaunchElevated(L"/uninstall");
        return;
    }
    RemoveScheduledTask();
    CursorFixer::DisableSoftwareMouseRegistry();
    MessageBoxW(nullptr, L"CursorSyncKeeper uninstalled.",
                L"CursorSyncKeeper", MB_OK | MB_ICONINFORMATION);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    std::wstring args;
    if (lpCmdLine) {
        int len = MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, nullptr, 0);
        if (len > 0) {
            args.resize(len - 1);
            MultiByteToWideChar(CP_ACP, 0, lpCmdLine, -1, &args[0], len);
        }
    }

    if (args.find(L"install")   != std::wstring::npos) { Install();   return 0; }
    if (args.find(L"uninstall") != std::wstring::npos) { Uninstall(); return 0; }

    // Normal run: hidden window + event-driven message pump (the daemon).
    HiddenWindow window;
    if (!window.Create()) {
        MessageBoxW(nullptr, L"Failed to create message window.",
                    L"CursorSyncKeeper", MB_OK | MB_ICONERROR);
        return 1;
    }

    // One-time restore on startup (in case the setting was already lost
    // before we began listening). Still 0% CPU afterwards.
    CursorFixer::Apply();

    window.RunMessageLoop();
    return 0;
}

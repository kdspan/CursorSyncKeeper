#include "AdminOps.h"
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <string>
#include <fstream>

static const wchar_t* kUninstallKey =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\CursorSyncKeeper";
static const wchar_t* kTaskName = L"CursorSyncKeeper";

namespace AdminOps {

bool IsElevated() {
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

void RelaunchElevated(const std::wstring& params) {
    wchar_t path[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, path, MAX_PATH);
    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.lpVerb = L"runas";
    sei.lpFile = path;
    sei.lpParameters = params.empty() ? nullptr : params.c_str();
    sei.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&sei);
}

bool RunWait(const std::wstring& cmd) {
    // Capture the child's stdout+stderr so failures are diagnosable in the log.
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE hRead = nullptr, hWrite = nullptr;
    if (!CreatePipe(&hRead, &hWrite, &sa, 0)) hRead = hWrite = nullptr;

    STARTUPINFOW si = { sizeof(si) };
    if (hRead && hWrite) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = hWrite;
        si.hStdError  = hWrite;
        si.hStdInput  = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmd;
    if (!CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, TRUE, 0,
                        nullptr, nullptr, &si, &pi)) {
        const DWORD err = GetLastError();
        Log(L"[RunWait] CreateProcess failed (err " + std::to_wstring(err) +
            L"): " + cmd);
        if (hRead)  CloseHandle(hRead);
        if (hWrite) CloseHandle(hWrite);
        return false;
    }
    if (hWrite) CloseHandle(hWrite);

    // Read all child output before waiting, so we never deadlock on a full pipe.
    std::string out;
    if (hRead) {
        char buf[1024];
        DWORD n = 0;
        while (ReadFile(hRead, buf, sizeof(buf), &n, nullptr) && n > 0)
            out.append(buf, n);
        CloseHandle(hRead);
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (!out.empty()) {
        // schtasks/cmd output is in the OEM codepage; convert for the UTF-16 log.
        const int wlen = MultiByteToWideChar(CP_OEMCP, 0, out.c_str(),
                                             (int)out.size(), nullptr, 0);
        std::wstring wout(wlen, L'\0');
        MultiByteToWideChar(CP_OEMCP, 0, out.c_str(), (int)out.size(),
                            &wout[0], wlen);
        Log(L"[RunWait] output: " + wout);
    }
    if (code != 0)
        Log(L"[RunWait] exit code " + std::to_wstring(code) + L": " + cmd);
    return code == 0;
}

void RunNoWait(const std::wstring& cmd) {
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = {};
    std::wstring mutableCmd = cmd;
    if (CreateProcessW(nullptr, &mutableCmd[0], nullptr, nullptr, FALSE,
                       CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

std::wstring LogDir() {
    wchar_t buf[MAX_PATH] = {0};
    SHGetFolderPathW(nullptr, CSIDL_COMMON_APPDATA, nullptr, 0, buf);
    return std::wstring(buf) + L"\\CursorSyncKeeper";
}

void Log(const std::wstring& msg) {
    std::wstring dir = LogDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    std::wstring file = dir + L"\\install.log";
    std::wstring line = msg + L"\r\n";
    std::ofstream f(file, std::ios::app | std::ios::binary);
    if (f) f.write(reinterpret_cast<const char*>(line.c_str()),
                  static_cast<std::streamsize>(line.size() * sizeof(wchar_t)));
}

void StopDaemon() {
    RunWait(L"taskkill /f /im CursorSyncKeeper.exe");
}

bool InstallScheduledTask(const std::wstring& exePath) {
    // Run at every logon, with highest privileges (admin), no UAC prompt.
    // The /tr value must be a SINGLE-quoted path (schtasks strips the outer
    // quotes). Do NOT wrap it in an extra pair or schtasks mis-parses the
    // program path and fails with "parameter is incorrect".
    std::wstring tr = L"\"" + exePath + L"\"";   // "C:\...\CursorSyncKeeper.exe"
    std::wstring cmd = L"schtasks /create /tn \"" + std::wstring(kTaskName) +
                       L"\" /tr " + tr + L" /sc onlogon /rl highest /f";
    Log(L"[InstallScheduledTask] " + cmd);
    const bool ok = RunWait(cmd);
    Log(L"[InstallScheduledTask] -> " + std::wstring(ok ? L"ok" : L"FAILED"));
    return ok;
}

bool RemoveScheduledTask() {
    std::wstring cmd = L"schtasks /delete /tn \"" +
                       std::wstring(kTaskName) + L"\" /f";
    return RunWait(cmd);
}

bool WriteARP(const std::wstring& dir) {
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

    // Uninstall is owned by the control panel program.
    const std::wstring uninst = L"\"" + dir + L"\\CursorSyncKeeperPanel.exe\"";
    const std::wstring quiet  = uninst + L" /uninstall";
    RegSetValueExW(hKey, L"UninstallString", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(uninst.c_str()),
                   static_cast<DWORD>((uninst.size() + 1) * sizeof(wchar_t)));
    RegSetValueExW(hKey, L"QuietUninstallString", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(quiet.c_str()),
                   static_cast<DWORD>((quiet.size() + 1) * sizeof(wchar_t)));

    const std::wstring icon = dir + L"\\CursorSyncKeeper.exe";
    RegSetValueExW(hKey, L"DisplayIcon", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(icon.c_str()),
                   static_cast<DWORD>((icon.size() + 1) * sizeof(wchar_t)));

    RegSetValueExW(hKey, L"InstallLocation", 0, REG_SZ,
                   reinterpret_cast<const BYTE*>(dir.c_str()),
                   static_cast<DWORD>((dir.size() + 1) * sizeof(wchar_t)));

    const DWORD noModify = 1;
    RegSetValueExW(hKey, L"NoModify", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&noModify), sizeof(DWORD));
    RegSetValueExW(hKey, L"NoRepair", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&noModify), sizeof(DWORD));
    RegCloseKey(hKey);
    Log(L"[WriteARP] ok");
    return true;
}

void RemoveARP() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                      0, KEY_SET_VALUE | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        RegDeleteKeyW(hKey, L"CursorSyncKeeper");
        RegCloseKey(hKey);
    }
}

bool IsInstalled() {
    HKEY hKey = nullptr;
    const LONG res = RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0,
                                   KEY_READ | KEY_WOW64_64KEY, &hKey);
    if (res == ERROR_SUCCESS) { RegCloseKey(hKey); return true; }
    return false;
}

std::wstring ReadInstallLocation() {
    HKEY hKey = nullptr;
    std::wstring r;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kUninstallKey, 0,
                      KEY_READ | KEY_WOW64_64KEY, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[512] = {0};
        DWORD sz = sizeof(buf);
        if (RegQueryValueExW(hKey, L"InstallLocation", 0, nullptr,
                             reinterpret_cast<BYTE*>(buf), &sz) == ERROR_SUCCESS) {
            r = buf;
        }
        RegCloseKey(hKey);
    }
    return r;
}

void RemoveFiles(const std::wstring& dir) {
    RunWait(L"cmd /c rmdir /s /q \"" + dir + L"\"");
}

// ---- Start Menu (All Users) shortcuts -------------------------------------

static std::wstring StartMenuProgramsDir() {
    // CSIDL_COMMON_PROGRAMS -> %ProgramData%\Microsoft\Windows\Start Menu\Programs
    // (visible to every user on the machine; correct for a per-machine install).
    wchar_t buf[MAX_PATH] = {0};
    if (SHGetFolderPathW(nullptr, CSIDL_COMMON_PROGRAMS, nullptr, 0, buf) == S_OK)
        return std::wstring(buf);
    return std::wstring();
}

// Create a single .lnk. Self-contains COM so callers need not initialize it.
static bool CreateShortcut(const std::wstring& lnkPath,
                           const std::wstring& target,
                           const std::wstring& args,
                           const std::wstring& iconPath) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool ok = false;
    IShellLinkW* psl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                   (void**)&psl))) {
        psl->SetPath(target.c_str());
        if (!args.empty()) psl->SetArguments(args.c_str());
        psl->SetIconLocation(iconPath.c_str(), 0);
        // Default working directory = the target's own folder.
        std::wstring work = target;
        const size_t pos = work.find_last_of(L'\\');
        if (pos != std::wstring::npos) {
            work = work.substr(0, pos);
            psl->SetWorkingDirectory(work.c_str());
        }
        IPersistFile* ppf = nullptr;
        if (SUCCEEDED(psl->QueryInterface(IID_IPersistFile, (void**)&ppf))) {
            if (SUCCEEDED(ppf->Save(lnkPath.c_str(), TRUE))) ok = true;
            ppf->Release();
        }
        psl->Release();
    }
    if (!ok) Log(L"[CreateShortcut] failed: " + lnkPath);
    CoUninitialize();
    return ok;
}

bool CreateStartMenuShortcuts(const std::wstring& dir) {
    const std::wstring progs = StartMenuProgramsDir();
    if (progs.empty()) {
        Log(L"[StartMenu] cannot resolve Programs folder");
        return false;
    }
    const std::wstring folder = progs + L"\\CursorSyncKeeper";
    CreateDirectoryW(folder.c_str(), nullptr);

    const std::wstring panel = dir + L"\\CursorSyncKeeperPanel.exe";
    const std::wstring icon  = dir + L"\\CursorSyncKeeper.exe";

    bool ok = true;
    if (!CreateShortcut(folder + L"\\CursorSyncKeeper 控制面板.lnk",
                        panel, L"", icon))
        ok = false;
    if (!CreateShortcut(folder + L"\\卸载 CursorSyncKeeper.lnk",
                        panel, L"/uninstall", icon))
        ok = false;

    Log(L"[StartMenu] -> " + std::wstring(ok ? L"ok" : L"FAILED"));
    return ok;
}

void RemoveStartMenuShortcuts() {
    const std::wstring progs = StartMenuProgramsDir();
    if (progs.empty()) return;
    const std::wstring folder = progs + L"\\CursorSyncKeeper";
    RunWait(L"cmd /c rmdir /s /q \"" + folder + L"\"");
    Log(L"[StartMenu] removed");
}

} // namespace AdminOps

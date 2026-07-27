#include "HiddenWindow.h"
#include "CursorFixer.h"
#include <windows.h>
#include <cstring>

static const wchar_t* RUN_KEY    = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static const wchar_t* VALUE_NAME  = L"CursorSyncKeeper";

static void Install() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        wchar_t path[MAX_PATH] = {0};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        RegSetValueExW(hKey, VALUE_NAME, 0, REG_SZ,
                       reinterpret_cast<const BYTE*>(path),
                       static_cast<DWORD>((wcslen(path) + 1) * sizeof(wchar_t)));
        RegCloseKey(hKey);
    }
    MessageBoxW(nullptr,
                L"Installed. CursorSyncKeeper will start automatically at logon.",
                L"CursorSyncKeeper", MB_OK | MB_ICONINFORMATION);
}

static void Uninstall() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, VALUE_NAME);   // remove the Run value (not the key itself)
        RegCloseKey(hKey);
    }
    MessageBoxW(nullptr, L"Uninstalled.", L"CursorSyncKeeper", MB_OK | MB_ICONINFORMATION);
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR lpCmdLine, int) {
    if (lpCmdLine && strstr(lpCmdLine, "/install"))   { Install();   return 0; }
    if (lpCmdLine && strstr(lpCmdLine, "/uninstall")) { Uninstall(); return 0; }

    // Normal run: hidden window + event-driven message pump.
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

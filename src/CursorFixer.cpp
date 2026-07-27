#include "CursorFixer.h"
#include <windows.h>

namespace {

const wchar_t* kDwmKey = L"SOFTWARE\\Microsoft\\Windows\\DWM";
const wchar_t* kOverlayValue = L"OverlayTestMode";
const DWORD    kOverlayTestMode = 5;   // disable MPO -> force software cursor

} // namespace

bool CursorFixer::EnableSoftwareMouseRegistry() {
    HKEY hKey = nullptr;
    DWORD disp = 0;
    LONG res = RegCreateKeyExW(HKEY_LOCAL_MACHINE, kDwmKey, 0, nullptr,
                               REG_OPTION_NON_VOLATILE, KEY_SET_VALUE,
                               nullptr, &hKey, &disp);
    if (res != ERROR_SUCCESS) return false;
    res = RegSetValueExW(hKey, kOverlayValue, 0, REG_DWORD,
                         reinterpret_cast<const BYTE*>(&kOverlayTestMode),
                         sizeof(kOverlayTestMode));
    RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}

void CursorFixer::DisableSoftwareMouseRegistry() {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDwmKey, 0, KEY_SET_VALUE, &hKey)
            == ERROR_SUCCESS) {
        RegDeleteValueW(hKey, kOverlayValue);
        RegCloseKey(hKey);
    }
}

void CursorFixer::ResetGraphicsDriver() {
    // Replicate Win+Ctrl+Shift+B to restart the graphics driver. This makes
    // the driver re-read OverlayTestMode and drop the hardware cursor without
    // a reboot. SendInput only works from an interactive session.
    INPUT inputs[8] = {};
    int n = 0;
    auto key = [&](WORD vk, bool down) {
        inputs[n].type = INPUT_KEYBOARD;
        inputs[n].ki.wVk = vk;
        inputs[n].ki.wScan = 0;
        inputs[n].ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
        inputs[n].ki.time = 0;
        inputs[n].ki.dwExtraInfo = 0;
        ++n;
    };
    key(VK_LWIN,   true);
    key(VK_CONTROL, true);
    key(VK_SHIFT,  true);
    key('B',       true);
    key('B',       false);
    key(VK_SHIFT,  false);
    key(VK_CONTROL, false);
    key(VK_LWIN,   false);
    SendInput(static_cast<UINT>(n), inputs, sizeof(INPUT));
}

void CursorFixer::Apply() {
    // 1) Ensure software mouse is enabled at the driver level.
    EnableSoftwareMouseRegistry();

    // 2) Force a real state transition on the mouse-trail setting so the
    //    system re-reads and re-applies it (writing the same value is a
    //    no-op and would NOT restore the lost visual state).
    UINT cur = 0;
    SystemParametersInfoW(SPI_GETMOUSETRAILS, 0, &cur, 0);
    const UINT toggle = (cur == 0) ? 1 : 0;
    SystemParametersInfoW(SPI_SETMOUSETRAILS, toggle, nullptr, SPIF_SENDCHANGE);

    // 3) Reset the GPU driver so OverlayTestMode takes effect immediately
    //    (this is the part that previously required a reboot).
    ResetGraphicsDriver();

    // 4) Restore the user's real trail preference and persist + broadcast it.
    SystemParametersInfoW(SPI_SETMOUSETRAILS, cur, nullptr,
                          SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);

    // 5) Reload the whole cursor scheme from the registry end-to-end.
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
}

#include "CursorFixer.h"
#include <windows.h>

namespace {

const wchar_t* kDwmKey = L"SOFTWARE\\Microsoft\\Windows\\DWM";
const wchar_t* kOverlayValue = L"OverlayTestMode";
const DWORD    kOverlayTestMode = 5;   // disable MPO -> force software cursor

const wchar_t* kDesktopKey = L"Control Panel\\Desktop";
const wchar_t* kTrailsValue = L"MouseTrails";

// Write HKCU\Control Panel\Desktop\MouseTrails as REG_SZ (that value is a
// string in the registry, not a DWORD).
bool WriteTrailsRegistry(const wchar_t* value) {
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kDesktopKey, 0, KEY_SET_VALUE, &hKey)
            != ERROR_SUCCESS)
        return false;
    const LONG res = RegSetValueExW(
        hKey, kTrailsValue, 0, REG_SZ,
        reinterpret_cast<const BYTE*>(value),
        static_cast<DWORD>((wcslen(value) + 1) * sizeof(wchar_t)));
    RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
}

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

void CursorFixer::EnsureOverlayTestModeAlive() {
    // Cheap, NON-flashing re-affirmation: open with query+set, read the current
    // value and only write it back when it has drifted away from 5. This keeps
    // MPO disabled (software cursor) during gameplay WITHOUT the black-flash
    // driver reset that ResetGraphicsDriver() would cause. The driver re-reads
    // the value on the next mode change, so rewriting now guarantees the
    // software-cursor path persists -- and ReassertSoftwareCursor() (which does
    // act live via SPI) handles the visible cursor in the meantime.
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kDwmKey, 0,
                      KEY_QUERY_VALUE | KEY_SET_VALUE, &hKey) != ERROR_SUCCESS)
        return;
    DWORD val = 0, sz = sizeof(val);
    const LONG r = RegQueryValueExW(hKey, kOverlayValue, nullptr, nullptr,
                                    reinterpret_cast<BYTE*>(&val), &sz);
    if (r != ERROR_SUCCESS || val != kOverlayTestMode) {
        RegSetValueExW(hKey, kOverlayValue, 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&kOverlayTestMode),
                       sizeof(kOverlayTestMode));
    }
    RegCloseKey(hKey);
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

bool CursorFixer::ForceSoftwareCursorTrails() {
    // A real 0 -> -1 state transition: first drop to 0 (runtime only, not
    // persisted), then set the -1 sentinel. Writing the same value twice is a
    // no-op for the input stack, so the transition is what forces the OS to
    // actually re-enter the software-cursor path.
    SystemParametersInfoW(SPI_SETMOUSETRAILS, 0, nullptr, 0);
    SystemParametersInfoW(SPI_SETMOUSETRAILS, static_cast<UINT>(-1), nullptr,
                          SPIF_SENDCHANGE);
    // Persist as the string "-1" ourselves (SPIF_UPDATEINIFILE would store the
    // unsigned decimal form "4294967295" instead of the canonical "-1").
    return WriteTrailsRegistry(L"-1");
}

void CursorFixer::RestoreMouseTrails() {
    SystemParametersInfoW(SPI_SETMOUSETRAILS, 0, nullptr, SPIF_SENDCHANGE);
    WriteTrailsRegistry(L"0");
}

bool CursorFixer::TrailsSentinelActive() {
    UINT cur = 0;
    SystemParametersInfoW(SPI_GETMOUSETRAILS, 0, &cur, 0);
    return cur == static_cast<UINT>(-1);
}

void CursorFixer::ReassertSoftwareCursor() {
    ForceSoftwareCursorTrails();
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
}

void CursorFixer::Apply() {
    // 1) Ensure software mouse is enabled at the driver level (MPO off).
    EnableSoftwareMouseRegistry();

    // 2) Force the OS-level software cursor via the MouseTrails=-1 sentinel.
    //    This is what actually recovers from hardware-cursor lock-in caused by
    //    exclusive-fullscreen games (e.g. Genshin + FPSUnlocker): the hardware
    //    cursor cannot draw trails, so the OS is forced onto the software path.
    ForceSoftwareCursorTrails();

    // 3) Reset the GPU driver so OverlayTestMode takes effect immediately
    //    (this is the part that previously required a reboot).
    ResetGraphicsDriver();

    // 4) Re-assert the sentinel AFTER the driver reset, because the reset
    //    itself can momentarily re-evaluate cursor state.
    ForceSoftwareCursorTrails();

    // 5) Reload the whole cursor scheme from the registry end-to-end.
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
}

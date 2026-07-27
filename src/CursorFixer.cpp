#include "CursorFixer.h"
#include <windows.h>
#include <cstdlib>

namespace {

// Read the persisted mouse-trail value (0 = off, 1-7 = number of trail images)
// from HKCU\Control Panel\Mouse\MouseTrails.
int ReadTrailsFromRegistry() {
    int trails = 0;
    HKEY hKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\Mouse", 0,
                       KEY_READ, &hKey) == ERROR_SUCCESS) {
        wchar_t buf[16] = {0};
        DWORD size = sizeof(buf);
        DWORD type = 0;
        if (RegQueryValueExW(hKey, L"MouseTrails", nullptr, &type,
                             reinterpret_cast<LPBYTE>(buf), &size) == ERROR_SUCCESS &&
            type == REG_SZ) {
            trails = static_cast<int>(wcstol(buf, nullptr, 10));
        }
        RegCloseKey(hKey);
    }
    if (trails < 0) trails = 0;
    if (trails > 7) trails = 7;
    return trails;
}

} // namespace

void CursorFixer::Apply() {
    const int trails = ReadTrailsFromRegistry();

    // Writing the *same* value back is a no-op: the system detects "no change"
    // and does NOT re-apply the setting, so the broken (lost) visual state
    // persists even though the registry value is correct.
    //
    // To make the system actually RE-READ and RE-APPLY, we must force a real
    // state transition: switch to a different value, then switch back.
    const UINT toggle = (trails == 0) ? 1 : 0;   // guaranteed != target

    // 1) Force a state change (target <-> opposite). This tears down and
    //    rebuilds the cursor-trail rendering pipeline inside the kernel,
    //    which is what was lost when the display driver reset the context.
    SystemParametersInfoW(SPI_SETMOUSETRAILS, toggle, nullptr, SPIF_SENDCHANGE);

    // 2) Re-apply the user's real setting. SPIF_UPDATEINIFILE persists it
    //    back to the registry (in case the driver wiped it) and
    //    SPIF_SENDCHANGE broadcasts WM_SETTINGCHANGE so every application
    //    re-reads the new value.
    SystemParametersInfoW(SPI_SETMOUSETRAILS, static_cast<UINT>(trails),
                          nullptr, SPIF_UPDATEINIFILE | SPIF_SENDCHANGE);

    // 3) Make the system reload the cursor scheme from the registry
    //    (Control Panel\Cursors) so the whole mouse configuration is
    //    re-read and applied end-to-end, not just the trail flag.
    SystemParametersInfoW(SPI_SETCURSORS, 0, nullptr, SPIF_SENDCHANGE);
}

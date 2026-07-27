#include "HiddenWindow.h"
#include "CursorFixer.h"
#include <windows.h>

// ===========================================================================
// CursorSyncKeeper  --  the DAEMON (lightweight)
//
//   This process does exactly ONE thing: keep the software mouse (MPO) enabled
//   by re-applying the fix whenever a display change would otherwise flip the
//   system back to the hardware cursor. It is purely event-driven (0% CPU when
//   idle) and contains NO install / uninstall / registry / scheduled-task code.
//
//   All privileged setup (writing HKLM OverlayTestMode at install time,
//   registering the logon auto-start task, the ARP entry, and teardown on
//   uninstall) lives in the installer wizard (CursorSyncKeeper_Setup.exe) and
//   the control panel (CursorSyncKeeperPanel.exe) via the shared AdminOps
//   module. This daemon is launched only AFTER that setup, already elevated.
// ===========================================================================

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Single instance: the logon task, the installer's immediate launch and a
    // manual start must never stack multiple daemons (each extra instance
    // would double the driver resets -> repeated black flashes).
    HANDLE hMutex = CreateMutexW(nullptr, TRUE,
                                 L"Global\\CursorSyncKeeperDaemon");
    if (!hMutex || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;   // another daemon is already running -- quietly exit

    HiddenWindow window;
    if (!window.Create()) {
        MessageBoxW(nullptr, L"Failed to create message window.",
                    L"CursorSyncKeeper", MB_OK | MB_ICONERROR);
        return 1;
    }

    // One-time restore on startup (in case the setting was already lost
    // before we began listening). Still 0% CPU afterwards.
    CursorFixer::Apply();
    // Suppress the driver-reset aftershock (WM_DISPLAYCHANGE) our own Apply()
    // just caused, so the event loop does not immediately re-trigger a fix.
    window.MarkApplied();

    window.RunMessageLoop();
    ReleaseMutex(hMutex);
    CloseHandle(hMutex);
    return 0;
}

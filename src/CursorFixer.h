#pragma once
#include <windows.h>

// Core fix logic for the "hardware mouse reverts after fullscreen / needs
// reboot to fix" problem.
//
// Root cause: Windows uses a *hardware* cursor (GPU Multiplane Overlay, MPO)
// by default. Some fullscreen apps / display-driver resets flip the system
// back to the hardware cursor and never restore the software path. The only
// reliable fix is to disable the MPO overlay so DWM is forced to compose the
// cursor in software -- which otherwise only takes effect after a reboot.
//
// To re-enable software mouse WITHOUT rebooting we:
//   1. write HKLM\...\DWM\OverlayTestMode = 5  (disable MPO -> software cursor)
//   2. force a mouse-setting state transition so the system re-applies it
//   3. trigger a GPU driver reset (Win+Ctrl+Shift+B) so the driver reloads
//      the new OverlayTestMode and drops the hardware cursor immediately.
class CursorFixer {
public:
    // Persistently enable software mouse by disabling the DWM overlay
    // (MPO). Writes HKLM, so it REQUIRES administrator privileges.
    // Returns true on success.
    static bool EnableSoftwareMouseRegistry();

    // Remove the HKLM value (used by uninstall).
    static void DisableSoftwareMouseRegistry();

    // Full re-apply: ensure software mouse is enabled in the registry, force
    // the system to re-read the mouse setting, and reset the GPU driver so
    // the change takes effect immediately (no reboot).
    static void Apply();

    // Trigger a graphics-driver reset via the Win+Ctrl+Shift+B hotkey so the
    // driver reloads OverlayTestMode without a reboot.
    static void ResetGraphicsDriver();

private:
    CursorFixer() = delete;
};

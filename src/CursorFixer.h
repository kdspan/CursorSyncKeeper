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

    // NON-flashing keep-alive for OverlayTestMode. Re-checks the HKLM value and
    // re-writes it ONLY if it has drifted away from 5. Crucially this does NOT
    // call ResetGraphicsDriver(), so MPO stays disabled (software cursor) across
    // a running game with zero screen flash -- safe to call every few seconds.
    static void EnsureOverlayTestModeAlive();

    // Force the OS-level SOFTWARE cursor by setting MouseTrails to -1 (the
    // sentinel that enables the software cursor path with no visible trail).
    // Persists HKCU\Control Panel\Desktop\MouseTrails = "-1" and pushes a real
    // 0 -> -1 state transition via SPI_SETMOUSETRAILS. This is the part that
    // actually beats stubborn hardware-cursor lock-in (e.g. exclusive-
    // fullscreen games with injectors), which OverlayTestMode alone cannot.
    static bool ForceSoftwareCursorTrails();

    // Undo the -1 sentinel: MouseTrails back to 0 (used by uninstall).
    static void RestoreMouseTrails();

    // True if the runtime MouseTrails state still equals the -1 sentinel.
    // Cheap (one SystemParametersInfo query) -- safe to call frequently.
    static bool TrailsSentinelActive();

    // CHEAP re-fix for the "borderless game reset the mouse setting" case:
    // re-assert the -1 sentinel and reload the cursor scheme, WITHOUT the GPU
    // driver reset. OverlayTestMode is untouched (still in effect), so there
    // is no screen flash at all.
    static void ReassertSoftwareCursor();

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

#pragma once
#include <windows.h>
#include <string>

class HiddenWindow {
public:
    HiddenWindow();
    ~HiddenWindow();

    // Create the invisible message-only window.
    bool Create();

    // Block on the message pump until WM_QUIT.
    void RunMessageLoop();

    // Tear down the window.
    void Destroy();

    // Record that a fix just ran. Any display-change trigger arriving within
    // COOLDOWN_MS afterwards is suppressed, because it is the aftershock of our
    // own ResetGraphicsDriver() (which broadcasts WM_DISPLAYCHANGE), not a
    // genuine external display change. Without this the fix re-triggers itself
    // forever (screen flashes in a loop until the process is killed).
    void MarkApplied();

private:
    static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

    // True if COOLDOWN_MS has elapsed since the last fix. This is the ONLY
    // check done at event time; the real "did the display set change?" test is
    // deferred to the timer callback, after transient jitter has settled.
    bool CooldownElapsed() const;

    // Called when the delay timer fires (jitter has settled). Compares the
    // CURRENT monitor device-name set against the snapshot; returns true (and
    // updates the snapshot) only on a real monitor add/remove. A mobile-HDD
    // plug/unplug whose GPU jitter has already settled compares equal here and
    // is silently dropped -- zero screen flash.
    bool ShouldApplyNow();

    HWND m_hWnd = nullptr;

    // Device-notification handles for display-class interfaces. These let us
    // react ONLY to USB-C monitors / docking-station displays / USB GPUs
    // arriving or leaving, instead of mis-firing on every USB plug (flash
    // drive, mouse, keyboard).
    HDEVNOTIFY m_hDevNotify = nullptr;         // GUID_DEVINTERFACE_MONITOR
    HDEVNOTIFY m_hDevNotifyAdapter = nullptr;  // GUID_DEVINTERFACE_DISPLAY_ADAPTER

    // Tick of the last fix. Triggers within COOLDOWN_MS after it are ignored
    // (they are the aftermath of our own driver reset).
    DWORD m_lastApplyTick = 0;

    // Snapshot of the monitor DEVICE-NAME set (sorted, names only -- geometry
    // and primary-flag are deliberately excluded because they jitter briefly
    // during unrelated USB plug/unplug). A fix only runs if the live set
    // differs from this, i.e. a monitor was really added or removed.
    std::wstring m_lastTopology;

    static constexpr DWORD COOLDOWN_MS = 4000;
};

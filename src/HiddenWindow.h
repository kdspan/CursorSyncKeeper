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
    // CURRENT full topology (device names + geometry + primary flag) against
    // the snapshot; returns true (and updates the snapshot) only on a
    // persistent change: game-exit resolution switch or monitor add/remove.
    // A mobile-HDD plug/unplug whose transient GPU jitter has already settled
    // compares equal here and is silently dropped -- zero screen flash.
    bool ShouldApplyNow();

    // Sentinel verification for BORDERLESS / windowed games, which never
    // change the display mode (no WM_DISPLAYCHANGE) yet still reset the mouse
    // setting back to the hardware path. If MouseTrails != -1 and no
    // fullscreen app currently owns the foreground, cheaply re-asserts the
    // software cursor (no GPU reset -> zero screen flash).
    static void VerifyCursorSentinel();

    // Out-of-context WinEvent callback: any foreground-window switch (game
    // launch/exit/alt-tab) schedules a sentinel verification.
    static void CALLBACK WinEventProc(HWINEVENTHOOK, DWORD event, HWND,
                                      LONG, LONG, DWORD, DWORD);

    HWND m_hWnd = nullptr;

    // Device-notification handles for display-class interfaces. These let us
    // react ONLY to USB-C monitors / docking-station displays / USB GPUs
    // arriving or leaving, instead of mis-firing on every USB plug (flash
    // drive, mouse, keyboard).
    HDEVNOTIFY m_hDevNotify = nullptr;         // GUID_DEVINTERFACE_MONITOR
    HDEVNOTIFY m_hDevNotifyAdapter = nullptr;  // GUID_DEVINTERFACE_DISPLAY_ADAPTER

    // Foreground-switch hook (EVENT_SYSTEM_FOREGROUND). Borderless games do
    // not touch the display mode, so the only reliable moment to notice that
    // they hijacked the cursor path is when they gain/lose the foreground.
    HWINEVENTHOOK m_hWinEventHook = nullptr;

    // Tick of the last fix. Triggers within COOLDOWN_MS after it are ignored
    // (they are the aftermath of our own driver reset).
    DWORD m_lastApplyTick = 0;

    // Snapshot of the FULL display topology (sorted device name + geometry +
    // primary flag). Geometry is required: a game leaving exclusive fullscreen
    // changes resolution on the SAME monitor, which a name-only set would
    // miss. Transient jitter is filtered by the deferred WM_TIMER comparison,
    // not by excluding fields from the snapshot.
    std::wstring m_lastTopology;

    static constexpr DWORD COOLDOWN_MS = 4000;
};

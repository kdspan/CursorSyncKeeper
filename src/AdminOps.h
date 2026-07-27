#pragma once
#include <windows.h>
#include <string>

// ===========================================================================
// AdminOps  --  shared privileged install / uninstall operations.
//
//   This module owns every action that needs administrator rights AND is part
//   of *installing* or *uninstalling* the product:
//
//     • write / remove the HKLM "Uninstall" (ARP) entry
//     • register / remove the logon auto-start scheduled task
//     • stop the running daemon
//     • delete the install directory + log folder
//
//   It is linked ONLY into CursorSyncKeeper_Setup (installer wizard) and
//   CursorSyncKeeperPanel (control panel). The lightweight daemon
//   (CursorSyncKeeper.exe) does NOT link this module -- it only does the
//   runtime software-mouse fix and event monitoring.
// ===========================================================================

namespace AdminOps {

bool IsElevated();

// Re-launch the current executable elevated (UAC prompt) with the given args.
void RelaunchElevated(const std::wstring& params);

// Run a command and wait for it to finish. Returns true if exit code == 0.
bool RunWait(const std::wstring& cmd);

// Launch a command without waiting (used for the long-lived daemon).
void RunNoWait(const std::wstring& cmd);

// Logs to %ProgramData%\CursorSyncKeeper\install.log.
std::wstring LogDir();
void Log(const std::wstring& msg);

// Kill any running CursorSyncKeeper.exe daemon.
void StopDaemon();

// Register a logon auto-start scheduled task that launches <exePath> elevated.
// Returns true on success.
bool InstallScheduledTask(const std::wstring& exePath);

// Remove the scheduled task. Returns true on success (also true if absent).
bool RemoveScheduledTask();

// Write the ARP "Uninstall" entry pointing at the control panel.
bool WriteARP(const std::wstring& dir);

// Remove the ARP "Uninstall" entry.
void RemoveARP();

// True if the product is installed (ARP entry present).
bool IsInstalled();

// Read the install location from the ARP entry (empty if missing).
std::wstring ReadInstallLocation();

// Delete the install directory (and its contents).
void RemoveFiles(const std::wstring& dir);

// Create the standard All-Users Start Menu shortcuts for the program:
//   Start Menu\Programs\CursorSyncKeeper\
//     • "CursorSyncKeeper 控制面板.lnk"  -> CursorSyncKeeperPanel.exe
//     • "卸载 CursorSyncKeeper.lnk"      -> CursorSyncKeeperPanel.exe /uninstall
// (a normal Windows program registers itself this way so it shows up in the
// Start Menu after install). Returns true on success.
bool CreateStartMenuShortcuts(const std::wstring& dir);

// Remove the program's Start Menu folder (called on uninstall).
void RemoveStartMenuShortcuts();

} // namespace AdminOps

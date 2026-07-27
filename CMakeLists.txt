cmake_minimum_required(VERSION 3.10)
project(CursorSyncKeeper)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Copy the application icon next to the build dir so rc.exe (which runs from
# the build directory) can resolve the "appicon.ico" referenced by the .rc
# resource files.
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/src/appicon.ico
               ${CMAKE_CURRENT_BINARY_DIR}/appicon.ico COPYONLY)

# WIN32: build a GUI subsystem executable (no console window)
# The daemon is lightweight: it only does the runtime fix + event monitoring.
# It links ONLY CursorFixer (for Apply) -- NO AdminOps, so it contains no
# install / uninstall / registry / scheduled-task logic.
add_executable(CursorSyncKeeper WIN32
    src/main.cpp
    src/HiddenWindow.cpp
    src/CursorFixer.cpp
    src/Daemon.rc
)

# user32  : window / message pump / SystemParametersInfo / SendInput
# kernel32: GetModuleFileName / registry helpers / CreateProcess
# advapi32: RegOpenKeyEx / RegSetValueEx (HKLM write) / CheckTokenMembership
# shell32 : ShellExecuteEx (UAC elevation) / SHGetFolderPath
target_link_libraries(CursorSyncKeeper user32 kernel32 advapi32 shell32)

# Installer wizard (install / reinstall, selectable location). It owns the
# install-time privileged work -- writing HKLM + registering the scheduled
# task -- via the shared AdminOps module, and links CursorFixer to persist the
# software-mouse registry value.
# Listing setup.manifest as a source makes CMake embed it (via mt.exe) as the
# ONLY manifest, so it is not silently replaced by CMake's default asInvoker
# manifest (which previously broke every privileged write with ACCESS_DENIED).
add_executable(CursorSyncKeeper_Setup WIN32
    src/Setup.cpp
    src/AdminOps.cpp
    src/Setup.rc
    src/setup.manifest
    src/CursorFixer.cpp
)
target_link_libraries(CursorSyncKeeper_Setup user32 kernel32 advapi32 shell32 ole32)
target_link_options(CursorSyncKeeper_Setup PRIVATE /MANIFEST:NO)

# The Setup wizard embeds CursorSyncKeeper.exe and CursorSyncKeeperPanel.exe as
# binary resources (self-extracting installer). Make sure those two targets are
# fully linked BEFORE Setup.rc is compiled, so rc.exe can find them in the build
# directory at resource-compile time.
add_dependencies(CursorSyncKeeper_Setup CursorSyncKeeper CursorSyncKeeperPanel)

# Control Panel program: single fix + uninstall (a separate program from both
# the daemon and the installer wizard). It also uses AdminOps for teardown and
# CursorFixer for the one-shot fix, and embeds a requireAdministrator manifest.
add_executable(CursorSyncKeeperPanel WIN32
    src/Panel.cpp
    src/AdminOps.cpp
    src/Panel.rc
    src/panel.manifest
    src/CursorFixer.cpp
)
target_link_libraries(CursorSyncKeeperPanel user32 kernel32 advapi32 shell32 ole32)
target_link_options(CursorSyncKeeperPanel PRIVATE /MANIFEST:NO)

if(MSVC)
    # Source files contain Chinese UI strings -> compile as UTF-8.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")

    # Smaller, release-optimized binary; strip unnecessary stuff
    target_link_options(CursorSyncKeeper PRIVATE /ENTRY:WinMainCRTStartup)
    set_target_properties(CursorSyncKeeper PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF")
endif()

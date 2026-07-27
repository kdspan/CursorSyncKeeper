cmake_minimum_required(VERSION 3.10)
project(CursorSyncKeeper)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WIN32: build a GUI subsystem executable (no console window)
add_executable(CursorSyncKeeper WIN32
    src/main.cpp
    src/HiddenWindow.cpp
    src/CursorFixer.cpp
)

# user32  : window / message pump / SystemParametersInfo / SendInput
# kernel32: GetModuleFileName / registry helpers / CreateProcess
# advapi32: RegOpenKeyEx / RegSetValueEx (HKLM write) / CheckTokenMembership
# shell32 : ShellExecuteEx (UAC elevation) / SHGetFolderPath
target_link_libraries(CursorSyncKeeper user32 kernel32 advapi32 shell32)

# Installer package (install / reinstall / uninstall GUI + ARP entry).
# Listing setup.manifest as a source makes CMake embed it (via mt.exe) as the
# ONLY manifest, so it is not silently replaced by CMake's default asInvoker
# manifest (which previously broke every privileged write with ACCESS_DENIED).
add_executable(CursorSyncKeeper_Setup WIN32
    src/Setup.cpp
    src/Setup.rc
    src/setup.manifest
)
target_link_libraries(CursorSyncKeeper_Setup user32 kernel32 advapi32 shell32)

# Stop the linker from emitting its own default asInvoker manifest; we supply
# the only manifest (setup.manifest, requireAdministrator) via the .manifest
# source above.
target_link_options(CursorSyncKeeper_Setup PRIVATE /MANIFEST:NO)

if(MSVC)
    # Source files contain Chinese UI strings -> compile as UTF-8.
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /utf-8")

    # Smaller, release-optimized binary; strip unnecessary stuff
    target_link_options(CursorSyncKeeper PRIVATE /ENTRY:WinMainCRTStartup)
    set_target_properties(CursorSyncKeeper PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF")
endif()

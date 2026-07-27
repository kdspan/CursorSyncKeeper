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

# user32  : window / message pump / SystemParametersInfo
# kernel32: GetModuleFileName / registry helpers
# advapi32: RegOpenKeyEx / RegSetValueEx / RegQueryValueEx
target_link_libraries(CursorSyncKeeper user32 kernel32 advapi32)

if(MSVC)
    # Smaller, release-optimized binary; strip unnecessary stuff
    target_link_options(CursorSyncKeeper PRIVATE /ENTRY:WinMainCRTStartup)
    set_target_properties(CursorSyncKeeper PROPERTIES
        LINK_FLAGS_RELEASE "/SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF")
endif()

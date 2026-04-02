cmake_minimum_required(VERSION 3.21)

project(IconMover VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "IconMover requires Windows.")
endif()

if(NOT MSVC)
    message(FATAL_ERROR "IconMover requires the MSVC toolchain.")
endif()

if(CMAKE_SIZEOF_VOID_P AND NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "IconMover must be configured for x64.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(IconMover WIN32
    src/main.cpp
    src/app/App.cpp
    src/app/App.h
    src/autostart/AutoStartManager.cpp
    src/autostart/AutoStartManager.h
    src/app/MessageWindow.cpp
    src/app/MessageWindow.h
    src/config/ConfigManager.cpp
    src/config/ConfigManager.h
    src/desktop/DesktopViewAccessor.cpp
    src/desktop/DesktopViewAccessor.h
    src/desktop/DesktopIconMover.cpp
    src/desktop/DesktopIconMover.h
    src/desktop/IconLayoutCalculator.cpp
    src/desktop/IconLayoutCalculator.h
    src/explorer/ExplorerWatcher.cpp
    src/explorer/ExplorerWatcher.h
    src/monitor/MonitorManager.cpp
    src/monitor/MonitorManager.h
    src/monitor/MonitorWorkArea.cpp
    src/monitor/MonitorWorkArea.h
    src/tray/TrayManager.cpp
    src/tray/TrayManager.h
    src/core/AppState.h
    src/core/Constants.h
    src/core/Models.h
    src/common/Logger.cpp
    src/common/Logger.h
    src/common/Win32Error.cpp
    src/common/Win32Error.h
    src/resource/app.rc
    src/resource/resource.h
    include/IconMover/Version.h
)

target_include_directories(IconMover
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_definitions(IconMover
    PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        STRICT
        WIN32_LEAN_AND_MEAN
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
)

target_compile_options(IconMover
    PRIVATE
        /W4
        /permissive-
        /utf-8
        /EHsc
        /Zc:__cplusplus
)

target_link_libraries(IconMover
    PRIVATE
        shell32
        user32
        gdi32
        comctl32
        ole32
        uuid
)

set_target_properties(IconMover PROPERTIES
    OUTPUT_NAME "IconMover"
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
)

set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT IconMover)

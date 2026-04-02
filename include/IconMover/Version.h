#pragma once

#define ICON_MOVER_VERSION_MAJOR 0
#define ICON_MOVER_VERSION_MINOR 1
#define ICON_MOVER_VERSION_PATCH 0
#define ICON_MOVER_VERSION_BUILD 0

#define ICON_MOVER_FILE_VERSION \
    ICON_MOVER_VERSION_MAJOR, ICON_MOVER_VERSION_MINOR, ICON_MOVER_VERSION_PATCH, ICON_MOVER_VERSION_BUILD
#define ICON_MOVER_PRODUCT_VERSION ICON_MOVER_FILE_VERSION

#define ICON_MOVER_VERSION_STRING "0.1.0.0"
#define ICON_MOVER_PRODUCT_NAME "IconMover"
#define ICON_MOVER_COMPANY_NAME "SnapSide"
#define ICON_MOVER_FILE_DESCRIPTION "SnapSide desktop icon mover for Windows"
#define ICON_MOVER_INTERNAL_NAME "IconMover"
#define ICON_MOVER_ORIGINAL_FILENAME "IconMover.exe"
#define ICON_MOVER_COPYRIGHT "Copyright (C) 2026 SnapSide"

#ifndef RC_INVOKED
namespace icon_mover
{
inline constexpr wchar_t kApplicationName[] = L"IconMover";
inline constexpr wchar_t kMainWindowClassName[] = L"IconMover.MainWindow";
inline constexpr wchar_t kCompanyName[] = L"SnapSide";
inline constexpr wchar_t kFileDescription[] = L"SnapSide desktop icon mover for Windows";
inline constexpr wchar_t kProductVersion[] = L"0.1.0.0";
}  // namespace icon_mover
#endif

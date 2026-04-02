#pragma once

#include <string>

#include <windows.h>

namespace icon_mover
{
std::wstring FormatWin32ErrorMessage(DWORD error_code);
std::wstring DescribeWin32Error(DWORD error_code);
std::wstring DescribeLastWin32Error();
}  // namespace icon_mover


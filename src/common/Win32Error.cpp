#include "common/Win32Error.h"

#include <cwctype>
#include <string>

namespace icon_mover
{
namespace
{
std::wstring TrimTrailingWhitespace(std::wstring value)
{
    while (!value.empty() && iswspace(value.back()) != 0)
    {
        value.pop_back();
    }

    return value;
}
}  // namespace

std::wstring FormatWin32ErrorMessage(DWORD error_code)
{
    LPWSTR buffer = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const DWORD length = ::FormatMessageW(
        flags,
        nullptr,
        error_code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        reinterpret_cast<LPWSTR>(&buffer),
        0,
        nullptr);

    if (length == 0 || buffer == nullptr)
    {
        return L"Unknown error";
    }

    std::wstring message(buffer, length);
    ::LocalFree(buffer);
    return TrimTrailingWhitespace(std::move(message));
}

std::wstring DescribeWin32Error(DWORD error_code)
{
    return L"error_code=" + std::to_wstring(error_code) + L" error_message=\"" + FormatWin32ErrorMessage(error_code) + L"\"";
}

std::wstring DescribeLastWin32Error()
{
    return DescribeWin32Error(::GetLastError());
}
}  // namespace icon_mover


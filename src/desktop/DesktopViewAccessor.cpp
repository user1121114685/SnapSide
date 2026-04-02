#include "desktop/DesktopViewAccessor.h"

#include <CommCtrl.h>
#include <ShlGuid.h>
#include <ShObjIdl.h>
#include <exdisp.h>
#include <propkey.h>
#include <servprov.h>

#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

#include "common/Logger.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kDesktopViewPhase[] = L"desktop_view";
constexpr wchar_t kDesktopListViewClassName[] = L"SysListView32";

template <typename InterfaceT>
void SafeRelease(InterfaceT*& pointer) noexcept
{
    if (pointer != nullptr)
    {
        pointer->Release();
        pointer = nullptr;
    }
}

std::wstring HResultToString(HRESULT result)
{
    std::wstringstream stream;
    stream << L"0x" << std::uppercase << std::hex << static_cast<unsigned long>(result);
    return stream.str();
}

DWORD HResultToWin32Error(HRESULT result, DWORD fallback = ERROR_GEN_FAILURE) noexcept
{
    if (HRESULT_FACILITY(result) == FACILITY_WIN32 && HRESULT_CODE(result) != 0)
    {
        return static_cast<DWORD>(HRESULT_CODE(result));
    }

    if (result == S_OK || result == S_FALSE)
    {
        return ERROR_SUCCESS;
    }

    return fallback;
}

bool ReportExplorerRefreshRetryable(DWORD error_code, std::wstring_view detail)
{
    ::SetLastError(error_code);

    std::wstring message = L"Explorer desktop shell view is not stable yet; refresh will be retried later. detail=\"";
    message.append(detail.data(), detail.size());
    message += L"\"";
    message += L" last_error=";
    message += std::to_wstring(error_code);
    Logger::Warning(kDesktopViewPhase, message);
    return false;
}

bool WindowClassEquals(HWND window, std::wstring_view expected_class) noexcept
{
    if (window == nullptr || expected_class.empty())
    {
        return false;
    }

    wchar_t class_name[64] = {};
    const int copied = ::GetClassNameW(window, class_name, static_cast<int>(sizeof(class_name) / sizeof(class_name[0])));
    if (copied <= 0)
    {
        return false;
    }

    return ::CompareStringOrdinal(
               class_name,
               copied,
               expected_class.data(),
               static_cast<int>(expected_class.size()),
               TRUE) == CSTR_EQUAL;
}

bool ResolveFolderViewItemPidl(IFolderView2* folder_view, int icon_index, PITEMID_CHILD& item_pidl)
{
    item_pidl = nullptr;

    if (folder_view == nullptr)
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Desktop folder view is unavailable while resolving an item PIDL.");
        return false;
    }

    const HRESULT result = folder_view->Item(icon_index, &item_pidl);
    if (FAILED(result) || item_pidl == nullptr)
    {
        ::SetLastError(HResultToWin32Error(result, ERROR_NOT_FOUND));
        Logger::Error(
            kDesktopViewPhase,
            L"IFolderView2::Item failed while resolving a desktop icon PIDL. icon_index=" + std::to_wstring(icon_index) +
                L" hr=" + HResultToString(result));
        ::CoTaskMemFree(item_pidl);
        item_pidl = nullptr;
        return false;
    }

    return true;
}
}  // namespace

bool DesktopViewAccessor::IsTransientRefreshError(DWORD error_code) noexcept
{
    switch (error_code)
    {
    case ERROR_INVALID_WINDOW_HANDLE:
    case ERROR_NOT_READY:
    case ERROR_BUSY:
        return true;

    default:
        return false;
    }
}

DesktopViewAccessor::~DesktopViewAccessor()
{
    Reset();
    ReleaseDesktopContext();

    if (com_initialized_)
    {
        ::CoUninitialize();
        com_initialized_ = false;
    }
}

bool DesktopViewAccessor::Refresh(HWND desktop_list_view_window)
{
    Reset();

    if (!ValidateDesktopHandle(desktop_list_view_window))
    {
        return false;
    }

    if (!EnsureComInitialized() || !EnsureDesktopContext())
    {
        return false;
    }

    IShellWindows* shell_windows = nullptr;
    HRESULT result = ::CoCreateInstance(
        CLSID_ShellWindows,
        nullptr,
        CLSCTX_ALL,
        IID_PPV_ARGS(&shell_windows));
    if (FAILED(result))
    {
        Logger::Error(
            kDesktopViewPhase,
            L"CoCreateInstance(CLSID_ShellWindows) failed. hr=" + HResultToString(result));
        return false;
    }

    VARIANT desktop_location = {};
    desktop_location.vt = VT_I4;
    desktop_location.lVal = CSIDL_DESKTOP;

    VARIANT empty = {};
    long shell_window_handle = 0;
    IDispatch* desktop_dispatch = nullptr;
    result = shell_windows->FindWindowSW(
        &desktop_location,
        &empty,
        SWC_DESKTOP,
        &shell_window_handle,
        SWFO_NEEDDISPATCH,
        &desktop_dispatch);
    SafeRelease(shell_windows);

    if (FAILED(result))
    {
        Logger::Error(
            kDesktopViewPhase,
            L"IShellWindows::FindWindowSW failed to resolve the desktop window. hr=" + HResultToString(result));
        SafeRelease(desktop_dispatch);
        return false;
    }

    if (desktop_dispatch == nullptr)
    {
        SafeRelease(desktop_dispatch);
        return ReportExplorerRefreshRetryable(
            ERROR_NOT_READY,
            L"IShellWindows::FindWindowSW did not return the desktop dispatch yet.");
    }

    IServiceProvider* service_provider = nullptr;
    result = desktop_dispatch->QueryInterface(IID_PPV_ARGS(&service_provider));
    SafeRelease(desktop_dispatch);
    if (FAILED(result) || service_provider == nullptr)
    {
        Logger::Error(
            kDesktopViewPhase,
            L"QueryInterface(IServiceProvider) for the desktop dispatch failed. hr=" + HResultToString(result));
        SafeRelease(service_provider);
        return false;
    }

    IShellBrowser* shell_browser = nullptr;
    result = service_provider->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&shell_browser));
    SafeRelease(service_provider);
    if (FAILED(result) || shell_browser == nullptr)
    {
        Logger::Error(
            kDesktopViewPhase,
            L"QueryService(SID_STopLevelBrowser) failed for the desktop window. hr=" + HResultToString(result));
        SafeRelease(shell_browser);
        return false;
    }

    IShellView* shell_view = nullptr;
    result = shell_browser->QueryActiveShellView(&shell_view);
    SafeRelease(shell_browser);
    if (FAILED(result))
    {
        Logger::Error(
            kDesktopViewPhase,
            L"IShellBrowser::QueryActiveShellView failed for the desktop. hr=" + HResultToString(result));
        SafeRelease(shell_view);
        return false;
    }

    if (shell_view == nullptr)
    {
        SafeRelease(shell_view);
        return ReportExplorerRefreshRetryable(
            ERROR_NOT_READY,
            L"IShellBrowser::QueryActiveShellView did not return the active desktop shell view yet.");
    }

    IFolderView2* folder_view = nullptr;
    result = shell_view->QueryInterface(IID_PPV_ARGS(&folder_view));
    if (FAILED(result) || folder_view == nullptr)
    {
        Logger::Error(
            kDesktopViewPhase,
            L"QueryInterface(IFolderView2) failed for the desktop shell view. hr=" + HResultToString(result));
        SafeRelease(shell_view);
        SafeRelease(folder_view);
        return false;
    }

    IPersistFolder2* persist_folder = nullptr;
    result = folder_view->GetFolder(IID_PPV_ARGS(&persist_folder));
    if (FAILED(result) || persist_folder == nullptr)
    {
        Logger::Error(
            kDesktopViewPhase,
            L"IFolderView2::GetFolder(IPersistFolder2) failed for the desktop shell view. hr=" + HResultToString(result));
        SafeRelease(folder_view);
        SafeRelease(shell_view);
        SafeRelease(persist_folder);
        return false;
    }

    PIDLIST_ABSOLUTE current_folder_pidl = nullptr;
    result = persist_folder->GetCurFolder(&current_folder_pidl);
    SafeRelease(persist_folder);
    if (FAILED(result))
    {
        Logger::Error(
            kDesktopViewPhase,
            L"IPersistFolder2::GetCurFolder failed for the desktop shell view. hr=" + HResultToString(result));
        SafeRelease(folder_view);
        SafeRelease(shell_view);
        ::CoTaskMemFree(current_folder_pidl);
        return false;
    }

    if (current_folder_pidl == nullptr)
    {
        SafeRelease(folder_view);
        SafeRelease(shell_view);
        ::CoTaskMemFree(current_folder_pidl);
        return ReportExplorerRefreshRetryable(
            ERROR_BUSY,
            L"IPersistFolder2::GetCurFolder returned no desktop folder PIDL yet.");
    }

    const bool is_desktop_folder = ::ILIsEqual(current_folder_pidl, desktop_folder_pidl_) != FALSE;
    ::CoTaskMemFree(current_folder_pidl);
    if (!is_desktop_folder)
    {
        SafeRelease(folder_view);
        SafeRelease(shell_view);
        return ReportExplorerRefreshRetryable(ERROR_BUSY, L"The active shell view is not bound to the desktop folder yet.");
    }

    HWND shell_view_window = nullptr;
    result = shell_view->GetWindow(&shell_view_window);
    if (FAILED(result))
    {
        Logger::Error(
            kDesktopViewPhase,
            L"IShellView::GetWindow failed for the desktop shell view. hr=" + HResultToString(result));
        SafeRelease(folder_view);
        SafeRelease(shell_view);
        return false;
    }

    if (shell_view_window == nullptr)
    {
        SafeRelease(folder_view);
        SafeRelease(shell_view);
        return ReportExplorerRefreshRetryable(
            ERROR_INVALID_WINDOW_HANDLE,
            L"IShellView::GetWindow returned no desktop shell view window yet.");
    }

    HWND resolved_list_view_window = nullptr;
    if (WindowClassEquals(shell_view_window, kDesktopListViewClassName))
    {
        resolved_list_view_window = shell_view_window;
    }
    else
    {
        resolved_list_view_window = ::FindWindowExW(shell_view_window, nullptr, kDesktopListViewClassName, nullptr);
    }

    if (resolved_list_view_window != desktop_list_view_window)
    {
        SafeRelease(folder_view);
        SafeRelease(shell_view);
        return ReportExplorerRefreshRetryable(
            ERROR_BUSY,
            L"The shell desktop view does not match the discovered SysListView32 handle and Explorer is still rebuilding.");
    }

    const LRESULT list_view_icon_count = ::SendMessageW(desktop_list_view_window, LVM_GETITEMCOUNT, 0, 0);
    int folder_view_icon_count = 0;
    result = folder_view->ItemCount(SVGIO_ALLVIEW, &folder_view_icon_count);
    if (FAILED(result))
    {
        Logger::Error(
            kDesktopViewPhase,
            L"IFolderView2::ItemCount failed for the desktop view. hr=" + HResultToString(result));
        SafeRelease(folder_view);
        SafeRelease(shell_view);
        return false;
    }

    if (list_view_icon_count != static_cast<LRESULT>(folder_view_icon_count))
    {
        Logger::Warning(
            kDesktopViewPhase,
            L"Desktop ListView and IFolderView2 reported different icon counts; continuing with the ListView count. list_view_count=" +
                std::to_wstring(static_cast<long long>(list_view_icon_count)) + L" folder_view_count=" +
                std::to_wstring(folder_view_icon_count));
    }

    desktop_shell_view_ = shell_view;
    folder_view_ = folder_view;
    desktop_list_view_window_ = desktop_list_view_window;
    return true;
}

void DesktopViewAccessor::Reset() noexcept
{
    ReleaseViewInterfaces();
    desktop_list_view_window_ = nullptr;
}

bool DesktopViewAccessor::IsReady() const noexcept
{
    return HasUsableContext();
}

bool DesktopViewAccessor::GetIconCount(int& icon_count) const
{
    icon_count = 0;

    if (!HasUsableContext())
    {
        return false;
    }

    const LRESULT result = ::SendMessageW(desktop_list_view_window_, LVM_GETITEMCOUNT, 0, 0);
    if (result < 0)
    {
        Logger::Error(kDesktopViewPhase, L"LVM_GETITEMCOUNT returned an invalid result for the desktop ListView.");
        return false;
    }

    icon_count = static_cast<int>(result);
    return true;
}

bool DesktopViewAccessor::GetIconPosition(int icon_index, POINT& position) const
{
    position = {};

    if (!HasUsableContext())
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Cannot query a desktop icon position because the desktop view accessor is not ready.");
        return false;
    }

    if (icon_index < 0)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Desktop icon index cannot be negative when querying its current position.");
        return false;
    }

    PITEMID_CHILD item_pidl = nullptr;
    if (!ResolveFolderViewItemPidl(folder_view_, icon_index, item_pidl))
    {
        return false;
    }

    const HRESULT result = folder_view_->GetItemPosition(item_pidl, &position);
    ::CoTaskMemFree(item_pidl);
    if (FAILED(result))
    {
        ::SetLastError(HResultToWin32Error(result));
        Logger::Error(
            kDesktopViewPhase,
            L"IFolderView2::GetItemPosition failed while reading a desktop icon position. icon_index=" +
                std::to_wstring(icon_index) + L" hr=" + HResultToString(result));
        return false;
    }

    return true;
}

bool DesktopViewAccessor::GetItemTypeText(int icon_index, std::wstring& item_type_text) const
{
    item_type_text.clear();

    if (!HasUsableContext())
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Cannot query a desktop item type because the desktop view accessor is not ready.");
        return false;
    }

    if (icon_index < 0)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Desktop icon index cannot be negative when querying its item type.");
        return false;
    }

    IShellFolder2* shell_folder2 = nullptr;
    HRESULT result = desktop_folder_->QueryInterface(IID_PPV_ARGS(&shell_folder2));
    if (FAILED(result) || shell_folder2 == nullptr)
    {
        ::SetLastError(HResultToWin32Error(result));
        Logger::Error(
            kDesktopViewPhase,
            L"QueryInterface(IShellFolder2) failed while querying a desktop item type. hr=" + HResultToString(result));
        SafeRelease(shell_folder2);
        return false;
    }

    PITEMID_CHILD item_pidl = nullptr;
    if (!ResolveFolderViewItemPidl(folder_view_, icon_index, item_pidl))
    {
        SafeRelease(shell_folder2);
        return false;
    }

    VARIANT value = {};
    ::VariantInit(&value);
    result = shell_folder2->GetDetailsEx(item_pidl, &PKEY_ItemTypeText, &value);
    SafeRelease(shell_folder2);
    ::CoTaskMemFree(item_pidl);

    if (FAILED(result))
    {
        ::SetLastError(HResultToWin32Error(result));
        Logger::Error(
            kDesktopViewPhase,
            L"IShellFolder2::GetDetailsEx failed while querying a desktop item type. icon_index=" +
                std::to_wstring(icon_index) + L" hr=" + HResultToString(result));
        return false;
    }

    VARIANT string_value = {};
    ::VariantInit(&string_value);
    if (SUCCEEDED(::VariantChangeType(&string_value, &value, 0, VT_BSTR)) && string_value.bstrVal != nullptr)
    {
        item_type_text.assign(string_value.bstrVal, SysStringLen(string_value.bstrVal));
    }

    ::VariantClear(&string_value);
    ::VariantClear(&value);
    return true;
}

bool DesktopViewAccessor::GetItemDisplayName(int icon_index, std::wstring& display_name) const
{
    display_name.clear();

    if (!HasUsableContext())
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Cannot query a desktop item name because the desktop view accessor is not ready.");
        return false;
    }

    if (icon_index < 0)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Desktop icon index cannot be negative when querying its display name.");
        return false;
    }

    PITEMID_CHILD item_pidl = nullptr;
    if (!ResolveFolderViewItemPidl(folder_view_, icon_index, item_pidl))
    {
        return false;
    }

    PIDLIST_ABSOLUTE absolute_pidl = ::ILCombine(desktop_folder_pidl_, item_pidl);
    ::CoTaskMemFree(item_pidl);
    if (absolute_pidl == nullptr)
    {
        ::SetLastError(ERROR_OUTOFMEMORY);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"ILCombine failed while building an absolute desktop item PIDL.");
        return false;
    }

    PWSTR name = nullptr;
    const HRESULT result = ::SHGetNameFromIDList(absolute_pidl, SIGDN_NORMALDISPLAY, &name);
    ::CoTaskMemFree(absolute_pidl);
    if (FAILED(result))
    {
        ::SetLastError(HResultToWin32Error(result));
        Logger::Error(
            kDesktopViewPhase,
            L"SHGetNameFromIDList failed while querying a desktop item display name. icon_index=" +
                std::to_wstring(icon_index) + L" hr=" + HResultToString(result));
        ::CoTaskMemFree(name);
        return false;
    }

    if (name != nullptr)
    {
        display_name.assign(name);
        ::CoTaskMemFree(name);
    }

    return true;
}

bool DesktopViewAccessor::GetItemParsingName(int icon_index, std::wstring& parsing_name) const
{
    parsing_name.clear();

    if (!HasUsableContext())
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Cannot query a desktop item parsing name because the desktop view accessor is not ready.");
        return false;
    }

    if (icon_index < 0)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Desktop icon index cannot be negative when querying its parsing name.");
        return false;
    }

    PITEMID_CHILD item_pidl = nullptr;
    if (!ResolveFolderViewItemPidl(folder_view_, icon_index, item_pidl))
    {
        return false;
    }

    PIDLIST_ABSOLUTE absolute_pidl = ::ILCombine(desktop_folder_pidl_, item_pidl);
    ::CoTaskMemFree(item_pidl);
    if (absolute_pidl == nullptr)
    {
        ::SetLastError(ERROR_OUTOFMEMORY);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"ILCombine failed while building an absolute desktop item PIDL for the parsing name.");
        return false;
    }

    PWSTR name = nullptr;
    const HRESULT result = ::SHGetNameFromIDList(absolute_pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name);
    ::CoTaskMemFree(absolute_pidl);
    if (FAILED(result))
    {
        ::SetLastError(HResultToWin32Error(result));
        Logger::Error(
            kDesktopViewPhase,
            L"SHGetNameFromIDList failed while querying a desktop item parsing name. icon_index=" +
                std::to_wstring(icon_index) + L" hr=" + HResultToString(result));
        ::CoTaskMemFree(name);
        return false;
    }

    if (name != nullptr)
    {
        parsing_name.assign(name);
        ::CoTaskMemFree(name);
    }

    return true;
}

bool DesktopViewAccessor::ForEachIconIndex(const IconIndexVisitor& visitor) const
{
    if (!visitor)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Cannot enumerate desktop icon indexes with an empty visitor.");
        return false;
    }

    int icon_count = 0;
    if (!GetIconCount(icon_count))
    {
        return false;
    }

    for (int icon_index = 0; icon_index < icon_count; ++icon_index)
    {
        if (!visitor(icon_index))
        {
            break;
        }
    }

    return true;
}

bool DesktopViewAccessor::EnsureComInitialized()
{
    if (com_initialized_)
    {
        return true;
    }

    const HRESULT result = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(result))
    {
        Logger::Error(kDesktopViewPhase, L"CoInitializeEx failed for DesktopViewAccessor. hr=" + HResultToString(result));
        return false;
    }

    com_initialized_ = true;
    return true;
}

bool DesktopViewAccessor::EnsureDesktopContext()
{
    if (desktop_folder_ != nullptr && desktop_folder_pidl_ != nullptr)
    {
        return true;
    }

    ReleaseDesktopContext();

    HRESULT result = ::SHGetDesktopFolder(&desktop_folder_);
    if (FAILED(result) || desktop_folder_ == nullptr)
    {
        Logger::Error(kDesktopViewPhase, L"SHGetDesktopFolder failed. hr=" + HResultToString(result));
        SafeRelease(desktop_folder_);
        return false;
    }

    result = ::SHGetIDListFromObject(desktop_folder_, &desktop_folder_pidl_);
    if (FAILED(result) || desktop_folder_pidl_ == nullptr)
    {
        Logger::Error(
            kDesktopViewPhase,
            L"SHGetIDListFromObject failed while capturing the desktop folder context. hr=" + HResultToString(result));
        ReleaseDesktopContext();
        return false;
    }

    return true;
}

bool DesktopViewAccessor::HasUsableContext() const noexcept
{
    return desktop_shell_view_ != nullptr &&
           folder_view_ != nullptr &&
           desktop_list_view_window_ != nullptr &&
           ValidateDesktopHandle(desktop_list_view_window_);
}

bool DesktopViewAccessor::ValidateDesktopHandle(HWND desktop_list_view_window) const
{
    if (desktop_list_view_window == nullptr || ::IsWindow(desktop_list_view_window) == FALSE)
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Desktop ListView handle is unavailable.");
        return false;
    }

    if (!WindowClassEquals(desktop_list_view_window, kDesktopListViewClassName))
    {
        ::SetLastError(ERROR_INVALID_WINDOW_HANDLE);
        Logger::LogLastError(LogLevel::Error, kDesktopViewPhase, L"Desktop ListView handle does not reference SysListView32.");
        return false;
    }

    return true;
}

void DesktopViewAccessor::ReleaseViewInterfaces() noexcept
{
    SafeRelease(folder_view_);
    SafeRelease(desktop_shell_view_);
}

void DesktopViewAccessor::ReleaseDesktopContext() noexcept
{
    SafeRelease(desktop_folder_);

    if (desktop_folder_pidl_ != nullptr)
    {
        ::CoTaskMemFree(desktop_folder_pidl_);
        desktop_folder_pidl_ = nullptr;
    }
}
}  // namespace icon_mover

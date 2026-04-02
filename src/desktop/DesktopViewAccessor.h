#pragma once

#include <functional>

#include <windows.h>
#include <shlobj.h>

namespace icon_mover
{
class DesktopViewAccessor final
{
public:
    using IconIndexVisitor = std::function<bool(int icon_index)>;

    DesktopViewAccessor() noexcept = default;
    ~DesktopViewAccessor();

    DesktopViewAccessor(const DesktopViewAccessor&) = delete;
    DesktopViewAccessor& operator=(const DesktopViewAccessor&) = delete;

    static bool IsTransientRefreshError(DWORD error_code) noexcept;

    bool Refresh(HWND desktop_list_view_window);
    void Reset() noexcept;

    bool IsReady() const noexcept;
    bool GetIconCount(int& icon_count) const;
    bool ForEachIconIndex(const IconIndexVisitor& visitor) const;

private:
    bool EnsureComInitialized();
    bool EnsureDesktopContext();
    bool HasUsableContext() const noexcept;
    bool ValidateDesktopHandle(HWND desktop_list_view_window) const;
    void ReleaseViewInterfaces() noexcept;
    void ReleaseDesktopContext() noexcept;

    bool com_initialized_ = false;
    IShellFolder* desktop_folder_ = nullptr;
    PIDLIST_ABSOLUTE desktop_folder_pidl_ = nullptr;
    IShellView* desktop_shell_view_ = nullptr;
    IFolderView2* folder_view_ = nullptr;
    HWND desktop_list_view_window_ = nullptr;
};
}  // namespace icon_mover

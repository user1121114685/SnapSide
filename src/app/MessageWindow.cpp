#include "app/MessageWindow.h"

#include <windows.h>

#include "IconMover/Version.h"

namespace icon_mover
{
MessageWindow::~MessageWindow()
{
    Destroy();
}

bool MessageWindow::Create(HINSTANCE instance, MessageWindowHost* host)
{
    if (window_ != nullptr)
    {
        return true;
    }

    if (instance == nullptr || host == nullptr)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        return false;
    }

    instance_ = instance;
    host_ = host;

    if (!RegisterWindowClass())
    {
        return false;
    }

    window_ = ::CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kMainWindowClassName,
        kApplicationName,
        WS_OVERLAPPED,
        0,
        0,
        0,
        0,
        nullptr,
        nullptr,
        instance_,
        this);

    if (window_ == nullptr)
    {
        const DWORD error_code = ::GetLastError();
        UnregisterWindowClass();
        host_ = nullptr;
        instance_ = nullptr;
        ::SetLastError(error_code);
        return false;
    }

    return true;
}

void MessageWindow::Destroy() noexcept
{
    const HWND window = window_;
    if (window != nullptr)
    {
        ::DestroyWindow(window);
        return;
    }

    UnregisterWindowClass();
    host_ = nullptr;
    instance_ = nullptr;
}

HWND MessageWindow::Handle() const noexcept
{
    return window_;
}

bool MessageWindow::RegisterWindowClass()
{
    if (window_class_ != 0)
    {
        return true;
    }

    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &MessageWindow::WindowProc;
    window_class.hInstance = instance_;
    window_class.lpszClassName = kMainWindowClassName;

    window_class_ = ::RegisterClassExW(&window_class);
    if (window_class_ == 0)
    {
        const DWORD error_code = ::GetLastError();
        if (error_code != ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }

        window_class_ = 1;
    }

    return true;
}

void MessageWindow::UnregisterWindowClass() noexcept
{
    if (window_class_ == 0 || instance_ == nullptr)
    {
        return;
    }

    ::UnregisterClassW(kMainWindowClassName, instance_);
    window_class_ = 0;
}

LRESULT MessageWindow::DispatchMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    const std::optional<LRESULT> host_result =
        host_ != nullptr ? host_->OnMessageWindowMessage(window, message, w_param, l_param) : std::nullopt;

    const LRESULT result =
        host_result.has_value() ? *host_result : ::DefWindowProcW(window, message, w_param, l_param);

    if (message == WM_NCDESTROY)
    {
        ::SetWindowLongPtrW(window, GWLP_USERDATA, 0);
        window_ = nullptr;
        host_ = nullptr;
        UnregisterWindowClass();
        instance_ = nullptr;
    }

    return result;
}

LRESULT CALLBACK MessageWindow::WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    MessageWindow* message_window = nullptr;

    if (message == WM_NCCREATE)
    {
        const auto* create_struct = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        message_window = static_cast<MessageWindow*>(create_struct->lpCreateParams);
        if (message_window != nullptr)
        {
            message_window->window_ = window;
            ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(message_window));
        }
    }
    else
    {
        message_window = reinterpret_cast<MessageWindow*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    }

    if (message_window != nullptr)
    {
        return message_window->DispatchMessage(window, message, w_param, l_param);
    }

    return ::DefWindowProcW(window, message, w_param, l_param);
}
}  // namespace icon_mover


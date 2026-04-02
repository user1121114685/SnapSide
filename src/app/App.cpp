#include "app/App.h"

#include <cstdlib>
#include <commctrl.h>

#include "IconMover/Version.h"

namespace icon_mover
{
App::App(HINSTANCE instance) noexcept
    : instance_(instance),
      window_(nullptr),
      window_class_(0)
{
}

int App::Run()
{
    if (!Initialize())
    {
        return EXIT_FAILURE;
    }

    return MessageLoop();
}

bool App::Initialize()
{
    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;

    InitCommonControlsEx(&controls);

    return RegisterWindowClass() && CreateMainWindow();
}

bool App::RegisterWindowClass()
{
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &App::WindowProc;
    window_class.hInstance = instance_;
    window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kMainWindowClassName;

    window_class_ = RegisterClassExW(&window_class);
    return window_class_ != 0;
}

bool App::CreateMainWindow()
{
    window_ = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kMainWindowClassName,
        kApplicationName,
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT,
        CW_USEDEFAULT,
        640,
        480,
        nullptr,
        nullptr,
        instance_,
        this);

    if (window_ == nullptr)
    {
        return false;
    }

    ShowWindow(window_, SW_HIDE);
    return true;
}

int App::MessageLoop() const
{
    MSG message = {};

    while (true)
    {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result == -1)
        {
            return EXIT_FAILURE;
        }

        if (result == 0)
        {
            return static_cast<int>(message.wParam);
        }

        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    if (message == WM_NCCREATE)
    {
        const auto* create_struct = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        auto* app = static_cast<App*>(create_struct->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    auto* app = reinterpret_cast<App*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (app != nullptr)
    {
        return app->HandleMessage(window, message, w_param, l_param);
    }

    return DefWindowProcW(window, message, w_param, l_param);
}

LRESULT App::HandleMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message)
    {
    case WM_CLOSE:
        DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;

    default:
        return DefWindowProcW(window, message, w_param, l_param);
    }
}
}  // namespace icon_mover

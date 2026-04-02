#include "app/App.h"

#include <cstdlib>
#include <string>

#include <commctrl.h>

#include "IconMover/Version.h"
#include "common/Logger.h"

namespace icon_mover
{
namespace
{
constexpr wchar_t kBootstrapPhase[] = L"bootstrap";
constexpr wchar_t kRuntimePhase[] = L"runtime";
constexpr wchar_t kShutdownPhase[] = L"shutdown";
}

App::App(HINSTANCE instance, bool log_test_error) noexcept
    : instance_(instance),
      window_(nullptr),
      window_class_(0),
      log_test_error_(log_test_error)
{
}

int App::Run()
{
    if (!Initialize())
    {
        Logger::Error(kBootstrapPhase, L"Application initialization failed.");
        return EXIT_FAILURE;
    }

    return MessageLoop();
}

bool App::Initialize()
{
    const bool logger_ready = Logger::Initialize();
    if (logger_ready)
    {
        std::wstring message = L"Logger initialized. log_path=\"";
        message += Logger::GetLogFilePath();
        message += L"\"";
        Logger::Info(kBootstrapPhase, message);
    }

    if (log_test_error_)
    {
        ::SetLastError(ERROR_INVALID_PARAMETER);
        Logger::LogLastError(LogLevel::Error, kBootstrapPhase, L"Controlled error path requested via --log-test-error.");
    }

    INITCOMMONCONTROLSEX controls = {};
    controls.dwSize = sizeof(controls);
    controls.dwICC = ICC_STANDARD_CLASSES;

    if (::InitCommonControlsEx(&controls) == FALSE)
    {
        Logger::Warning(kBootstrapPhase, L"InitCommonControlsEx reported failure; continuing startup.");
    }

    if (!RegisterWindowClass())
    {
        return false;
    }

    if (!CreateMainWindow())
    {
        return false;
    }

    Logger::Info(kBootstrapPhase, L"Application bootstrap completed.");
    return true;
}

bool App::RegisterWindowClass()
{
    WNDCLASSEXW window_class = {};
    window_class.cbSize = sizeof(window_class);
    window_class.lpfnWndProc = &App::WindowProc;
    window_class.hInstance = instance_;
    window_class.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = kMainWindowClassName;

    window_class_ = ::RegisterClassExW(&window_class);
    if (window_class_ == 0)
    {
        Logger::LogLastError(LogLevel::Error, kBootstrapPhase, L"RegisterClassExW failed.");
        return false;
    }

    return true;
}

bool App::CreateMainWindow()
{
    window_ = ::CreateWindowExW(
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
        Logger::LogLastError(LogLevel::Error, kBootstrapPhase, L"CreateWindowExW failed.");
        return false;
    }

    ::ShowWindow(window_, SW_HIDE);
    return true;
}

int App::MessageLoop() const
{
    MSG message = {};

    while (true)
    {
        const BOOL result = ::GetMessageW(&message, nullptr, 0, 0);
        if (result == -1)
        {
            Logger::LogLastError(LogLevel::Error, kRuntimePhase, L"GetMessageW failed.");
            return EXIT_FAILURE;
        }

        if (result == 0)
        {
            return static_cast<int>(message.wParam);
        }

        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }
}

LRESULT CALLBACK App::WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    if (message == WM_NCCREATE)
    {
        const auto* create_struct = reinterpret_cast<const CREATESTRUCTW*>(l_param);
        auto* app = static_cast<App*>(create_struct->lpCreateParams);
        ::SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }

    auto* app = reinterpret_cast<App*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
    if (app != nullptr)
    {
        return app->HandleMessage(window, message, w_param, l_param);
    }

    return ::DefWindowProcW(window, message, w_param, l_param);
}

LRESULT App::HandleMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param)
{
    switch (message)
    {
    case WM_CLOSE:
        ::DestroyWindow(window);
        return 0;

    case WM_DESTROY:
        Logger::Info(kShutdownPhase, L"Application shutdown requested.");
        ::PostQuitMessage(0);
        return 0;

    default:
        return ::DefWindowProcW(window, message, w_param, l_param);
    }
}
}  // namespace icon_mover


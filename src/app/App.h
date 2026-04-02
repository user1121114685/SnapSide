#pragma once

#include <windows.h>

#include "core/AppState.h"

namespace icon_mover
{
class App final
{
public:
    App(HINSTANCE instance, bool log_test_error) noexcept;

    int Run();

private:
    bool Initialize();
    bool RegisterWindowClass();
    bool CreateMainWindow();
    int MessageLoop() const;

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);
    LRESULT HandleMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

    HINSTANCE instance_;
    HWND window_;
    ATOM window_class_;
    bool log_test_error_;
    AppState state_;
};
}  // namespace icon_mover

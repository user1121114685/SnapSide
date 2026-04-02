#pragma once

#include <optional>

#include <windows.h>

namespace icon_mover
{
class MessageWindowHost
{
public:
    virtual ~MessageWindowHost() = default;

    virtual std::optional<LRESULT> OnMessageWindowMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param) = 0;
};

class MessageWindow final
{
public:
    MessageWindow() noexcept = default;
    ~MessageWindow();

    MessageWindow(const MessageWindow&) = delete;
    MessageWindow& operator=(const MessageWindow&) = delete;
    MessageWindow(MessageWindow&&) = delete;
    MessageWindow& operator=(MessageWindow&&) = delete;

    bool Create(HINSTANCE instance, MessageWindowHost* host);
    void Destroy() noexcept;

    [[nodiscard]] HWND Handle() const noexcept;

private:
    bool RegisterWindowClass();
    void UnregisterWindowClass() noexcept;
    LRESULT DispatchMessage(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w_param, LPARAM l_param);

    HINSTANCE instance_ = nullptr;
    HWND window_ = nullptr;
    ATOM window_class_ = 0;
    MessageWindowHost* host_ = nullptr;
};
}  // namespace icon_mover


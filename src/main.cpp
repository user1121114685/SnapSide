#include "app/App.h"

#include <string_view>

#include <shellapi.h>

namespace
{
bool HasCommandLineFlag(std::wstring_view flag)
{
    int argc = 0;
    LPWSTR* argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr)
    {
        return false;
    }

    bool found = false;
    for (int index = 1; index < argc; ++index)
    {
        if (::lstrcmpiW(argv[index], flag.data()) == 0)
        {
            found = true;
            break;
        }
    }

    ::LocalFree(argv);
    return found;
}
}  // namespace

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    icon_mover::App app(instance, HasCommandLineFlag(L"--log-test-error"));
    return app.Run();
}


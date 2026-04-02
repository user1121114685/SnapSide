#include "app/App.h"

int WINAPI wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ PWSTR, _In_ int)
{
    icon_mover::App app(instance);
    return app.Run();
}

#include <pch.h>
#include <Headers/Modules/ConsoleGuard.h>

namespace
{
    BOOL WINAPI SwallowBreak(DWORD Event)
    {
        switch (Event)
        {
        case CTRL_C_EVENT:
        case CTRL_BREAK_EVENT:
            Ares::Log("console: ignored a Ctrl+C/Break from the client's console");
            return TRUE;

        case CTRL_CLOSE_EVENT:
            // windows kills it anyway, just log it
            Ares::Log("console: the client's console window was closed");
            return TRUE;

        default:
            return FALSE;
        }
    }
} // namespace

void ConsoleGuard::Install()
{
    if (!SetConsoleCtrlHandler(SwallowBreak, TRUE))
    {
        Ares::Log("console: could not install the break handler (%lu)", GetLastError());
        return;
    }

    wchar_t Buffer[8]{};
    const bool bHideRequested =
        !(GetEnvironmentVariableW(L"ARES_HIDE_CONSOLE", Buffer, 8) > 0 && Buffer[0] == L'0');

    if (bHideRequested && !Ares::Config().Console)
    {
        if (HWND Console = GetConsoleWindow())
        {
            ShowWindow(Console, SW_HIDE);
            Ares::Log("console: break handler installed, client console hidden");
            return;
        }
    }

    Ares::Log("console: break handler installed");
}

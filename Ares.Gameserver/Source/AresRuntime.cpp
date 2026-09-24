#include "pch.h"
#include "Headers/AresRuntime.hpp"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <mutex>

namespace
{
    std::mutex LogMutex;
    std::ofstream LogFile;
    bool LogReady = false;
    const unsigned long long StartTick = GetTickCount64();

    std::wstring EnvW(const wchar_t* Name)
    {
        wchar_t Buffer[2048]{};
        const DWORD Length = GetEnvironmentVariableW(Name, Buffer, 2048);
        if (Length == 0 || Length >= 2048)
            return L"";
        return std::wstring(Buffer, Length);
    }

    std::string EnvA(const wchar_t* Name)
    {
        const std::wstring Wide = EnvW(Name);
        return std::string(Wide.begin(), Wide.end());
    }

    void EnsureLogOpen(const Ares::Settings& Cfg)
    {
        if (LogReady)
            return;
        LogReady = true;

        LogFile.open(Cfg.LogPath, std::ios::trunc);

        if (Cfg.Console && AllocConsole())
        {
            SetConsoleTitleA("Ares gameserver");
            FILE* Dummy = nullptr;
            freopen_s(&Dummy, "CONOUT$", "w", stdout);
            freopen_s(&Dummy, "CONOUT$", "w", stderr);
        }
    }

    void Emit(const char* Line)
    {
        const Ares::Settings& Cfg = Ares::Config();

        std::lock_guard<std::mutex> Lock(LogMutex);
        EnsureLogOpen(Cfg);

        char Stamped[4096];
        snprintf(Stamped, sizeof(Stamped), "[%8.3f] %s", Ares::Seconds(), Line);

        if (LogFile.is_open())
        {
            LogFile << Stamped << std::endl;
            LogFile.flush();
        }
        if (Cfg.Console)
        {
            fputs(Stamped, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
    }
} // namespace

namespace Ares
{
    double Seconds()
    {
        return double(GetTickCount64() - StartTick) / 1000.0;
    }

    const Settings& Config()
    {
        static Settings Cfg = []
        {
            Settings Out;

            Out.MapUrl = EnvW(L"ARES_MAP");
            if (Out.MapUrl.empty())
                Out.MapUrl = L"/Game/Maps/Poveglia/Range"
                             L"?game=/Game/GameModes/ShootingRange/ShootingRangeGameMode.ShootingRangeGameMode_C";

            Out.PawnClass = EnvW(L"ARES_PAWN");
            if (Out.PawnClass.empty())
                Out.PawnClass = L"/Game/Characters/Wushu/Wushu_PC.Wushu_PC_C";

            const std::wstring Loadout = EnvW(L"ARES_LOADOUT");
            for (size_t Start = 0; Start < Loadout.size();)
            {
                const size_t End = Loadout.find(L';', Start);
                const std::wstring Entry = Loadout.substr(Start, End == std::wstring::npos ? End : End - Start);
                if (!Entry.empty())
                    Out.Loadout.push_back(Entry);
                if (End == std::wstring::npos)
                    break;
                Start = End + 1;
            }

            const std::wstring Port = EnvW(L"ARES_PORT");
            if (!Port.empty())
                Out.Port = (unsigned short)_wtoi(Port.c_str());

            const std::wstring Mode = EnvW(L"ARES_MODE");
            Out.Mode = (Mode == L"listen") ? EMode::Listen : EMode::Standalone;

            const std::wstring Delay = EnvW(L"ARES_START_DELAY");
            if (!Delay.empty())
                Out.StartDelay = (float)_wtof(Delay.c_str());

            Out.Console = EnvW(L"ARES_CONSOLE") == L"1";
            Out.PatchEndpoint = EnvW(L"ARES_ENDPOINT_PATCH") != L"0";
            Out.Abilities = EnvW(L"ARES_ABILITIES") != L"0";

            Out.LogPath = EnvA(L"ARES_LOG");
            if (Out.LogPath.empty())
                Out.LogPath = "Ares.log";

            return Out;
        }();

        return Cfg;
    }

    void Log(const char* Format, ...)
    {
        char Buffer[4000];
        va_list Args;
        va_start(Args, Format);
        vsnprintf(Buffer, sizeof(Buffer), Format, Args);
        va_end(Args);
        Emit(Buffer);
    }

    void LogW(const wchar_t* Format, ...)
    {
        wchar_t Wide[2000];
        va_list Args;
        va_start(Args, Format);
        vswprintf(Wide, 2000, Format, Args);
        va_end(Args);

        char Narrow[4000];
        const int Written = WideCharToMultiByte(CP_UTF8, 0, Wide, -1, Narrow, sizeof(Narrow), nullptr, nullptr);
        if (Written > 0)
            Emit(Narrow);
    }
} // namespace Ares

void AresBootLogLine(const char* Line)
{
    Ares::Log("%s", Line);
}

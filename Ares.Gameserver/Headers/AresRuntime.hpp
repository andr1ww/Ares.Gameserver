#pragma once
// env settings, read once
// ARES_MAP travel url, can have ?game= options
// ARES_PAWN pawn class path
// ARES_LOADOUT ; separated weapon class paths
// ARES_PORT udp port, 7777
// ARES_MODE standalone or listen
// ARES_START_DELAY secs before standalone starts the match, 6
// ARES_LOG log file path
// ARES_CONSOLE 1 = console window
// ARES_ENDPOINT_PATCH 0 = dont patch endpoint checks
// ARES_ABILITIES 0 = old empty activation stub
#include <string>
#include <vector>

namespace Ares
{
    enum class EMode
    {
        Standalone,
        Listen,
    };

    struct Settings
    {
        std::wstring MapUrl;
        std::wstring PawnClass;
        std::vector<std::wstring> Loadout;
        unsigned short Port = 7777;
        EMode Mode = EMode::Standalone;
        float StartDelay = 6.0f;
        bool Console = false;
        bool PatchEndpoint = true;
        bool Abilities = true;
        std::string LogPath;
    };

    const Settings& Config();

    double Seconds();

    void Log(const char* Format, ...);

    inline bool Profiling = false;
    inline unsigned long GameThreadId = 0;
    void LogW(const wchar_t* Format, ...);
} // namespace Ares

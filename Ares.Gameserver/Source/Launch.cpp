// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <Headers/Modules/Diagnostics.h>
#include <Headers/Modules/DeveloperConsole.h>
#include <Headers/Modules/LocalSettings.h>
#include <Headers/Modules/OfflineAuthority.h>
#include <Headers/Modules/ShooterGame/LocalCharacterVisibility.h>
#include <Headers/Modules/ShooterGame/LocalForceModules.h>
#include <Headers/Modules/ShooterGame/LocalDeath.h>
#include <Headers/Modules/ShooterGame/LocalBots.h>
#include <Headers/Modules/ShooterGame/MapSurvey.h>
#include <Headers/Modules/ShooterGame/TacticalMap.h>
#include <Headers/Modules/ShooterGame/RoundReferee.h>
#include <Headers/Modules/Freecam.h>
#include <Headers/Modules/ShooterGame/CustomSkins.h>
#include <Headers/Modules/ConsoleGuard.h>
#include <Headers/Modules/PlatformEndpoint.h>
#include <Headers/Modules/VanguardBypass.h>
#include <Headers/Modules/ShooterGame/MatchFlow.h>
#include <Headers/Modules/ShooterGame/AresInventory.h>
#include <Headers/Modules/ScreenCleanup.h>
#include <mutex>
#include <chrono>
#include <atomic>
#include <unordered_set>

static constexpr uintptr_t kOffsetGIsClient = 0x6C67BE9;
static constexpr uintptr_t kOffsetGIsServer = 0x6C67BEA;

static std::atomic<bool> ServerStarted{false};
static std::atomic<uint64_t> GameThreadTicks{0};
static DWORD SimulationThread = 0;
static std::mutex FunctionCacheMutex;
static std::unordered_set<void*> SuppressedFunctions;
static std::unordered_set<void*> CheckedFunctions;

static void* (*ProcessEventOG)(UObject*, UFunction*, void*);

// crash breadcrumbs, minidump cant name the script call
namespace
{
    struct FCrumb { UObject* Obj; UFunction* Function; };
    // script calls run on worker threads too
    thread_local FCrumb Crumbs[64];
    thread_local int CrumbDepth = 0;
    std::atomic<int> CrashReports{0};

    std::string SafeName(UObject* Object)
    {
        return Object ? Object->GetFullName() : "null";
    }

    LONG CALLBACK CrashCrumbs(EXCEPTION_POINTERS* Info)
    {
        if (!Info || Info->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
        if (CrumbDepth <= 0 || CrashReports.fetch_add(1) >= 3) return EXCEPTION_CONTINUE_SEARCH;
        const auto* R = Info->ExceptionRecord;
        HMODULE Module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)R->ExceptionAddress, &Module);
        char ModuleName[MAX_PATH] = "?";
        if (Module) GetModuleFileNameA(Module, ModuleName, MAX_PATH);
        Ares::Log("crash: access violation %s 0x%llx at %s+0x%llx on thread %lu (game thread %lu); script calls in flight, innermost last:",
                  R->NumberParameters > 0 && R->ExceptionInformation[0] ? "writing" : "reading",
                  R->NumberParameters > 1 ? (unsigned long long)R->ExceptionInformation[1] : 0ull,
                  strrchr(ModuleName, '\\') ? strrchr(ModuleName, '\\') + 1 : ModuleName,
                  (unsigned long long)((uintptr_t)R->ExceptionAddress - (uintptr_t)Module), GetCurrentThreadId(), (unsigned long)SimulationThread);
        const int Depth = std::min(CrumbDepth, 64);
        for (int I = std::max(0, Depth - 12); I < Depth; ++I)
            Ares::Log("crash:   %s  on  %s", SafeName(Crumbs[I].Function).c_str(), SafeName(Crumbs[I].Obj).c_str());
        return EXCEPTION_CONTINUE_SEARCH;
    }
}
void InternalServerTryActivateAbility(UAbilitySystemComponent* _this, FGameplayAbilitySpecHandle Handle, bool, FPredictionKey* PredictionKey, void* TriggerEventData);

static void RunServerSequence()
{
    SimulationThread = GetCurrentThreadId();
    Ares::GameThreadId = SimulationThread;
    AddVectoredExceptionHandler(1, CrashCrumbs);
    Freecam::Install();
    const Ares::Settings& Cfg = Ares::Config();

    Ares::Log("server sequence start (game thread)");
    Ares::Log("mode=%s port=%u start-delay=%.1fs",
              Cfg.Mode == Ares::EMode::Standalone ? "standalone" : "listen",
              (unsigned)Cfg.Port,
              Cfg.StartDelay);
    Ares::LogW(L"map=%s", Cfg.MapUrl.c_str());

    for (auto& Initter : Initters)
        Initter();

    // standalone needs listen, dedicated never renders
    const bool bStandalone = Cfg.Mode == Ares::EMode::Standalone;
    *(bool*)(ImageBase + kOffsetGIsClient) = bStandalone; // GIsClient
    *(bool*)(ImageBase + kOffsetGIsServer) = true; // GIsServer
    Ares::Log("GIsClient=%d GIsServer=1 (%s server)", (int)bStandalone,
              bStandalone ? "listen" : "dedicated");

    // the empty stub kills left click and every ability
    if (Cfg.Abilities)
    {
        Ares::Log("abilities: ability activation left intact");
    }
    else
    {
        Hooking::HookEvery<UAbilitySystemComponent>(0x7F8 / 8, &InternalServerTryActivateAbility);
        Ares::Log("abilities: activation stubbed out (ARES_ABILITIES=0)");
    }

    std::wstring LevelName = Cfg.MapUrl;
    std::wstring Options;
    const size_t Query = LevelName.find(L'?');
    if (Query != std::wstring::npos)
    {
        // OpenLevel adds the ? itself
        Options = LevelName.substr(Query + 1);
        LevelName = LevelName.substr(0, Query);
    }

    // LoadMap only listens if GIsClient is clear or the url says so
    if (bStandalone && Options.find(L"listen") == std::wstring::npos)
        Options = Options.empty() ? L"listen" : Options + L"?listen";

    Ares::LogW(L"opening level %s?%s", LevelName.c_str(), Options.c_str());
   // UGameplayStatics::OpenLevel(UWorld::GetWorld(), FName(TEXT("/Game/Maps/Duality/Duality")), false, TEXT(""));
    UGameplayStatics::OpenLevel(UWorld::GetWorld(), FName(LevelName.c_str()), false, FString(Options.c_str()));
    //UGameplayStatics::OpenLevel(UWorld::GetWorld(), FName(TEXT("/Game/Maps/Poveglia/Range?game=/Game/GameModes/ShootingRange/ShootingRangeGameMode.ShootingRangeGameMode_C")), false, TEXT(""));
   // UGameplayStatics::OpenLevel(UWorld::GetWorld(), FName(TEXT("/Game/Maps/PregameV2/CharacterSelectPersistentLevel")), true, TEXT(""));
    Ares::Log("open level dispatched");
}

static void* ProcessEventDetour(UObject* Obj, UFunction* Function, void* Params)
{
    if (Function)
    {
        GameThreadTicks.fetch_add(1, std::memory_order_relaxed);

        if (!ServerStarted.exchange(true))
            RunServerSequence();

        if (GetCurrentThreadId() == SimulationThread)
            OfflineAuthority::Prepare(Obj);

        if (LocalForceModules::IsDuplicateClientApply(Function))
            return nullptr;

        static double ModuleMs[16] = {};
        static const char* ModuleNames[16] = {"MatchFlow", "ScreenCleanup", "Diagnostics", "LocalSettings", "AresInventory",
            "OfflineAuthority", "CharacterVisibility", "ForceModules", "LocalBots", "MapSurvey", "TacticalMap",
            "RoundReferee", "Freecam", "CustomSkins", "DeveloperConsole", nullptr};
        auto Timed = [](int Index, auto&& Fn)
        {
            if (!Ares::Profiling) { Fn(); return; }
            const auto T0 = std::chrono::steady_clock::now();
            Fn();
            ModuleMs[Index] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count();
        };
        Timed(0, [] { MatchFlow::Tick(); });
        Timed(1, [] { ScreenCleanup::Tick(); });
        Timed(2, [] { Diagnostics::Tick(); });
        if (GetCurrentThreadId() == SimulationThread)
        {
            Timed(3, [] { LocalSettings::Tick(); });
            Timed(4, [] { AresInventory::TickPossessedPawn(); });
            Timed(5, [] { OfflineAuthority::Tick(); });
            Timed(6, [] { LocalCharacterVisibility::Tick(); });
            Timed(7, [] { LocalForceModules::Tick(); });
            Timed(8, [] { LocalBots::Tick(); });
            Timed(9, [] { MapSurvey::Tick(); });
            Timed(10, [] { TacticalMap::Tick(); });
            Timed(11, [] { RoundReferee::Tick(); });
            Timed(12, [] { Freecam::Tick(); });
            Timed(13, [] { CustomSkins::Tick(); });
            Timed(14, [] { DeveloperConsole::Tick(); });
            static auto NextReport = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            if (Ares::Profiling && std::chrono::steady_clock::now() >= NextReport)
            {
                NextReport = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                double Total = 0; std::string Line;
                for (int I = 0; ModuleNames[I]; ++I)
                {
                    Total += ModuleMs[I];
                    if (ModuleMs[I] / 5 >= 1.0) { char B[64]; snprintf(B, sizeof B, " %s %.0f", ModuleNames[I], ModuleMs[I] / 5); Line += B; }
                    ModuleMs[I] = 0;
                }
                Ares::Log("perf: modules %.0f ms/s total (nested ticks count twice):%s", Total / 5, Line.c_str());
            }
        }

        if (DeveloperConsole::SpyArmed())
            DeveloperConsole::SpyRecord(Obj, Function);

        {
            std::lock_guard<std::mutex> Lock(FunctionCacheMutex);
            if (CheckedFunctions.find(Function) == CheckedFunctions.end())
            {
                CheckedFunctions.insert(Function);
                const std::string Name = Function->GetFullName();
                if (Name.find("oordinatedHUDModel.HandlePlayerStateListUpdated") != std::string::npos)
                    SuppressedFunctions.insert(Function);
            }
            if (SuppressedFunctions.count(Function))
                return nullptr;
        }
    }

    if (GetCurrentThreadId() == SimulationThread && CustomSkins::BeforeDispatch(Obj, Function, Params))
        return nullptr;
    if (CrumbDepth < 64) Crumbs[CrumbDepth] = {Obj, Function};
    ++CrumbDepth;
    auto Result = ProcessEventOG(Obj, Function, Params);
    --CrumbDepth;
    LocalDeath::AfterDispatch(Obj, Function, Params);
    if (GetCurrentThreadId() == SimulationThread)
        CustomSkins::AfterDispatch(Obj, Function, Params);
    return Result;
}

void InternalServerTryActivateAbility(UAbilitySystemComponent* _this, FGameplayAbilitySpecHandle Handle, bool, FPredictionKey* PredictionKey, void* TriggerEventData)
{
}

void MainThread()
{
    Ares::Log("Ares attached, ImageBase=0x%llX", (unsigned long long)ImageBase);

    // first, ctrl+c in the client console kills the game
    ConsoleGuard::Install();
    DeveloperConsole::Start();

    // has to run before the platform layer inits
    if (Ares::Config().PatchEndpoint)
    {
        PlatformEndpoint::Install();
        VanguardBypass::Install();
    }
    else
    {
        Ares::Log("platform endpoint: patch disabled by config");
    }

    HANDLE GoEvent = CreateEventW(nullptr, TRUE, FALSE, L"Local\\AresGoEvent");
    if (GoEvent)
    {
        const DWORD Wait = WaitForSingleObject(GoEvent, 120000);
        CloseHandle(GoEvent);
        Ares::Log("go signal %s", Wait == WAIT_OBJECT_0 ? "received" : "timed out, continuing");
    }

    Hooking::Hook(ImageBase + Offsets::ProcessEvent, ProcessEventDetour, ProcessEventOG);
    Ares::Log("ProcessEvent hook installed; waiting for a game-thread call");

    // watchdog
    uint64_t LastTicks = 0;
    double LastProgress = Ares::Seconds();
    bool bReported = false;

    for (;;)
    {
        Sleep(1000);

        const uint64_t Ticks = GameThreadTicks.load(std::memory_order_relaxed);
        if (Ticks != LastTicks)
        {
            LastTicks = Ticks;
            LastProgress = Ares::Seconds();
            bReported = false;
            continue;
        }

        if (bReported)
            continue;

        const double Stalled = Ares::Seconds() - LastProgress;
        const bool bStarted = ServerStarted.load();

        if (Stalled > (bStarted ? 6.0 : 30.0))
        {
            bReported = true;
            char Reason[128];
            snprintf(Reason, sizeof(Reason), "game thread made no progress for %.0fs", Stalled);
            Diagnostics::ReportStall(Reason);
        }
    }
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(hModule);
        std::thread(MainThread).detach();
        break;
    }

    return TRUE;
}

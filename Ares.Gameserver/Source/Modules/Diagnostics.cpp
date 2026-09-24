#include <pch.h>
#include <Headers/Modules/Diagnostics.h>

namespace
{
    bool Enabled()
    {
        static const bool On = []
        {
            wchar_t Buffer[8]{};
            return GetEnvironmentVariableW(L"ARES_DIAG", Buffer, 8) > 0 && Buffer[0] == L'1';
        }();
        return On;
    }

    std::string NameOf(UObject* Object)
    {
        return Object ? Object->GetName() : std::string("<null>");
    }

    void DumpStateMachine(UStateMachineComponent* Machine)
    {
        if (!Machine)
        {
            Ares::Log("  state machine: <null>");
            return;
        }

        UStateComponent* Current = Machine->AuthCurrentState.CurrentState;
        Ares::Log("  state machine: current=%s starting=%s states=%d entered-at=%.1f",
                  NameOf(Current).c_str(),
                  NameOf(Machine->StartingState).c_str(),
                  Machine->States.Num(),
                  Machine->AuthCurrentStateStartTimestamp);

        if (Current)
        {
            if (UTimeGameStateComponent* Timed = Current->Cast<UTimeGameStateComponent>())
            {
                Ares::Log("  timed state: next=%s length=%.1f",
                          NameOf(Timed->NextGameState).c_str(),
                          Timed->StateLengthSeconds);
            }
        }

        std::string All;
        for (auto& Pair : Machine->States)
        {
            if (UStateComponent* State = Pair.Key())
            {
                if (!All.empty())
                    All += ", ";
                All += State->GetName();
            }
        }
        Ares::Log("  known states: %s", All.empty() ? "<none>" : All.c_str());
    }

    void DumpPlayers(AShooterGameMode* GameMode)
    {
        int32 Controllers = 0;
        for (APlayerController* PC : GetObjectsOfClass<APlayerController>())
        {
            if (!PC || PC->IsDefaultObject())
                continue;
            Controllers++;
            Ares::Log("  controller %s pawn=%s playerstate=%s",
                      NameOf(PC).c_str(), NameOf(PC->Pawn).c_str(), NameOf(PC->PlayerState).c_str());
        }
        Ares::Log("  controllers=%d players=%d bots=%d", Controllers, GameMode->NumPlayers, GameMode->NumBots);
    }

    void DumpStreaming(UWorld* World)
    {
        std::string Summary;
        for (ULevelStreaming* Level : World->StreamingLevels)
        {
            if (!Level)
                continue;
            if (!Summary.empty())
                Summary += ", ";
            Summary += Level->PackageNameToLoad.ToString();
            Summary += Level->LoadedLevel ? "(loaded)" : "(pending)";
        }
        Ares::Log("  streaming: %s", Summary.empty() ? "<none>" : Summary.c_str());
    }
} // namespace

void Diagnostics::Tick()
{
    if (!Enabled())
        return;

    static bool bBusy = false;
    FReentrancyGuard Guard(bBusy);
    if (!Guard.Entered())
        return;

    static double LastDump = -1000.0;
    const double Now = Ares::Seconds();
    if (Now - LastDump < 2.0)
        return;
    LastDump = Now;

    UWorld* World = UWorld::GetWorld();
    if (!World)
        return;

    AShooterGameMode* GameMode = World->AuthorityGameMode ? World->AuthorityGameMode->Cast<AShooterGameMode>() : nullptr;
    if (!GameMode)
    {
        Ares::Log("diag: no authority game mode yet");
        return;
    }

    Ares::Log("diag: gamemode=%s matchstate=%s",
              GameMode->Class->GetName().c_str(),
              GameMode->MatchState.ToString().c_str());

    if (AShooterGameState* GameState = GameMode->GameState ? GameMode->GameState->Cast<AShooterGameState>() : nullptr)
        Ares::Log("  gamestate=%s phase=%d", NameOf(GameState).c_str(), (int)GameState->Phase);

    DumpStateMachine(GameMode->StateMachine);
    DumpPlayers(GameMode);
    DumpStreaming(World);
}

#include <tlhelp32.h>

namespace
{
    // main thread = oldest thread
    DWORD FindMainThreadId()
    {
        const DWORD Pid = GetCurrentProcessId();
        HANDLE Snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
        if (Snapshot == INVALID_HANDLE_VALUE)
            return 0;

        THREADENTRY32 Entry{};
        Entry.dwSize = sizeof(Entry);

        DWORD Best = 0;
        ULONGLONG BestCreated = MAXULONGLONG;

        if (Thread32First(Snapshot, &Entry))
        {
            do
            {
                if (Entry.th32OwnerProcessID != Pid)
                    continue;

                HANDLE Thread = OpenThread(THREAD_QUERY_INFORMATION, FALSE, Entry.th32ThreadID);
                if (!Thread)
                    continue;

                FILETIME Created{}, Exit{}, Kernel{}, User{};
                if (GetThreadTimes(Thread, &Created, &Exit, &Kernel, &User))
                {
                    ULARGE_INTEGER When{};
                    When.LowPart = Created.dwLowDateTime;
                    When.HighPart = Created.dwHighDateTime;
                    if (When.QuadPart < BestCreated)
                    {
                        BestCreated = When.QuadPart;
                        Best = Entry.th32ThreadID;
                    }
                }
                CloseHandle(Thread);
            } while (Thread32Next(Snapshot, &Entry));
        }

        CloseHandle(Snapshot);
        return Best;
    }

    uint64_t ImageSize()
    {
        const uint8_t* Base = (const uint8_t*)ImageBase;
        const uint32_t PeOffset = *(const uint32_t*)(Base + 0x3C);
        return *(const uint32_t*)(Base + PeOffset + 0x18 + 0x38); // OptionalHeader.SizeOfImage
    }

    void DescribeAddress(uint64_t Address, char* Out, size_t OutSize)
    {
        const uint64_t Size = ImageSize();
        if (Address >= ImageBase && Address < ImageBase + Size)
        {
            snprintf(Out, OutSize, "client+0x%llX", (unsigned long long)(Address - ImageBase));
            return;
        }

        HMODULE Module = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR)Address, &Module) && Module)
        {
            char Path[MAX_PATH]{};
            GetModuleFileNameA(Module, Path, MAX_PATH);
            const char* Name = strrchr(Path, '\\');
            snprintf(Out, OutSize, "%s+0x%llX", Name ? Name + 1 : Path,
                     (unsigned long long)(Address - (uint64_t)Module));
            return;
        }

        snprintf(Out, OutSize, "0x%llX", (unsigned long long)Address);
    }
} // namespace

void Diagnostics::ReportStall(const char* Reason)
{
    const DWORD ThreadId = FindMainThreadId();
    if (!ThreadId)
    {
        Ares::Log("stall (%s): could not find the main thread", Reason);
        return;
    }

    HANDLE Thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                               FALSE, ThreadId);
    if (!Thread)
    {
        Ares::Log("stall (%s): OpenThread(%lu) failed", Reason, ThreadId);
        return;
    }

    if (SuspendThread(Thread) == (DWORD)-1)
    {
        Ares::Log("stall (%s): SuspendThread failed", Reason);
        CloseHandle(Thread);
        return;
    }

    CONTEXT Context{};
    Context.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(Thread, &Context))
    {
        char Where[256];
        DescribeAddress(Context.Rip, Where, sizeof(Where));
        Ares::Log("stall (%s): main thread %lu parked at %s", Reason, ThreadId, Where);

        // no unwind info, just scan the stack for client addrs
        const uint64_t Size = ImageSize();
        int Printed = 0;
        for (uint64_t Slot = Context.Rsp; Slot < Context.Rsp + 0x1000 && Printed < 16; Slot += 8)
        {
            uint64_t Value = 0;
            if (!ReadProcessMemory(GetCurrentProcess(), (void*)Slot, &Value, 8, nullptr))
                continue;
            if (Value < ImageBase || Value >= ImageBase + Size)
                continue;

            Ares::Log("    return to client+0x%llX", (unsigned long long)(Value - ImageBase));
            Printed++;
        }
    }

    ResumeThread(Thread);
    CloseHandle(Thread);
}

#include <pch.h>
#include <Headers/Modules/ShooterGame/LocalForceModules.h>
#include <map>
#include <tuple>

// same force module from the same source twice in one frame, drop the second

namespace LocalForceModules { void Init(); }
INIT_MODULE(LocalForceModules);

namespace
{
    using FApply = FForceModuleHandle* (*)(UForceModuleManagerComponent*, FForceModuleHandle*, AActor*, UClass*, float);
    FApply ApplyOG = nullptr, ApplySyncedOG = nullptr, ApplyDeterministicOG = nullptr;

    std::map<std::tuple<void*, void*, void*>, double> LastApplied;
    constexpr double SameFrame = 0.05;

    bool IsRepeat(UForceModuleManagerComponent* Manager, AActor* Source, UClass* Module)
    {
        if (Ares::Config().Mode != Ares::EMode::Standalone || !Manager || !Module) return false;
        const double Now = Ares::Seconds();
        auto& Last = LastApplied[{(void*)Manager, (void*)Module, (void*)Source}];
        if (Last > 0.0 && Now - Last < SameFrame)
        {
            Ares::Log("force: dropped a second %s from one source in the same frame",
                      Module->GetName().c_str());
            return true;
        }
        Last = Now;
        return false;
    }

    FForceModuleHandle* Apply(UForceModuleManagerComponent* Manager, FForceModuleHandle* Out, AActor* Source,
                              UClass* Module, float Duration)
    {
        if (Out && IsRepeat(Manager, Source, Module)) { *Out = FForceModuleHandle{}; return Out; }
        return ApplyOG(Manager, Out, Source, Module, Duration);
    }

    FForceModuleHandle* ApplySynced(UForceModuleManagerComponent* Manager, FForceModuleHandle* Out, AActor* Source,
                                    UClass* Module, float Duration)
    {
        if (Out && IsRepeat(Manager, Source, Module)) { *Out = FForceModuleHandle{}; return Out; }
        return ApplySyncedOG(Manager, Out, Source, Module, Duration);
    }

    FForceModuleHandle* ApplyDeterministic(UForceModuleManagerComponent* Manager, FForceModuleHandle* Out,
                                           AActor* Source, UClass* Module, float Duration)
    {
        if (Out && IsRepeat(Manager, Source, Module)) { *Out = FForceModuleHandle{}; return Out; }
        return ApplyDeterministicOG(Manager, Out, Source, Module, Duration);
    }
}

void LocalForceModules::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Hooking::Hook(ImageBase + 0x19C5000, Apply, ApplyOG);
    Hooking::Hook(ImageBase + 0x19C49D0, ApplySynced, ApplySyncedOG);
    Hooking::Hook(ImageBase + 0x19C4F20, ApplyDeterministic, ApplyDeterministicOG);
    Ares::Log("force: apply hooks installed (%d/%d/%d)", ApplyOG ? 1 : 0, ApplySyncedOG ? 1 : 0,
              ApplyDeterministicOG ? 1 : 0);
}

// logs what the pawn is holding, apply hooks dont fire for satchel
void LocalForceModules::Tick()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    static double NextCheck = 0;
    if (Ares::Seconds() < NextCheck) return;
    NextCheck = Ares::Seconds() + 0.2;
    static bool Busy = false;
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered()) return;
    auto World = UWorld::GetWorld();
    auto Controller = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    auto Pawn = Controller ? Controller->Pawn : nullptr;
    if (!Pawn) return;
    auto Component = Pawn->GetComponentByClass(UForceModuleManagerComponent::StaticClass());
    auto Manager = Component ? Component->Cast<UForceModuleManagerComponent>() : nullptr;
    if (!Manager) return;
    static int Reported = -1;
    const int Live = Manager->ForceModules.Num();
    if (Live == Reported) return;
    Reported = Live;
    std::string Line;
    for (auto Module : Manager->ForceModules)
        if (Module && Module->Class) Line += " " + Module->Class->GetName();
    Ares::Log("force: pawn now holds %d module(s):%s", Live, Line.empty() ? " none" : Line.c_str());
}

// server and client are the same process here, satchel pushed twice
bool LocalForceModules::IsDuplicateClientApply(UFunction* Function)
{
    if (Ares::Config().Mode != Ares::EMode::Standalone || !Function) return false;
    static UFunction* ClientApply = nullptr;
    static bool Resolved = false;
    if (!Resolved)
    {
        Resolved = true;
        ClientApply = UForceModuleManagerComponent::StaticClass()->FindFunction("ClientApplyForceModule");
    }
    return Function == ClientApply;
}

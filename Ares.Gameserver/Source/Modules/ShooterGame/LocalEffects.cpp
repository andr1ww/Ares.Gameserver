#include <pch.h>

namespace LocalEffects { void Init(); }
INIT_MODULE(LocalEffects);

namespace
{
    // replicated path just logs and bails on a client, force LocalOnly
    using ContinuousFn = FEffectID* (*)(FEffectID*, UClass*, AActor*, UObject*, void*,
                                       UObject*, const FVector*, const FRotator*, const FName*, bool, int32);
    using OneShotFn = void (*)(UClass*, AActor*, UObject*, void*, UObject*,
                              const FVector*, const FRotator*, const FName*, bool, int32);
    ContinuousFn ContinuousOG = nullptr;
    OneShotFn OneShotOG = nullptr;
    using ClientSimulatedFn = FEffectID* (*)(FEffectID*, AController*, UClass*, AActor*, UObject*,
        void*, const FVector*, const FRotator*, const FName*, int32);
    ClientSimulatedFn ClientSimulatedOG = nullptr;

    FEffectID* ClientSimulated(FEffectID* Result, AController* Controller, UClass* Effect,
        AActor* AttachTo, UObject* Context, void* Data, const FVector* Location,
        const FRotator* Rotation, const FName* Socket, int32 Alliance)
    {
        Ares::Log("effects: client simulated %s attach=%s context=%s controller=%s",
            Effect ? Effect->GetName().c_str() : "none", AttachTo ? AttachTo->GetName().c_str() : "none",
            Context ? Context->GetName().c_str() : "none", Controller ? Controller->GetName().c_str() : "none");
        // alliance filter never resolves offline, Any or nothing spawns
        return ContinuousOG(Result, Effect, AttachTo, Context, Data, AttachTo,
                            Location, Rotation, Socket, true, (int32)EAresAlliance::Alliance_Any);
    }
    using ManagedContinuousFn = FEffectID* (*)(UEffectManagerComponent*, FEffectID*, void*, UClass*,
        void*, const FVector*, const FRotator*, const FName*, bool, bool, float, float, int32);
    ManagedContinuousFn ManagedContinuousOG = nullptr;
    using ManagedOneShotFn = void (*)(UEffectManagerComponent*, void*, UClass*, void*,
        const FVector*, const FRotator*, const FName*, bool, float, float, int32);
    ManagedOneShotFn ManagedOneShotOG = nullptr;
    UDecalManager* (*GetDecalManagerOG)(AShooterGameState*) = nullptr;

    UDecalManager* GetDecalManager(AShooterGameState* State)
    {
        auto Manager = GetDecalManagerOG(State);
        if (Manager) return Manager;
        static bool Busy = false;
        FReentrancyGuard Guard(Busy);
        if (!Guard.Entered()) return nullptr;
        // client only init never ran on the host
        auto Object = UGameplayStatics::SpawnObject(UDecalManager::StaticClass(), State);
        State->DecalManager = Object ? Object->Cast<UDecalManager>() : nullptr;
        if (State->DecalManager) Ares::Log("effects: initialized the local decal manager");
        return State->DecalManager;
    }

    void ManagedOneShot(UEffectManagerComponent* Manager, void* Trigger, UClass* Effect, void* Data,
        const FVector* Location, const FRotator* Rotation, const FName* Socket,
        bool LocalOnly, float StartTime, float Duration, int32 Alliance)
    {
        ManagedOneShotOG(Manager, Trigger, Effect, Data, Location, Rotation, Socket, true, StartTime, Duration, Alliance);
    }

    FEffectID* ManagedContinuous(UEffectManagerComponent* Manager, FEffectID* Result, void* Trigger,
        UClass* Effect, void* Data, const FVector* Location, const FRotator* Rotation,
        const FName* Socket, bool LocalOnly, bool Transient, float StartTime, float Duration, int32 Alliance)
    {
        return ManagedContinuousOG(Manager, Result, Trigger, Effect, Data, Location, Rotation,
                                   Socket, true, Transient, StartTime, Duration, Alliance);
    }

    FEffectID* Continuous(FEffectID* Result, UClass* Effect, AActor* AttachTo, UObject* Context,
                          void* Data, UObject* WorldContext, const FVector* Location,
                          const FRotator* Rotation, const FName* Socket, bool LocalOnly, int32 Alliance)
    {
        return ContinuousOG(Result, Effect, AttachTo, Context, Data, WorldContext,
                            Location, Rotation, Socket, true, Alliance);
    }

    void OneShot(UClass* Effect, AActor* AttachTo, UObject* Context, void* Data,
                 UObject* WorldContext, const FVector* Location, const FRotator* Rotation,
                 const FName* Socket, bool LocalOnly, int32 Alliance)
    {
        OneShotOG(Effect, AttachTo, Context, Data, WorldContext, Location, Rotation, Socket, true, Alliance);
    }
}

void LocalEffects::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Hooking::Hook(ImageBase + 0x1903600, Continuous, ContinuousOG);
    Hooking::Hook(ImageBase + 0x1903190, ClientSimulated, ClientSimulatedOG);
    Hooking::Hook(ImageBase + 0x19051E0, OneShot, OneShotOG);
    Hooking::Hook(ImageBase + 0x1904070, ManagedContinuous, ManagedContinuousOG);
    Hooking::Hook(ImageBase + 0x1905FD0, ManagedOneShot, ManagedOneShotOG);
    Hooking::Hook(ImageBase + 0x1A0EA60, GetDecalManager, GetDecalManagerOG);
    Ares::Log("effects: standalone effects use the native local presentation path");
}

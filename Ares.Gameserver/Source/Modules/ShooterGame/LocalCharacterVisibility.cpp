#include <pch.h>
#include <Headers/Modules/ShooterGame/LocalCharacterVisibility.h>
#include <Headers/Modules/ShooterGame/LocalBots.h>

// ai pawns get the first person hide rule, turret etc were invisible

namespace
{
    bool NeedsThirdPersonMesh(AShooterCharacter* Character, APlayerController* Controller)
    {
        if (!Character || Character->IsDefaultObject() || Character->bActorIsBeingDestroyed) return false;
        if (!Character->Mesh || !Character->Mesh->bHiddenInGame) return false;
        if (Character == Controller->Pawn || Character == Controller->GetViewTarget()) return false;
        auto Owner = Character->Controller;
        return Owner && !Owner->IsA<APlayerController>();
    }
}

void LocalCharacterVisibility::Tick()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    static bool Busy = false;
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered()) return;
    static double LastSweep = -1.0;
    const double Now = Ares::Seconds();
    if (Now - LastSweep < 0.1) return;
    LastSweep = Now;

    auto World = UWorld::GetWorld();
    auto Controller = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    if (!Controller) return;
    for (auto Character : LocalBots::Characters())
    {
        if (!NeedsThirdPersonMesh(Character, Controller)) continue;
        Character->Set3PMeshVisible(true);
        Ares::Log("presentation: showing %s, an AI pawn hidden by the first-person rule",
                  Character->GetName().c_str());
    }
}

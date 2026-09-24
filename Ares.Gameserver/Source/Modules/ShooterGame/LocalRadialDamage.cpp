#include <pch.h>
#include <Headers/Modules/ShooterGame/LocalDamage.h>

// TakeDamage never reaches the health section, explosions did nothing

namespace LocalRadialDamage { void Init(); }
INIT_MODULE(LocalRadialDamage);

namespace
{
    using FTakeDamage = float (*)(AActor*, float, void*, AController*, AActor*);
    FTakeDamage TakeDamageOG = nullptr;

    UDamageSectionComponent* HealthOf(AActor* Target)
    {
        auto Character = Target ? Target->Cast<AShooterCharacter>() : nullptr;
        auto Handler = Character ? Character->DamageHandler : nullptr;
        if (!Handler) return nullptr;
        for (auto Section : Handler->DamageSections)
            if (Section && (int)Section->Type == 0) return Section;
        return nullptr;
    }

    float TakeDamage(AActor* Target, float Amount, void* Event, AController* Instigator, AActor* Causer)
    {
        auto Health = Ares::Config().Mode == Ares::EMode::Standalone ? HealthOf(Target) : nullptr;
        const float Before = Health ? Health->Life : 0.f;
        const float Result = TakeDamageOG(Target, Amount, Event, Instigator, Causer);
        static int Seen = 0;
        if (Seen++ < 60)
            Ares::Log("takedamage: %s amount=%.1f health=%.1f causer=%s type=%s", Target->GetName().c_str(),
                      Amount, Before, Causer ? Causer->GetName().c_str() : "<none>",
                      Event ? (*(UClass**)((uint8*)Event + 0x8)
                                   ? (*(UClass**)((uint8*)Event + 0x8))->GetName().c_str()
                                   : "<no type>")
                            : "<no event>");
        if (!Health || Amount <= 0.f) return Result;
        if (Health->Life < Before) return Result; // og already did it

        UClass* Type = Event ? *(UClass**)((uint8*)Event + 0x8) : nullptr; // FDamageEvent::DamageTypeClass
        auto Pawn = Instigator ? Instigator->Pawn : nullptr;
        if (!Type || !Pawn) return Result;

        FHitResult Hit{};
        Hit.Actor = Target;
        Hit.Component = Target->Cast<AShooterCharacter>()->Mesh;
        const FVector Where = Target->K2_GetActorLocation();
        *(FVector*)&Hit.ImpactPoint = Where;
        *(FVector*)&Hit.Location = Where;
        *(FVector*)&Hit.TraceStart = Causer ? Causer->K2_GetActorLocation() : Where;
        *(FVector*)&Hit.TraceEnd = Where;
        *(FVector*)&Hit.ImpactNormal = FVector{0.f, 0.f, 1.f};
        LocalDamage::ApplyHit(Causer ? Causer : Target, Pawn, Type, Hit, Amount);
        return Before - (Health ? Health->Life : Before);
    }
}

void LocalRadialDamage::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Hooking::Hook(ImageBase + 0x1D022B0, TakeDamage, TakeDamageOG);
}

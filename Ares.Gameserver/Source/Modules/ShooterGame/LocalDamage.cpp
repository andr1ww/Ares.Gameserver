#include <pch.h>
#include <Headers/Modules/ShooterGame/CustomSkins.h>
#include <unordered_map>
#include <cmath>
#include <Headers/Modules/ShooterGame/LocalDamage.h>

namespace LocalDamage { void Init(); }
INIT_MODULE(LocalDamage);

namespace
{
    using DamageFn = float (*)(UDamageProjectileEffectComponent*, const FVector*, const FHitResult*);
    DamageFn DamageOG = nullptr;
    bool Trace = false;
    void (*MeleeImpactOG)(UMeleeAttackStateComponent*, const FHitResult*, bool) = nullptr;

}

namespace
{
    // combat stats counter isnt kept offline, banner tier needs it
    int NextKillNumber(AShooterPlayerState* Killer, AShooterGameState* GameState)
    {
        struct FTally { int Round; int Kills; };
        static std::unordered_map<AShooterPlayerState*, FTally> Tallies;
        const int Round = GameState ? GameState->RoundNumber : 0;
        auto& Tally = Tallies[Killer];
        if (Tally.Round != Round) { Tally.Round = Round; Tally.Kills = 0; }
        return ++Tally.Kills;
    }

    // no banner without SkinKillBannerData, default guns dont have one
    UClass* DefaultKillBanner()
    {
        static UClass* Found = nullptr;
        static bool Looked = false;
        if (!Looked)
        {
            Looked = true;
            for (UKillBannerData* Banner : GetObjectsOfClass<UKillBannerData>())
                if (Banner && Banner->Class && Banner->Class->GetName() == "KillBannerData_Base_C")
                    Found = Banner->Class;
        }
        return Found;
    }

    void ReportKill(AShooterCharacter* Killer, AShooterCharacter* Killed, UClass* Type)
    {
        auto World = UWorld::GetWorld();
        auto GameState = World && World->GameState ? World->GameState->Cast<AShooterGameState>() : nullptr;
        auto KillerState = Killer->PlayerState ? Killer->PlayerState->Cast<AShooterPlayerState>() : nullptr;
        auto VictimState = Killed->PlayerState ? Killed->PlayerState->Cast<AShooterPlayerState>() : nullptr;
        auto Weapon = Killer->Inventory ? Killer->Inventory->CurrentEquippable : nullptr;
        if (Weapon && !Weapon->SkinKillBannerData && !CustomSkins::IsActive(Weapon))
            Weapon->SkinKillBannerData = DefaultKillBanner();
        const int KillNumber = NextKillNumber(KillerState, GameState);
        CustomSkins::OnKill(Weapon);

        Killer->AuthOnGotKill(Killed, Type ? (UDamageType*)Type->DefaultObject : nullptr);
        Killer->MulticastNotifyKilledEnemy(Killer, Killed, KillNumber);

        TArray<FAresAssist> NoAssists{};
        if (GameState && KillerState && VictimState)
        {
            // no ClientReceivePlayerKillEvent, doubles the kill feed line
            KillerState->AuthGiveUltimatePoints(1, EIsRefundable::No);
            for (UCoordinatedHUDModel* Model : GetObjectsOfClass<UCoordinatedHUDModel>())
            {
                if (!Model || Model->IsDefaultObject()) continue;
                // null response = never the headshot banner
                Model->RebroadcastOnAnyPlayerKilled(KillerState, VictimState, KillNumber, 1, nullptr, Weapon,
                                                    NoAssists);
                break;
            }
        }
        NoAssists.Free();
        // no TriggerHUDKillConfirmSound, banner already plays the sound

        Ares::Log("kill: %s killed %s (number %d, weapon %s, banner-data %s)", Killer->GetName().c_str(),
                  Killed->GetName().c_str(), KillNumber, Weapon ? Weapon->GetName().c_str() : "none",
                  Weapon && Weapon->SkinKillBannerData ? Weapon->SkinKillBannerData->GetName().c_str() : "none");
    }
}

void LocalDamage::ApplyHit(AActor* Causer, APawn* Instigator, UClass* Type, const FHitResult& Hit, float Amount)
{
        auto Target = Hit.Actor.Get();
        if (!Causer || !Target || !Target->bCanBeDamaged) return;
        if (!Instigator || !Instigator->IsLocallyControlled()) return;
        if (!Type || !Type->DefaultObject) return;
        UDamageableComponent* Handler = nullptr;
        UDamageSectionComponent* Health = nullptr;
        if (auto Object = Target->Cast<AGameObject>())
        {
            if (Object->Invulnerable) return;
            Handler = Object->DamageHandler;
            Health = Object->HealthSection;
        }
        else if (auto Character = Target->Cast<AShooterCharacter>())
        {
            Handler = Character->DamageHandler;
            if (Handler)
                for (auto Section : Handler->DamageSections)
                    if (Section && (int)Section->Type == 0) Health = Section;
        }
        if (!Handler || !Handler->bAlive) return;
        // range targets have no team, dont treat that as ally
        if (Target != Instigator)
        {
            auto Mine = UBaseTeamComponent::GetTeamComponent(Instigator);
            auto Theirs = UBaseTeamComponent::GetTeamComponent(Target);
            if (Mine && Theirs && Mine->HasValidTeam() && Theirs->HasValidTeam() && Mine->IsTeamAlly(Theirs))
                return;
        }
        // no armor handling here
        if (Handler->GetRawShield() > 0) return;
        FNetworkedPointDamageResponseData Response{};
        Response.DamageTaken = Response.DamageDealt = Amount;
        Response.DamageCauser = Causer;
        Response.EventInstigator = Instigator->Controller;
        Response.EventInstigatorPawn = Instigator;
        Response.DamageType = Type;
        Response.DamagedComponent = Hit.Component.Get();
        Response.DamagedBone = Hit.BoneName;
        Response.FalloffMultiplier = 1;
        *(FVector*)&Response.DamageImpactLocation = Hit.ImpactPoint;
        *(FVector*)&Response.DamageImpactNormal = Hit.ImpactNormal;
        FVector Direction = Hit.TraceEnd - Hit.TraceStart;
        const float Length = std::sqrt(Direction.X * Direction.X + Direction.Y * Direction.Y + Direction.Z * Direction.Z);
        if (Length > 0) *(FVector*)&Response.DamageDirection = Direction / Length;
        float Before = Health ? Health->Life : 0;
        if (Health && Health->MaximumLife > 0)
        {
            float Applied = 0, Remaining = 0;
            Health->AuthApplyDamage(Amount, Type->DefaultObject->Cast<UDamageType>(),
                Instigator->Controller, Causer, nullptr, Response.RegionalDamage, &Applied, &Remaining, false);
            Response.DamageTaken = Response.DamageDealt = Applied;
            if (!(Applied > 0)) return;
            Response.DamagedComponents.Add(Health);
            Response.bDamageKilledTarget = Handler->bCanDie && Health->Life <= 0;
            if (Response.bDamageKilledTarget) Response.DestroyedComponents.Add(Health);
        }
        Handler->NotifyDamage_Point(Response);
        if (Response.bDamageKilledTarget)
            if (auto Killed = Target->Cast<AShooterCharacter>())
                if (auto Killer = Instigator->Cast<AShooterCharacter>())
                    ReportKill(Killer, Killed, Type);
        static int Count = 0;
        if (Count++ < 100) Ares::Log("local-damage: %s %.2f health %.2f -> %.2f killed=%d",
            Target->GetName().c_str(), Response.DamageTaken, Before, Health ? Health->Life : 0, (int)Response.bDamageKilledTarget);
    }

namespace
{
    void MeleeImpact(UMeleeAttackStateComponent* State, const FHitResult* Hit, bool Backstab)
    {
        MeleeImpactOG(State, Hit, Backstab);
        auto Owner = State->GetOwner();
        auto Weapon = Owner ? Owner->Cast<AAresEquippable>() : nullptr;
        if (!Weapon || !Hit) return;
        auto& Tuning = Backstab ? State->BackstabAttackEffectTuning : State->AttackEffectTuning;
        // MulticastHitImpact skips fx on authority role
        if ((int)Weapon->Role == 3 && Weapon->MyPawn && Weapon->MyPawn->IsLocallyControlled())
        {
            auto PlayImpact = (void (*)(UObject*, UClass*, const FHitResult*, AAresEquippable*))(ImageBase + 0x1904370);
            PlayImpact(State, Tuning.ImpactFXC.Get(), Hit, Weapon);
        }
        LocalDamage::ApplyHit(Weapon, Weapon->MyPawn, Tuning.DamageType.Get(), *Hit, Tuning.AttackDamage);
    }

    float Damage(UDamageProjectileEffectComponent* Effect, const FVector* Origin, const FHitResult* Hit)
    {
        const float Result = DamageOG(Effect, Origin, Hit);
        static int Count = 0;
        if (Trace && Count++ < 100) Ares::Log("gameplay-trace: projectile damage target=%s amount=%.2f bone=%s",
            Hit->Actor.Get() ? Hit->Actor.Get()->GetName().c_str() : "none", Result, Hit->BoneName.ToString().c_str());
        if (std::isfinite(Result) && Result > 0)
        {
            auto Actor = Effect->GetOwner();
            auto Bullet = Actor ? Actor->Cast<AProjectile>() : nullptr;
            if (Bullet && !Bullet->bSimulation)
            {
                APawn* Instigator = Bullet->Instigator;
                auto Owner = Bullet->Owner;
                for (int Depth = 0; !Instigator && Owner && Depth < 8; ++Depth, Owner = Owner->Owner)
                {
                    if (auto Item = Owner->Cast<AAresItem>()) Instigator = Item->MyPawn;
                    else Instigator = Owner->Cast<APawn>();
                }
                LocalDamage::ApplyHit(Bullet, Instigator, Effect->DamageTuning.DamageType.Get(), *Hit, Result);
            }
        }
        return Result;
    }
}

void LocalDamage::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Trace = GetEnvironmentVariableA("ARES_TRACE_GAMEPLAY", nullptr, 0) != 0;
    Hooking::Hook(ImageBase + 0x1D16AF0, Damage, DamageOG);
    Hooking::Hook(ImageBase + 0x19AF1A0, MeleeImpact, MeleeImpactOG);
    Ares::Log("local-damage: projectile and melee delivery for world objects and unarmored Range bots enabled");
}

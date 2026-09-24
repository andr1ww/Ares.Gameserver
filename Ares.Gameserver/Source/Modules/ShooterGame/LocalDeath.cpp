#include <pch.h>
#include <Headers/Modules/ShooterGame/LocalDeath.h>

// AuthTriggerDeathReaction is stripped, play the death montage ourselves

namespace
{
    UFunction* HandleOnDeathFn = nullptr;
    bool Resolved = false;

    // region only, first entry is the generic death
    UAnimMontage* PickDeathMontage(UAresHitReactions* Reactions, EAresRegionalDamage Region)
    {
        UAnimMontage* Fallback = nullptr;
        for (auto& Entry : Reactions->HitReactions)
        {
            if (!Entry.Anim3P)
                continue;
            if (Entry.RegionalDamage == Region)
                return Entry.Anim3P;
            if (!Fallback)
                Fallback = Entry.Anim3P;
        }
        return Fallback;
    }
}

void LocalDeath::AfterDispatch(UObject* Object, UFunction* Function, void* Parameters)
{
    if (Ares::Config().Mode != Ares::EMode::Standalone || !Function || !Object) return;
    if (!Resolved)
    {
        Resolved = true;
        HandleOnDeathFn = AShooterCharacter::StaticClass()->FindFunction("HandleOnDeath");
    }
    if (Function != HandleOnDeathFn) return;

    auto Character = Object->Cast<AShooterCharacter>();
    if (!Character || !Character->DeathReactionComponent) return;
    auto Reaction = Character->DeathReactionComponent;
    if (Reaction->bDeathReactionPlaying) return;

    auto Response = Parameters ? *(UDamageResponse**)Parameters : nullptr;
    auto Reactions = Character->DeathReactionObject;
    auto Mesh = Character->Mesh;
    auto Animation = Mesh ? Mesh->AnimScriptInstance : nullptr;

    UAnimMontage* Montage = nullptr;
    if (Reactions && Animation)
    {
        const EAresRegionalDamage Region =
            Response ? Response->GetRegionalDamage() : EAresRegionalDamage::RegionalDamage_Normal;
        Montage = PickDeathMontage(Reactions, Region);
    }

    if (Montage)
    {
        Animation->Montage_Play(Montage, 1.0f, EMontagePlayReturnType::MontageLength, 0.0f, true); // stop all montages
        Reaction->DeathAnimMontage = Montage;
        Reaction->bDeathReactionPlaying = true;
        Reaction->TimeInSecondsWhenDeathReactionIsComplete = Montage->SequenceLength;
        if (auto Instance = Animation->Cast<UShooterCharacterAnimInstance>())
            if (auto State = Instance->AnimationStateComponent)
                State->OnMontageStart(Montage);
        Ares::Log("death: %s plays %s (%.2fs)", Character->GetName().c_str(),
                  Montage->GetName().c_str(), Montage->SequenceLength);
        return;
    }

    // no ragdoll fallback, physics asset just heaps the corpse
    Ares::Log("death: no death montage for %s (reactions=%s anim=%s)", Character->GetName().c_str(),
              Reactions ? "yes" : "none", Animation ? "yes" : "none");
}

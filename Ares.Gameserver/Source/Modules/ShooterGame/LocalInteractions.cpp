#include <pch.h>
#include <Headers/Modules/ShooterGame/LocalDrop.h>

namespace LocalInteractions { void Init(); }
INIT_MODULE(LocalInteractions);

namespace
{
    void (*UseInputOG)(UAresInputComponent*, EUseKeyType, bool) = nullptr;

    int SelectionPresentationMode(UInteractableUserComponent* User)
    {
        const int Mode = ((int (*)(UActorComponent*))(ImageBase + 0x338DF80))(User);
        // hud delegates only fire in the client branch
        return Mode == 2 && User->OwningShooterCharacter && User->OwningShooterCharacter->IsLocallyControlled() ? 3 : Mode;
    }

    void Notify(TMulticastInlineDelegate<void(UInteractableUserComponent*)>& Event,
                UInteractableUserComponent* User)
    {
        // copy first, callbacks can unbind
        std::vector<FScriptDelegate> List;
        for (auto Binding : Event.InvocationList) List.push_back(Binding);
        for (auto Binding : List)
            if (auto Object = Binding.Object.Get())
                if (auto Function = Object->Class->FindFunction(Binding.FunctionName.ToString().c_str()))
                    Object->ProcessEvent(Function, &User);
    }

    void UseInput(UAresInputComponent* Input, EUseKeyType Key, bool Pressed)
    {
        UseInputOG(Input, Key, Pressed);
        auto Pawn = Input->ShooterCharacterOwner;
        if (!Pressed || !Pawn || !Pawn->IsLocallyControlled()) return;
        static bool Busy = false;
        FReentrancyGuard Guard(Busy);
        if (!Guard.Entered()) return;
        for (auto Component : Pawn->GetComponentsByClass(UInteractableUserComponent::StaticClass()))
        {
            auto User = Component->Cast<UInteractableUserComponent>();
            User->FindBestUsable();
            auto Usable = Key == EUseKeyType(0) ? User->BestUsableComponent : User->BestAlternateUsableComponent;
            if (!Usable || Usable->UseKey != Key || !User->CanUseComponent(Usable)) continue;
            // AuthUseButtonPressed/AuthUseInstant are stripped, instant use only
            if (auto Ground = Usable->GetOwner()->Cast<AAresOnGroundEquippable>())
            {
                std::string Report;
                if (!LocalDrop::Pickup(Pawn, Ground, Report))
                    Ares::Log("interaction: %s", Report.c_str());
                continue;
            }
            if (Usable->GetUseTime(User) > 0 || Usable->EquippableToSwitchTo || Usable->CharacterUsingBuff) continue;
            Ares::Log("interaction: local instant use of %s", Usable->GetOwner()->GetName().c_str());
            TWeakObjectPtr<UUsableComponent> Alive(Usable);
            Notify(Usable->OnUseStarted, User);
            if (Alive.Get()) Notify(Usable->OnUseSuccessful, User);
        }
    }
}

void LocalInteractions::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Hooking::Hook(ImageBase + 0x1966DE0, UseInput, UseInputOG); // use/alternate use input
    Hooking::Rel32Hook(ImageBase + 0x18C224A, SelectionPresentationMode);
}

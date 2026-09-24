#include <pch.h>

// ability key pressed again on an equipped ability sends Activate, nothing binds it

namespace LocalAbilityActivate { void Init(); }
INIT_MODULE(LocalAbilityActivate);

namespace
{
    void (*OnActivationInputOG)(UAresInventory*, EAresItemSlot, EAresInputState, EAresActivationInputType,
                                float) = nullptr;
    EAresItemSlot SwallowedSlot = (EAresItemSlot)0xFF; // eat the release too or it puts the ability away

    bool ListeningForActivate(AAresEquippable* Item)
    {
        if (!Item) return false;
        for (auto Machine : Item->EquippableStateMachines)
        {
            if (!Machine) continue;
            auto State = Machine->AuthCurrentState.CurrentState;
            auto Responder = State ? State->Cast<URespondToEventStateComponent>() : nullptr;
            if (Responder && Responder->RespondsToInput(EAresEquippableInput::EquippableInput_Activate))
                return true;
        }
        return false;
    }

    void OnActivationInput(UAresInventory* Inventory, EAresItemSlot Slot, EAresInputState State,
                           EAresActivationInputType Type, float Time)
    {
        if (Ares::Config().Mode != Ares::EMode::Standalone || !Inventory)
            return OnActivationInputOG(Inventory, Slot, State, Type, Time);

        if (State == EAresInputState::Released && Slot == SwallowedSlot)
        {
            SwallowedSlot = (EAresItemSlot)0xFF;
            return;
        }
        if (State != EAresInputState::Pressed)
            return OnActivationInputOG(Inventory, Slot, State, Type, Time);

        const int Index = (int)Slot;
        const int Count = (int)(sizeof(Inventory->ItemSlots) / sizeof(Inventory->ItemSlots[0]));
        auto Holder = Index >= 0 && Index < Count ? Inventory->ItemSlots[Index] : nullptr;
        auto Item = Holder && Holder->Contents ? Holder->Contents->Cast<AAresEquippable>() : nullptr;
        if (!Item || Item != Inventory->CurrentEquippable || !ListeningForActivate(Item))
            return OnActivationInputOG(Inventory, Slot, State, Type, Time);

        SwallowedSlot = Slot;
        Inventory->OnInputStateChanged(EAresEquippableInput::EquippableInput_Activate,
                                       EAresInputState::Pressed, Time, Time, Time);
        Inventory->OnInputStateChanged(EAresEquippableInput::EquippableInput_Activate,
                                       EAresInputState::Released, Time, Time, Time);
        Ares::Log("input: %s was waiting to be committed, so the ability key sent Activate",
                  Item->GetName().c_str());
    }
}

void LocalAbilityActivate::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Hooking::Hook(ImageBase + 0x19AF780, OnActivationInput, OnActivationInputOG);
}

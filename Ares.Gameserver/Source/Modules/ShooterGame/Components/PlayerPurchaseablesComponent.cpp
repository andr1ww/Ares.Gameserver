#include <pch.h>
#include <Headers/Modules/ShooterGame/Components/PlayerPurchaseablesComponent.h>
INIT_MODULE(PlayerPurchaseablesComponent);

void PlayerPurchaseablesComponent::Init()
{
    AresBootLogLine("init: PlayerPurchaseablesComponent enter");
    Hooking::ExecHook(UPlayerPurchaseablesComponent::StaticClass()->FindFunction("AuthForceGivePurchasable"), AuthForceGivePurchasable);
}

bool PlayerPurchaseablesComponent::AuthForceGivePurchasable(UPlayerPurchaseablesComponent* _this, FFrame& Stack)
{
    UAresPurchasable* PurchasableClass;
    Stack.StepCompiledIn(&PurchasableClass);
    Stack.IncrementCode();

    if (!_this->CanHavePurchasable(PurchasableClass))
        return false;

    UClass* ItemClass = _this->GetItemClassForPurchasable(PurchasableClass);
    if (!ItemClass)
    {
        Ares::Log("AuthForceGivePurchasable: No item class found for purchasable %s", PurchasableClass->GetName().c_str());
        return false;
    }

    Ares::Log("AuthForceGivePurchasable called, Purchasable class: %s, Owner: %s", PurchasableClass->GetName().c_str(), _this->GetOwner()->GetName().c_str());

    return false;
}

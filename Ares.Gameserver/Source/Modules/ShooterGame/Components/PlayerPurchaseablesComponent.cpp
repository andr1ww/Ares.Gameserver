#include <pch.h>
#include <Headers/Modules/ShooterGame/Components/PlayerPurchaseablesComponent.h>
INIT_MODULE(PlayerPurchaseablesComponent);

void PlayerPurchaseablesComponent::Init()
{
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
        printf("[Runtime] AuthForceGivePurchasable: No item class found for purchasable %s\n", PurchasableClass->GetName().c_str());
        return false;
    }

    printf("[Runtime] AuthForceGivePurchasable called, Purchasable class: %s, Owner: %s\n", PurchasableClass->GetName().c_str(), _this->GetOwner()->GetName().c_str());

    return false;
}

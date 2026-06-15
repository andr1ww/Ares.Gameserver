#include <pch.h>
#include <Headers/Modules/ShooterGame/AresInventory.h>
INIT_MODULE(AresInventory);

void AresInventory::Init()
{
    Hooking::Rel32Hook(ImageBase + 0x1E8269B, AuthCreateAndAddItem);
    Hooking::Hook(ImageBase + 0x1999C60, AuthCreateAndAddEquippable);
}

AAresEquippable* AresInventory::AuthCreateAndAddEquippable(UAresInventory* _this, TSubclassOf<class AAresEquippable> EquippableClass)
{
    AAresEquippable* ClassDefaultObject = EquippableClass->DefaultObject->Cast<AAresEquippable>();
    if (!ClassDefaultObject)
    {
        printf("[Runtime] AuthCreateAndAddEquippable: CDO is not an AAresEquippable for class %s\n", EquippableClass->GetName().c_str());
        return nullptr;
    }

    AAresItem* Item = AuthCreateAndAddItem(_this, EquippableClass, EInventoryTransaction::Other, ClassDefaultObject->EquippableSlot);
    if (!Item)
    {
        printf("[Runtime] AuthCreateAndAddEquippable: failed to create item for equippable class %s\n", EquippableClass->GetName().c_str());
        return nullptr;
    }

    return Item->Cast<AAresEquippable>();
}

AAresItem* AresInventory::AuthCreateAndAddItem(UAresInventory* _this, TSubclassOf<class AAresItem> ItemClass, EInventoryTransaction Transaction, EAresItemSlot TargetSlot)
{
    printf("[Runtime] AuthCreateAndAddItem called, item class: %s, Slot: %d\n", ItemClass->GetName().c_str(), (int)TargetSlot);
    AAresItem* Item = SpawnActor(ItemClass, _this->ShooterCharacterOwner->GetTransform(), _this->ShooterCharacterOwner, 
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn)->Cast<AAresItem>();
    if (!Item)
        return nullptr;

    uint8 SlotIndex = (uint8)TargetSlot;

    _this->ItemSlots[SlotIndex]->Contents = Item;
    _this->ItemSlots[SlotIndex]->SlotType = TargetSlot;
    _this->ItemSlots[SlotIndex]->OnRep_Contents();

    _this->OnRep_ItemSlots();
    _this->ShooterCharacterOwner->OnInventoryItemsChanged();

    return Item;
}
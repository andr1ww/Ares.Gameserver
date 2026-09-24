#pragma once
#include "pch.h"

class AresInventory
{
public:
    static void Init();
    static int RefillAbilities(AShooterCharacter* Pawn);
    static void TickPossessedPawn();

    static bool InfiniteAbilities();
    static void SetInfiniteAbilities(bool bOn);

private:
    static AAresEquippable* AuthCreateAndAddEquippable(UAresInventory* _this, TSubclassOf<class AAresEquippable> EquippableClass);
    static AAresItem* AuthCreateAndAddItem(UAresInventory* _this, TSubclassOf<class AAresItem> ItemClass, EInventoryTransaction Transaction, EAresItemSlot TargetSlot);
};

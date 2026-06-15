#pragma once
#include "pch.h"

class AresInventory
{
public:
    static void Init();

private:
    static AAresEquippable* AuthCreateAndAddEquippable(UAresInventory* _this, TSubclassOf<class AAresEquippable> EquippableClass);
    static AAresItem* AuthCreateAndAddItem(UAresInventory* _this, TSubclassOf<class AAresItem> ItemClass, EInventoryTransaction Transaction, EAresItemSlot TargetSlot);
};
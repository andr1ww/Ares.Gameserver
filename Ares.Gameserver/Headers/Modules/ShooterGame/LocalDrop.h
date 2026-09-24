#pragma once
#include <string>

namespace LocalDrop
{
    void Init();

    bool Drop(class UAresInventory* Inventory, class AAresEquippable* Weapon, std::string& Report,
              class AAresOnGroundEquippable** OutPickup = nullptr);

    // AuthPickup is stripped too
    bool Pickup(class AShooterCharacter* Character, class AAresOnGroundEquippable* Ground,
                std::string& Report);
}

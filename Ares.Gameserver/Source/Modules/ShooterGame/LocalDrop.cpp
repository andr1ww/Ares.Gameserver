#include <pch.h>
#include <Headers/Modules/ShooterGame/LocalDrop.h>
#include <cmath>

// AuthDropEquippable and AuthDrop are stubbed in the client
namespace LocalDrop { void Init(); }
INIT_MODULE(LocalDrop);

namespace
{
    void (*OnDropInputOG)(UAresInventory*) = nullptr;

    constexpr float Pi = 3.14159265358979f;

    // never loaded offline, soft ref on ShooterGameGlobals
    UAresEquippableGlobals* EquippableGlobals()
    {
        static UAresEquippableGlobals* Known = nullptr;
        if (Known && Known->DroppedEquippable) return Known;
        for (auto Object : GetObjectsOfClass<UAresEquippableGlobals>())
            if (Object && Object->DroppedEquippable)
                return Known = Object;

        auto Globals = UShooterBlueprintLibrary::GetAresGlobals();
        if (!Globals) return nullptr;
        if (Globals->CachedEquippableGlobals && Globals->CachedEquippableGlobals->DroppedEquippable)
            return Known = Globals->CachedEquippableGlobals;

        const auto Path = Globals->EquippableGlobalsAsset.ObjectID.AssetPathName.GetRawString();
        if (Path.empty()) return nullptr;
        std::string Full = Path;
        if (Full.size() < 2 || Full.compare(Full.size() - 2, 2, "_C"))
            Full += "_C";
        const std::wstring Wide(Full.begin(), Full.end());
        auto Class = FindObject<UClass>(Wide.c_str());
        auto Loaded = Class && Class->DefaultObject
                          ? Class->DefaultObject->Cast<UAresEquippableGlobals>() : nullptr;
        if (Loaded)
        {
            Globals->CachedEquippableGlobals = Loaded;
            Ares::Log("drop: loaded the equippable globals from %s", Full.c_str());
        }
        return Known = Loaded;
    }

    AAresEquippable* NextBest(UAresInventory* Inventory, AAresEquippable* Leaving)
    {
        const EAresItemSlot Order[] = {EAresItemSlot::Primary, EAresItemSlot::Secondary,
                                       EAresItemSlot::Melee};
        for (auto Slot : Order)
            for (auto Item : Inventory->GetEquippables())
                if (Item && Item != Leaving && Item->EquippableSlot == Slot)
                    return Item;
        return nullptr;
    }

    void OnDropInput(UAresInventory* Inventory)
    {
        if (Ares::Config().Mode != Ares::EMode::Standalone)
        {
            OnDropInputOG(Inventory);
            return;
        }
        static bool Busy = false;
        FReentrancyGuard Guard(Busy);
        if (!Guard.Entered()) return;
        // fires more than once per press
        static double Last = -1.0;
        const double Now = Ares::Seconds();
        if (Now - Last < 0.4) return;
        Last = Now;
        std::string Report;
        if (!LocalDrop::Drop(Inventory, Inventory ? Inventory->DesiredEquippable : nullptr, Report))
            Ares::Log("drop: %s", Report.c_str());
    }
}

void LocalDrop::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Hooking::Hook(ImageBase + 0x19AF840, OnDropInput, OnDropInputOG); // OnDropEquippableInput, thunk at 0x1E848D0
    Ares::Log("drop: the drop input is handled locally");
}

bool LocalDrop::Drop(UAresInventory* Inventory, AAresEquippable* Weapon, std::string& Report,
                     AAresOnGroundEquippable** OutPickup)
{
    if (!Inventory || !Weapon) { Report = "nothing to drop"; return false; }
    auto Character = Inventory->ShooterCharacterOwner;
    if (!Character) { Report = "the inventory has no character"; return false; }
    if (!Weapon->bCanBeDropped) { Report = Weapon->GetName() + " cannot be dropped"; return false; }

    auto Globals = EquippableGlobals();
    auto GroundClass = Globals ? Globals->OnGroundEquippable.Get() : nullptr;
    if (!GroundClass) { Report = "no on-ground equippable class in the equippable globals"; return false; }

    // swap first, after the slot is cleared you end up unarmed
    auto Next = NextBest(Inventory, Weapon);
    if (Next)
        Inventory->SetDesiredEquippable(Next, EEquipSpeed::Fast, EEquipSource::RemoveItem);

    const int32 SlotCount = (int32)(sizeof(Inventory->ItemSlots) / sizeof(Inventory->ItemSlots[0]));
    for (int32 Index = 0; Index < SlotCount; ++Index)
    {
        auto Slot = Inventory->ItemSlots[Index];
        if (!Slot || Slot->Contents != Weapon) continue;
        Slot->RemoveItem(Weapon);
        Slot->OnRep_Contents();
    }
    Inventory->OnRep_ItemSlots();
    Weapon->InInventory = EEquippableInInventory::No;
    Character->OnInventoryItemsChanged();

    const FRotator Aim = Character->Controller ? Character->Controller->GetControlRotation()
                                               : Character->K2_GetActorRotation();
    const float Yaw = Aim.Yaw * Pi / 180.f, Pitch = Aim.Pitch * Pi / 180.f;
    const FVector Forward{cosf(Pitch) * cosf(Yaw), cosf(Pitch) * sinf(Yaw), sinf(Pitch)};
    const FVector Eye = Character->GetPawnViewLocation();
    const FVector Ahead{Eye.X + Forward.X * 110.f, Eye.Y + Forward.Y * 110.f, Eye.Z};
    FVector Ground{Ahead.X, Ahead.Y, Character->K2_GetActorLocation().Z - 80.f};

    TArray<AActor*> Ignore{};
    Ignore.Add(Character);
    Ignore.Add(Weapon);
    FHitResult Hit{};
    const FVector Down{Ahead.X, Ahead.Y, Ahead.Z - 400.f};
    if (UKismetSystemLibrary::LineTraceSingle(Character, Ahead, Down, ETraceTypeQuery::TraceTypeQuery1,
                                              false, Ignore, EDrawDebugTrace::None, &Hit, true,
                                              FLinearColor{}, FLinearColor{}, 0.f))
        Ground = Hit.Location;
    Ignore.Free();
    Ground.Z += 4.f;

    // AuthInitialize is stripped, OnRep_MyEquippable still works
    auto Spawned = SpawnActor(GroundClass, Ground, FRotator{0.f, Aim.Yaw, 0.f}, nullptr,
                              ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    auto Pickup = Spawned ? Spawned->Cast<AAresOnGroundEquippable>() : nullptr;
    if (!Pickup) { Report = "could not spawn " + GroundClass->GetName(); return false; }

    Weapon->MyPawn = nullptr;
    Weapon->AttributeOwner = nullptr;
    Weapon->SetOwner(Pickup);
    Pickup->LastOwner = Character;
    Pickup->MyEquippable = Weapon;
    Pickup->OnRep_MyEquippable();
    if (OutPickup) *OutPickup = Pickup;

    Report = "dropped " + Weapon->GetName();
    Ares::Log("drop: %s, now holding %s", Report.c_str(), Next ? Next->GetName().c_str() : "nothing");
    return true;
}

bool LocalDrop::Pickup(AShooterCharacter* Character, AAresOnGroundEquippable* Ground,
                       std::string& Report)
{
    if (!Character || !Ground) { Report = "nothing to pick up"; return false; }
    auto Inventory = Character->Inventory;
    auto Weapon = Ground->MyEquippable;
    if (!Inventory || !Weapon) { Report = "the pickup is empty"; return false; }

    const int32 SlotIndex = (int32)Weapon->EquippableSlot;
    const int32 SlotCount = (int32)(sizeof(Inventory->ItemSlots) / sizeof(Inventory->ItemSlots[0]));
    if (SlotIndex < 0 || SlotIndex >= SlotCount || !Inventory->ItemSlots[SlotIndex])
    {
        Report = "no slot " + std::to_string(SlotIndex) + " to put it in";
        return false;
    }

    auto Slot = Inventory->ItemSlots[SlotIndex];
    if (Slot->Contents && Slot->Contents != Weapon)
    {
        std::string Ignored;
        Drop(Inventory, Slot->Contents->Cast<AAresEquippable>(), Ignored);
    }

    // input is dead without MyPawn
    Weapon->MyPawn = Character;
    Weapon->AttributeOwner = Character;
    Weapon->InInventory = EEquippableInInventory::Yes;
    Weapon->SetOwner(Character);

    Ground->MyEquippable = nullptr;
    Ground->K2_DestroyActor();

    if (!Slot->AddItem(Weapon)) Slot->Contents = Weapon;
    Slot->SlotType = Weapon->EquippableSlot;
    Slot->OnRep_Contents();
    Inventory->OnRep_ItemSlots();
    Character->OnInventoryItemsChanged();
    Inventory->SetDesiredEquippable(Weapon, EEquipSpeed::Normal, EEquipSource::GrantEquippable);

    Report = "picked up " + Weapon->GetName();
    Ares::Log("drop: %s", Report.c_str());
    return true;
}

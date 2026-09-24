#include <pch.h>
#include <Headers/Modules/ShooterGame/AresInventory.h>
INIT_MODULE(AresInventory);

void AresInventory::Init()
{
    AresBootLogLine("init: AresInventory enter");
    // dont rel32 hook AuthCreateAndAddItem (0x1E8269B), crashes the loadout
    Hooking::Hook(ImageBase + 0x1999C60, AuthCreateAndAddEquippable);
}

AAresEquippable* AresInventory::AuthCreateAndAddEquippable(UAresInventory* _this, TSubclassOf<class AAresEquippable> EquippableClass)
{
    if (!_this || !EquippableClass || !EquippableClass->DefaultObject)
        return nullptr;
    AAresEquippable* ClassDefaultObject = EquippableClass->DefaultObject->Cast<AAresEquippable>();
    if (!ClassDefaultObject)
    {
        Ares::Log("AuthCreateAndAddEquippable: CDO is not an AAresEquippable for class %s", EquippableClass->GetName().c_str());
        return nullptr;
    }

    AAresItem* Item = AuthCreateAndAddItem(_this, EquippableClass, EInventoryTransaction::Other, ClassDefaultObject->EquippableSlot);
    if (!Item)
    {
        Ares::Log("AuthCreateAndAddEquippable: failed to create item for equippable class %s", EquippableClass->GetName().c_str());
        return nullptr;
    }

    return Item->Cast<AAresEquippable>();
}

AAresItem* AresInventory::AuthCreateAndAddItem(UAresInventory* _this, TSubclassOf<class AAresItem> ItemClass, EInventoryTransaction Transaction, EAresItemSlot TargetSlot)
{
    if (!_this || !ItemClass)
        return nullptr;

    if (!_this->ShooterCharacterOwner)
        return nullptr;

    AresBootLogLine("AuthCreateAndAddItem enter");

    // some slots are null, crashes otherwise
    const int32 SlotCount = (int32)(sizeof(_this->ItemSlots) / sizeof(_this->ItemSlots[0]));
    const int32 SlotIndex = (int32)TargetSlot;
    if (SlotIndex < 0 || SlotIndex >= SlotCount || !_this->ItemSlots[SlotIndex])
    {
        Ares::Log("AuthCreateAndAddItem: no slot %d for %s (inventory has %d slots)",
                  SlotIndex, ItemClass->GetName().c_str(), SlotCount);
        return nullptr;
    }

    AActor* Spawned = SpawnActor(ItemClass, _this->ShooterCharacterOwner->GetTransform(), _this->ShooterCharacterOwner,
        ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
    AAresItem* Item = Spawned ? Spawned->Cast<AAresItem>() : nullptr;
    if (!Item)
        return nullptr;

    // owner isnt enough, stubbed server funcs never set these so abilities break
    Item->MyPawn = _this->ShooterCharacterOwner;
    Item->InInventory = EEquippableInInventory::Yes;
    if (auto Equippable = Item->Cast<AAresEquippable>())
    {
        Equippable->AttributeOwner = _this->ShooterCharacterOwner;
        if (Ares::Config().Mode == Ares::EMode::Standalone)
        {
            // stripped grant skips forced attachments (sights, scopes)
            const std::string Suffix = "." + Equippable->Class->GetName();
            for (auto Asset : GetObjectsOfClass<UEquippableDataAsset>())
            {
                const auto Path = Asset->Equippable.ObjectID.AssetPathName.ToString();
                if (Path.size() < Suffix.size() || Path.compare(Path.size() - Suffix.size(), Suffix.size(), Suffix)) continue;
                for (auto Attachment : Asset->ForcedAttachments)
                {
                    auto Class = Attachment.Get();
                    auto Data = Class && Class->DefaultObject ? Class->DefaultObject->Cast<UEquippableAttachmentDataAsset>() : nullptr;
                    auto ComponentClass = Data ? Data->Attachment.Get() : nullptr;
                    if (!ComponentClass || Equippable->GetComponentByClass(ComponentClass)) continue;
                    UContentLibrary::AddWeaponAttachment(Equippable, Data->Attachment);
                    if (Equippable->GetComponentByClass(ComponentClass))
                    {
                        Equippable->AttachmentDataAssets.Add(Data);
                        Ares::Log("inventory: added mandatory %s to %s", ComponentClass->GetName().c_str(), Equippable->GetName().c_str());
                    }
                }
                break;
            }
        }
        // snipers dont hide 1p when scoped in this build
        if (Ares::Config().Mode == Ares::EMode::Standalone)
            for (auto Class = (UStruct*)Equippable->Class; Class; Class = Class->Super)
                if (Class->GetName() == "Gun_Sniper_C" || Class->GetName() == "LeverSniperRifle_C")
                {
                    Equippable->bHideFirstPersonWhileZoomed = true;
                    Equippable->ZoomInHideOption = EZoomInHideOption::WhenFullyZoomedIn;
                    Equippable->ZoomOutShowOption = EZoomOutShowOption::WhenFullyZoomedOut;
                    break;
                }
    }

    _this->ItemSlots[SlotIndex]->Contents = Item;
    _this->ItemSlots[SlotIndex]->SlotType = TargetSlot;
    _this->ItemSlots[SlotIndex]->OnRep_Contents();

    _this->OnRep_ItemSlots();
    _this->ShooterCharacterOwner->OnInventoryItemsChanged();

    return Item;
}

int AresInventory::RefillAbilities(AShooterCharacter* Pawn)
{
    if (!Pawn || !Pawn->Inventory) return 0;
    int Filled = 0;
    for (auto Weapon : Pawn->Inventory->GetEquippables())
    {
        if (!Weapon) continue;
        for (auto Component : Weapon->GetComponentsByClass(UEquipmentChargeComponent::StaticClass()))
        {
            auto Charge = Component->Cast<UEquipmentChargeComponent>();
            if (!Charge || Charge->MaxCharges <= 0) continue;
            // already full, hud flickers otherwise
            if (Charge->AuthResourceAmount >= Charge->MaxCharges &&
                Charge->PredictedResourceAmount >= Charge->MaxCharges)
                continue;
            Charge->AuthResourceAmount = Charge->MaxCharges;
            Charge->PredictedResourceAmount = Charge->MaxCharges;
            Charge->OnRep_AuthResourceAmount();
            Charge->NotifyResourceChanged();
            ++Filled;
        }
    }
    if (auto State = Pawn->PlayerState ? Pawn->PlayerState->Cast<AShooterPlayerState>() : nullptr)
    {
        const int Needed = State->GetNumPointsNeededForUltimate();
        if (Needed > 0 && State->NumUltimatePoints < Needed)
        {
            State->NumUltimatePoints = Needed;
            State->OnRep_NumUltimatePoints();
            ++Filled;
        }
        if (Filled) State->BroadcastEquippableChargeInfosChanged();
    }
    return Filled;
}

namespace
{
    bool bInfiniteAbilities = false;
}

bool AresInventory::InfiniteAbilities()
{
    return bInfiniteAbilities;
}

void AresInventory::SetInfiniteAbilities(bool bOn)
{
    bInfiniteAbilities = bOn;
}

void AresInventory::TickPossessedPawn()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    static bool Busy = false;
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered()) return;
    static double LastCheck = -1;
    const double Now = Ares::Seconds();
    if (Now - LastCheck < 0.25) return;
    LastCheck = Now;
    auto World = UWorld::GetWorld();
    auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    auto Pawn = PC && PC->Pawn ? PC->Pawn->Cast<AShooterCharacter>() : nullptr;
    static TWeakObjectPtr<AShooterCharacter> PreviousPawn;
    if (Pawn != PreviousPawn.Get())
    {
        // possession notify is stripped, interaction never ticks without it
        AShooterCharacter* Changed[] = {PreviousPawn.Get(), Pawn};
        PreviousPawn = TWeakObjectPtr<AShooterCharacter>(Pawn);
        if (Pawn)
        {
            // camera stays on the controller otherwise
            if (auto ShooterPC = PC->Cast<AShooterPlayerController>())
            {
                auto OldTarget = ShooterPC->PlayerViewTarget;
                ShooterPC->PlayerViewTarget = Pawn;
                ShooterPC->OnRep_ViewTarget(OldTarget);
            }
            if (auto State = PC->PlayerState ? PC->PlayerState->Cast<AShooterPlayerState>() : nullptr)
            {
                State->PossessedCharacter = Pawn;
                State->OnRep_PossessedCharacter();
            }
            PC->SetViewTargetWithBlend(Pawn, 0, EViewTargetBlendFunction::VTBlend_Linear, 0, false);
        }
        for (auto Character : Changed)
            if (Character)
                for (auto Component : Character->GetComponentsByClass(UInteractableUserComponent::StaticClass()))
                    Component->Cast<UInteractableUserComponent>()->PossessChanged(Character->Controller);
    }
    if (bInfiniteAbilities && Pawn)
        RefillAbilities(Pawn);

    if (!Pawn || !Pawn->Inventory || !Pawn->StartingEquippableClasses.Num()) return;
    auto Inventory = Pawn->Inventory;
    if (Inventory->GetEquippables().Num()) return;
    // AuthSpawnDefaultInventory is stripped, drones/cams need their loadout
    AAresEquippable* Preferred = nullptr;
    int Count = 0;
    for (auto Class : Pawn->StartingEquippableClasses)
        if (Class)
            if (auto Item = AuthCreateAndAddEquippable(Inventory, Class))
            {
                ++Count;
                if (!Preferred || Item->EquippableSlot == EAresItemSlot::Primary) Preferred = Item;
            }
    if (Preferred)
    {
        Inventory->SetDesiredEquippable(Preferred, EEquipSpeed::Normal, EEquipSource::GrantEquippable);
        Ares::Log("inventory: granted %d authored equippable(s) to possessed %s", Count, Pawn->GetName().c_str());
    }
}

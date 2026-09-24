#include <pch.h>
#include <Headers/Modules/ShooterGame/LocalSkins.h>
#include <Headers/Modules/ShooterGame/CustomSkins.h>
#include <algorithm>

namespace
{
    std::string Lowered(const std::string& Text)
    {
        std::string Out = Text;
        std::transform(Out.begin(), Out.end(), Out.begin(), [](unsigned char C) { return (char)tolower(C); });
        return Out;
    }

    template <typename T>
    T* LoadData(const TSoftClassPtr<UClass>& Reference)
    {
        const auto Path = Reference.ObjectID.AssetPathName.GetRawString();
        if (Path.empty() || Path == "None") return nullptr;
        const std::wstring Wide(Path.begin(), Path.end());
        auto Class = FindObject<UClass>(Wide.c_str());
        return Class && Class->DefaultObject ? Class->DefaultObject->Cast<T>() : nullptr;
    }

    UObject* AssetRegistry()
    {
        auto Helpers = UAssetRegistryHelpers::StaticClass();
        auto Get = Helpers ? Helpers->FindFunction("GetAssetRegistry") : nullptr;
        if (!Get) return nullptr;
        struct { FScriptInterface Registry; } Handle{};
        UAssetRegistryHelpers::GetDefaultObj()->ProcessEvent(Get, &Handle);
        return (UObject*)Handle.Registry.ObjectPointer;
    }

    std::vector<LocalSkins::FEntry> Build()
    {
        std::vector<LocalSkins::FEntry> Skins;
        auto Registry = AssetRegistry();
        // GetAssetsByClass is empty on the cooked registry, by path works
        auto ByPath = IAssetRegistry::StaticClass()->FindFunction("GetAssetsByPath");
        if (!Registry || !ByPath) return Skins;

        struct FParams
        {
            FName PackagePath;
            uint8 Pad_C[0x4];
            TArray<FAssetData> OutAssetData;
            bool bRecursive;
            bool bIncludeOnlyOnDiskAssets;
            bool ReturnValue;
            uint8 Pad_23[0x5];
        } Params{};
        Params.PackagePath = FName(L"/Game/Equippables/Guns");
        Params.bRecursive = true;
        Params.bIncludeOnlyOnDiskAssets = false;
        const double Started = Ares::Seconds();
        Registry->ProcessEvent(ByPath, &Params);
        Ares::Log("skins: swept %d asset(s) under /Game/Equippables/Guns in %.1fs",
                  Params.OutAssetData.Num(), Ares::Seconds() - Started);

        for (auto& Asset : Params.OutAssetData)
        {
            // ToString cuts everything before the last slash
            const std::string Name = Asset.AssetName.ToString();
            const std::string Folder = Asset.PackagePath.GetRawString();
            static const std::string Suffix = "_PrimaryAsset";
            if (Name.size() <= Suffix.size() ||
                Name.compare(Name.size() - Suffix.size(), Suffix.size(), Suffix) != 0)
                continue;
            if (Name.find("_Lv") != std::string::npos) continue;

            LocalSkins::FEntry Entry{};
            Entry.Name = Name.substr(0, Name.size() - Suffix.size());
            Entry.Folder = Folder;
            // data asset is the CDO of the generated class
            const std::string Path = Asset.ObjectPath.GetRawString() + "_C";
            Entry.Path.assign(Path.begin(), Path.end());
            Skins.push_back(std::move(Entry));
        }
        Params.OutAssetData.Free();

        std::sort(Skins.begin(), Skins.end(),
                  [](const LocalSkins::FEntry& A, const LocalSkins::FEntry& B) { return A.Name < B.Name; });
        Ares::Log("skins: %d skin data asset(s) in the registry", (int)Skins.size());
        return Skins;
    }
    // ammo can sit under state components, walk Outer
    void CopyAmmo(AAresEquippable* From, AAresEquippable* To)
    {
        std::vector<UAmmoComponent*> OldAmmo, NewAmmo;
        for (auto Ammo : GetObjectsOfClass<UAmmoComponent>())
            for (auto Outer = Ammo->Outer; Outer; Outer = Outer->Outer)
            {
                if (Outer == From) { OldAmmo.push_back(Ammo); break; }
                if (Outer == To) { NewAmmo.push_back(Ammo); break; }
            }
        for (auto Old : OldAmmo)
            for (auto New : NewAmmo)
                if (Old->Name.ComparisonIndex == New->Name.ComparisonIndex &&
                    Old->MaxAmmo == New->MaxAmmo && !!Old->ReserveComponent == !!New->ReserveComponent)
                {
                    const int Amount = Old->GetAmmo();
                    New->Ammo = Amount;
                    New->AuthResourceAmount = Amount;
                    New->PredictedResourceAmount = Amount;
                    New->OnRep_AuthResourceAmount();
                    New->NotifyResourceChanged();
                    Ares::Log("skins: preserved %s ammunition %d/%d", New->GetName().c_str(),
                              New->Ammo, New->MaxAmmo);
                    break;
                }
    }
}

const std::vector<LocalSkins::FEntry>& LocalSkins::Catalogue()
{
    static std::vector<FEntry> Skins = Build();
    return Skins;
}

std::wstring LocalSkins::FolderOf(const wchar_t* AssetPath)
{
    std::wstring Path = AssetPath ? AssetPath : L"";
    const size_t Dot = Path.find(L'.');
    if (Dot != std::wstring::npos) Path = Path.substr(0, Dot);
    const size_t Slash = Path.rfind(L'/');
    return Slash == std::wstring::npos ? Path : Path.substr(0, Slash);
}

std::vector<const LocalSkins::FEntry*> LocalSkins::For(const std::wstring& WeaponFolder)
{
    std::vector<const FEntry*> Found;
    const std::string Folder(WeaponFolder.begin(), WeaponFolder.end());
    const std::string Wanted = Lowered(Folder) + "/";
    for (const auto& Skin : Catalogue())
    {
        const std::string Where = Lowered(Skin.Folder) + "/";
        if (Where.rfind(Wanted, 0) == 0) Found.push_back(&Skin);
    }
    return Found;
}

std::string LocalSkins::Worn(AAresEquippable* Weapon)
{
    if (!Weapon) return "nothing";
    return Weapon->Class ? Weapon->Class->GetName() : "unknown";
}

bool LocalSkins::Apply(AAresEquippable*& Weapon, const std::wstring& WeaponFolder,
                       const std::string& Wanted, int Level, std::string& Report)
{
    if (!Weapon) { Report = "nothing to skin"; return false; }

    auto Available = For(WeaponFolder);
    if (Available.empty()) { Report = "no skins listed for that weapon"; return false; }

    std::string Lower = Lowered(Wanted);
    if (Lower == "elderflame") Lower = "dragon";
    if (Lower == "prime") Lower = "hypebeast";
    const FEntry* Chosen = nullptr;
    UEquippableSkinDataAsset* Data = nullptr;
    UEquippableSkinChromaDataAsset* Chroma = nullptr;
    for (auto Skin : Available)
    {
        if (Lowered(Skin->Name).find(Lower) == std::string::npos) continue;
        auto Class = FindObject<UClass>(Skin->Path.c_str());
        auto Candidate = Class && Class->DefaultObject
            ? Class->DefaultObject->Cast<UEquippableSkinDataAsset>() : nullptr;
        auto Variant = Class && Class->DefaultObject
            ? Class->DefaultObject->Cast<UEquippableSkinChromaDataAsset>() : nullptr;
        if (!Candidate && !Variant) continue;
        // standard = base gun, not a chroma
        if (Lower == "standard" && !Candidate) continue;
        if (!Chosen || Skin->Name.size() < Chosen->Name.size())
        {
            Chosen = Skin;
            Data = Candidate;
            Chroma = Variant;
        }
    }
    if (!Chosen) { Report = "no skin matching " + Wanted; return false; }

    // picked a chroma, find its parent skin
    if (!Data && Chroma)
        for (auto Skin : Available)
        {
            if (Chosen->Name.rfind(Skin->Name + "_", 0) != 0) continue;
            auto Class = FindObject<UClass>(Skin->Path.c_str());
            auto Parent = Class && Class->DefaultObject
                ? Class->DefaultObject->Cast<UEquippableSkinDataAsset>() : nullptr;
            if (!Parent) continue;
            bool Matches = Parent->DefaultChroma.ObjectID.AssetPathName.GetRawString() ==
                           std::string(Chosen->Path.begin(), Chosen->Path.end());
            for (const auto& Ref : Parent->Chromas)
                if (Ref.ObjectID.AssetPathName.GetRawString() == std::string(Chosen->Path.begin(), Chosen->Path.end()))
                    Matches = true;
            if (Matches) { Data = Parent; break; }
        }
    if (!Data) { Report = "could not load " + Chosen->Name; return false; }
    const int Top = Data->Levels.Num();
    if (Top <= 0) { Report = Chosen->Name + " has no levels"; return false; }
    if (Level <= 0 || Level > Top) Level = Top;
    auto LevelData = LoadData<UEquippableSkinLevelDataAsset>(Data->Levels[Level - 1]);
    auto AttachmentData = LevelData ? LoadData<UAresWeaponAttachmentComponent>(LevelData->SkinAttachment) : nullptr;
    if (!AttachmentData)
    {
        Report = Chosen->Name + " has no level " + std::to_string(Level);
        return false;
    }
    if (!Chroma) Chroma = LoadData<UEquippableSkinChromaDataAsset>(Data->DefaultChroma);

    // clearing doesnt undo the old skin, start from a fresh gun
    AAresEquippable* Previous = nullptr;
    auto Bag = Weapon->MyPawn ? Weapon->MyPawn->Inventory : nullptr;
    if (Weapon->SkinDataAsset && Bag)
    {
        Previous = Weapon;
        auto Replacement = Bag->AuthCreateAndAddEquippable(Weapon->Class);
        if (!Replacement) { Report = "could not create a clean weapon for skin change"; return false; }
        CopyAmmo(Previous, Replacement);
        Weapon = Replacement;
    }

    CustomSkins::Clear(Weapon);
    UContentLibrary::ClearWeaponComponents(Weapon);
    UContentLibrary::AddWeaponAttachment(Weapon, LevelData->SkinAttachment);
    UContentLibrary::ApplySkin(Weapon, Data, Chroma, Level, nullptr, 0);
    Weapon->SkinDataAsset = Data;
    Weapon->SkinLevelDataAsset = LevelData;
    Weapon->ChromaDataAsset = Chroma;

    if (Previous)
    {
        if (Bag->CurrentEquippable == Previous &&
            !Bag->SetDesiredEquippable(Weapon, EEquipSpeed::Normal, EEquipSource::GrantEquippable))
        {
            // equip refused, keep the old gun
            auto Slot = Bag->ItemSlots[(int)Previous->EquippableSlot];
            Slot->Contents = Previous;
            Slot->OnRep_Contents();
            Bag->OnRep_ItemSlots();
            Weapon->K2_DestroyActor();
            Weapon = Previous;
            Report = "finish the current weapon action before changing skins";
            return false;
        }
        // let the unequip finish
        Previous->SetLifeSpan(5.f);
    }

    int Parts = 0;
    {
        auto Found = Weapon->GetComponentsByClass(UAresWeaponAttachmentComponent::StaticClass());
        for (auto Part : Found)
            if (auto Skin = Part->Cast<UAresWeaponAttachmentComponent>())
                Ares::Log("skins:   attachment %s, %d mesh(es), data %s", Skin->GetName().c_str(),
                          Skin->AddedMeshes.Num(),
                          Skin->DataAsset ? Skin->DataAsset->GetName().c_str() : "none");
        Parts = Found.Num();
        Found.Free();
    }

    Report = Chosen->Name + " level " + std::to_string(Level) + " of " + std::to_string(Top);
    Ares::Log("skins: %s on %s (%d attachment(s)); banner %s", Report.c_str(), Weapon->GetName().c_str(), Parts,
              Weapon->SkinKillBannerData ? Weapon->SkinKillBannerData->GetName().c_str() : "none");
    return true;
}

void LocalSkins::ListFolder(const std::string& PackagePath, bool bRecursive, int Limit)
{
    auto Registry = AssetRegistry();
    auto ByPath = Registry ? IAssetRegistry::StaticClass()->FindFunction("GetAssetsByPath") : nullptr;
    if (!ByPath) { Ares::Log("skins: no GetAssetsByPath"); return; }

    struct FParams
    {
        FName PackagePath;
        uint8 Pad_C[0x4];
        TArray<FAssetData> OutAssetData;
        bool bRecursive;
        bool bIncludeOnlyOnDiskAssets;
        bool ReturnValue;
        uint8 Pad_23[0x5];
    } Params{};
    const std::wstring Wide(PackagePath.begin(), PackagePath.end());
    Params.PackagePath = FName(Wide.c_str());
    Params.bRecursive = bRecursive;
    Params.bIncludeOnlyOnDiskAssets = false;
    Registry->ProcessEvent(ByPath, &Params);

    int Shown = 0;
    for (auto& Asset : Params.OutAssetData)
    {
        if (++Shown > Limit) break;
        Ares::Log("  %s (%s) at %s", Asset.AssetName.ToString().c_str(),
                  Asset.AssetClass.ToString().c_str(), Asset.ObjectPath.GetRawString().c_str());
    }
    Ares::Log("skins: %d asset(s) under %s", Params.OutAssetData.Num(), PackagePath.c_str());
    Params.OutAssetData.Free();
}

AAresEquippable* LocalSkins::Replace(AAresEquippable* Weapon)
{
    auto Bag = Weapon && Weapon->MyPawn ? Weapon->MyPawn->Inventory : nullptr;
    if (!Bag) return nullptr;
    auto Replacement = Bag->AuthCreateAndAddEquippable(Weapon->Class);
    if (!Replacement) return nullptr;
    CopyAmmo(Weapon, Replacement);

    if (Bag->CurrentEquippable == Weapon &&
        !Bag->SetDesiredEquippable(Replacement, EEquipSpeed::Normal, EEquipSource::GrantEquippable))
    {
        auto Slot = Bag->ItemSlots[(int)Weapon->EquippableSlot];
        Slot->Contents = Weapon;
        Slot->OnRep_Contents();
        Bag->OnRep_ItemSlots();
        Replacement->K2_DestroyActor();
        return nullptr;
    }
    Weapon->SetLifeSpan(5.f);
    return Replacement;
}

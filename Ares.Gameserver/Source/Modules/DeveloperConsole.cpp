#include <pch.h>
#include <Headers/Modules/DeveloperConsole.h>
#include <Headers/Modules/ShooterGame/AresInventory.h>
#include <Headers/Modules/ShooterGame/LocalSkins.h>
#include <Headers/Modules/ShooterGame/CustomSkins.h>
#include <Headers/Modules/ShooterGame/LocalTeams.h>
#include <Headers/Modules/ShooterGame/LocalBots.h>
#include <Headers/Modules/ShooterGame/MapSurvey.h>
#include <Headers/Modules/ShooterGame/TacticalMap.h>
#include <Headers/Modules/ShooterGame/RoundReferee.h>
#include <Headers/Modules/Freecam.h>
#include <Headers/Modules/ShooterGame/MatchFlow.h>
#include <atomic>
#include <mutex>
#include <sstream>
#include <cstdarg>
#include <unordered_map>
#include <unordered_set>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace
{
    std::mutex QueueMutex;
    std::vector<std::string> Commands, Output;
    std::atomic<bool> HasCommands{false};
    HWND Panel = nullptr, Transcript = nullptr, Entry = nullptr, GameWindow = nullptr;
    WNDPROC EditProc = nullptr;
    std::vector<std::string> History;
    size_t HistoryIndex = 0;
    constexpr UINT kSubmit = WM_APP + 1;
    bool TraceEnabled = false;
    bool SpeedEnabled = false;
    AShooterCharacter* TracedPawn = nullptr;
    std::unordered_map<UStateMachineComponent*, UStateComponent*> LastStates;

    std::atomic<bool> SpyEnabled{false};
    std::mutex SpyMutex;
    std::unordered_set<UFunction*> SpySeen;

    // spam that fires every frame
    bool IsSpyNoise(const std::string& FullName)
    {
        static const char* Noise[] = {
            "Tick", "OnRep_", "BlueprintUpdateAnimation", "GetOwner", "Update",
            "K2_Get", "IsValid", "GetPlayer", "OnMouse", "SetVisibility",
            "GetVisibility", "OnPaint", "SequenceEvent", "AnimNotify",
        };
        for (const char* Pattern : Noise)
            if (FullName.find(Pattern) != std::string::npos)
                return true;
        return false;
    }

    std::string Name(UObject* Object) { return Object ? Object->GetName() : "<none>"; }

    void Reply(const char* Format, ...)
    {
        char Buffer[2048];
        va_list Args;
        va_start(Args, Format);
        vsnprintf(Buffer, sizeof(Buffer), Format, Args);
        va_end(Args);
        Ares::Log("console: %s", Buffer);
        std::lock_guard<std::mutex> Lock(QueueMutex);
        Output.emplace_back(Buffer);
    }

    AShooterCharacter* LocalPawn()
    {
        auto World = UWorld::GetWorld();
        auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
        return PC && PC->Pawn ? PC->Pawn->Cast<AShooterCharacter>() : nullptr;
    }

    void Status(AShooterCharacter* Pawn)
    {
        auto Inventory = Pawn->Inventory;
        Reply("Pawn %s | current %s | desired %s", Name(Pawn).c_str(),
              Name(Inventory->CurrentEquippable).c_str(), Name(Inventory->DesiredEquippable).c_str());
        auto ASC = Pawn->AbilitiesAndBuffs;
        if (ASC)
            Reply("Ability system: specs=%d inhibited=%d suppressed=%d owner=%s avatar=%s",
                  ASC->ActivatableAbilities.Items.Num(), ASC->UserAbilityActivationInhibited,
                  ASC->bSuppressGrantAbility, Name(ASC->OwnerActor).c_str(), Name(ASC->AvatarActor).c_str());
        for (auto Slot : Inventory->ItemSlots)
        {
            auto Weapon = Slot && Slot->Contents ? Slot->Contents->Cast<AAresEquippable>() : nullptr;
            if (!Weapon) continue;
            // 0 = usable, not a bool
            const char* CanUseNames[] = {"yes", "no", "not-enough-charges"};
            const int CanUse = (int)Weapon->CanUse();
            EAresEquippableRestrictionType Restriction = EAresEquippableRestrictionType::None;
            const bool Permitted = Pawn->IsEquippablePermitted(Weapon->Class, &Restriction);
            const char* RestrictionNames[] = {"None", "CantSwitchEquippables", "PrimaryWeaponsDisabled",
                                              "HeavyWeaponsDisabled", "WeaponsDisabled", "AbilityDisabled"};
            Reply("slot %d: %s | in-inventory=%d can-use=%s charges=%d/%d | owner=%s pawn=%s",
                  (int)Slot->SlotType, Name(Weapon).c_str(), (int)Weapon->InInventory,
                  CanUse >= 0 && CanUse < 3 ? CanUseNames[CanUse] : "?",
                  Weapon->GetCurrentCharges(), Weapon->GetMaxCharges(),
                  Name(Weapon->Owner).c_str(), Name(Weapon->MyPawn).c_str());
            Reply("  permitted=%d restriction=%s", (int)Permitted,
                  (int)Restriction < 6 ? RestrictionNames[(int)Restriction] : "?");
            Reply("  local=%d slot-disabled=%d primary-disabled=%d", Weapon->EquippableIsLocallyControlled(),
                  Weapon->EquippableSlotIsDisabled(), Weapon->InputIsDisabled(EAresEquippableInput::EquippableInput_Primary_Trigger,
                  EAresEquippableInputAction::EquippableInputAction_WentDown, false));
            for (auto Machine : Weapon->EquippableStateMachines)
                if (Machine)
                    Reply("  %s: server=%s predicted=%s states=%d", Name(Machine).c_str(),
                          Name(Machine->AuthCurrentState.CurrentState).c_str(),
                          Name(Machine->ClientPredictedCurrentState.CurrentState).c_str(), Machine->States.Num());
        }
        for (const char* FunctionName : {"AuthSetOwnerTo", "ReceiveClientSetOwningCharacter", "ReceiveAuthSetOwningCharacter"})
            if (auto Function = AAresItem::StaticClass()->FindFunction(FunctionName))
                Reply("  %s exec=client+0x%llX", FunctionName, (unsigned long long)((uintptr_t)Function->ExecFunction - ImageBase));
    }

    void ReportQueue(UStateComponent* State)
    {
        auto Script = State ? State->Cast<UScriptStateComponent>() : nullptr;
        if (!Script)
            return;
        const FQueuedStateTransition& Queue = Script->QueuedStateTransition;
        Reply("    queued: set=%d next=%s time-remaining=%.3f", (int)Queue.bIsSet,
              Name(Queue.QueuedNextState).c_str(), Queue.TimeRemaining);
        // BP_RespondToInput vtable+0x4E0
        Reply("    class=%s respond-impl=client+0x%llX", State->Class->GetName().c_str(),
              (unsigned long long)((uintptr_t)State->VTable[0x4E0 / 8] - ImageBase));

        if (auto Targeting = State->Cast<ULineTargetingStateComponent>())
        {
            const FHitResult Hit = Targeting->GetTargetingResult();
            Reply("    targeting: valid=%d range=%.0f blocking-hit=%d distance=%.0f at %.0f %.0f %.0f",
                  (int)Targeting->TargetingIsValid(), Targeting->TargetingRange,
                  (int)Hit.bBlockingHit, Hit.Distance,
                  Hit.ImpactPoint.X, Hit.ImpactPoint.Y, Hit.ImpactPoint.Z);
        }
    }

    struct WeaponEntry { const char* Name; const wchar_t* Path; };
    const WeaponEntry Weapons[] = {
        {"classic",  L"/Game/Equippables/Guns/Sidearms/BasePistol/BasePistol.BasePistol_C"},
        {"frenzy",   L"/Game/Equippables/Guns/Sidearms/AutoPistol/AutomaticPistol.AutomaticPistol_C"},
        {"ghost",    L"/Game/Equippables/Guns/Sidearms/Luger/LugerPistol.LugerPistol_C"},
        {"sheriff",  L"/Game/Equippables/Guns/Sidearms/Revolver/RevolverPistol.RevolverPistol_C"},
        {"shorty",   L"/Game/Equippables/Guns/Sidearms/Slim/SawedOffShotgun.SawedOffShotgun_C"},
        {"bucky",    L"/Game/Equippables/Guns/Shotguns/PumpShotgun/PumpShotgun.PumpShotgun_C"},
        {"judge",    L"/Game/Equippables/Guns/Shotguns/AutoShotgun/AutomaticShotgun.AutomaticShotgun_C"},
        {"spectre",  L"/Game/Equippables/Guns/SubMachineGuns/MP5/SubMachineGun_MP5.SubMachineGun_MP5_C"},
        {"stinger",  L"/Game/Equippables/Guns/SubMachineGuns/Vector/Vector.Vector_C"},
        {"bulldog",  L"/Game/Equippables/Guns/Rifles/Burst/AssaultRifle_Burst.AssaultRifle_Burst_C"},
        {"phantom",  L"/Game/Equippables/Guns/Rifles/Carbine/AssaultRifle_ACR.AssaultRifle_ACR_C"},
        {"vandal",   L"/Game/Equippables/Guns/Rifles/AK/AssaultRifle_AK.AssaultRifle_AK_C"},
        {"guardian", L"/Game/Equippables/Guns/SniperRifles/DMR/DMR.DMR_C"},
        {"marshal",  L"/Game/Equippables/Guns/SniperRifles/Leversniper/LeverSniperRifle.LeverSniperRifle_C"},
        {"operator", L"/Game/Equippables/Guns/SniperRifles/Boltsniper/BoltSniper.BoltSniper_C"},
        {"ares",     L"/Game/Equippables/Guns/HvyMachineGuns/LMG/LightMachineGun.LightMachineGun_C"},
        {"odin",     L"/Game/Equippables/Guns/HvyMachineGuns/HMG/HeavyMachineGun.HeavyMachineGun_C"},
    };


    const WeaponEntry* WeaponEntryFor(const std::string& Name)
    {
        for (auto& Entry : Weapons) if (Name == Entry.Name) return &Entry;
        return nullptr;
    }

    const WeaponEntry* WeaponEntryForClass(UClass* Class)
    {
        if (!Class) return nullptr;
        const std::string ClassName = Class->GetName();
        for (auto& Entry : Weapons)
        {
            const std::wstring Path = Entry.Path;
            const size_t Dot = Path.rfind(L'.');
            if (Dot == std::wstring::npos) continue;
            const std::wstring Tail = Path.substr(Dot + 1);
            if (std::string(Tail.begin(), Tail.end()) == ClassName) return &Entry;
        }
        return nullptr;
    }

    // nearest match, first match was usually a template at 0,0,0
    AActor* FindActor(const std::string& Needle, AActor* Ignore)
    {
        auto Pawn = LocalPawn();
        const FVector Eye = Pawn ? Pawn->K2_GetActorLocation() : FVector{};
        AActor* Best = nullptr;
        double BestDistance = 0.0;
        for (AActor* Actor : GetObjectsOfClass<AActor>())
        {
            if (!Actor || Actor->IsDefaultObject() || !Actor->Class || Actor == Ignore)
                continue;
            std::string Haystack = Actor->Class->GetName() + " " + Actor->GetName();
            std::transform(Haystack.begin(), Haystack.end(), Haystack.begin(),
                           [](unsigned char c) { return (char)tolower(c); });
            if (Haystack.find(Needle) == std::string::npos)
                continue;
            const FVector At = Actor->K2_GetActorLocation();
            const double dx = At.X - Eye.X, dy = At.Y - Eye.Y, dz = At.Z - Eye.Z;
            const double Distance = dx * dx + dy * dy + dz * dz;
            if (!Best || Distance < BestDistance)
            {
                Best = Actor;
                BestDistance = Distance;
            }
        }
        return Best;
    }

    void Describe(AActor* Actor)
    {
        if (!Actor) { Reply("  <none>"); return; }
        const FVector At = Actor->K2_GetActorLocation();
        Reply("  %s (%s) at %.0f %.0f %.0f", Actor->GetName().c_str(),
              Actor->Class->GetName().c_str(), At.X, At.Y, At.Z);
        Reply("  hidden=%d locally-hidden=%d role=%d remote-role=%d owner=%s",
              (int)Actor->bHidden, (int)Actor->bLocallyHidden,
              (int)Actor->Role, (int)Actor->RemoteRole, Name(Actor->Owner).c_str());
        if (auto Character = Actor->Cast<AShooterCharacter>())
            Reply("  health %.0f/%d", Character->GetHealth(), Character->GetMaxHealth());
        else if (auto Object = Actor->Cast<AGameObject>())
            Reply("  health %.0f/%.0f shield %.0f alive=%d invulnerable=%d damage-handler=%s",
                  Object->GetHealth(), Object->GetMaxHealth(), Object->GetShield(),
                  (int)Object->IsAlive(), (int)Object->Invulnerable,
                  Name(Object->DamageHandler).c_str());
    }

    void DumpFields(UStruct* Type, const uint8* Base, const std::string& Prefix, std::ostream& Out, int Depth)
    {
        for (UField* Field = Type->Children; Field; Field = Field->Next)
        {
            if (!Field->Class) continue;
            const std::string Kind = Field->Class->GetName();
            if (Kind.find("Property") == std::string::npos) continue;
            auto Prop = (UProperty*)Field;
            const uint8* At = Base + Prop->Offset;
            const std::string Key = Prefix + Field->GetName();
            if (Kind == "StructProperty")
            {
                auto S = ((UStructProperty*)Prop)->Struct;
                const std::string SN = S ? S->GetName() : "?";
                if (SN == "Vector") { auto V = (const float*)At; Out << Key << " = " << V[0] << " " << V[1] << " " << V[2] << "\n"; }
                else if (SN == "Rotator") { auto V = (const float*)At; Out << Key << " = P" << V[0] << " Y" << V[1] << " R" << V[2] << "\n"; }
                else if (S && Depth < 2 && SN.rfind("AnimNode", 0) != 0) DumpFields(S, At, Key + ".", Out, Depth + 1);
                else Out << Key << " = (" << SN << ")\n";
                continue;
            }
            Out << Key << " = ";
            if (Kind == "FloatProperty") Out << *(const float*)At;
            else if (Kind == "IntProperty") Out << *(const int32*)At;
            else if (Kind == "ByteProperty" || Kind == "EnumProperty") Out << (int)*At;
            else if (Kind == "BoolProperty") { auto B = (UBoolProperty*)Prop; Out << ((At[B->ByteOffset] & B->FieldMask) ? 1 : 0); }
            else if (Kind == "NameProperty") Out << ((const FName*)At)->ToString();
            else if (Kind == "ObjectProperty") Out << Name(*(UObject* const*)At);
            else Out << "(" << Kind << ")";
            Out << "\n";
        }
    }

    void DumpProperties(UObject* Object, std::ostream& Out, UClass* StopAt)
    {
        for (UStruct* Type = Object->Class; Type && Type != StopAt; Type = Type->Super)
            DumpFields(Type, (const uint8*)Object, Type->GetName() + ".", Out, 0);
    }

    std::string AnimDump(const std::string& Filter)
    {
        auto World = UWorld::GetWorld();
        if (!World) return "animdump: no world";
        const auto Dir = std::filesystem::path(Ares::Config().LogPath).parent_path() / "bots";
        std::filesystem::create_directories(Dir);
        int Files = 0;
        for (auto C : LocalBots::Characters())
        {
            if (!C) continue;
            std::string CName = C->GetName();
            std::string Lower = CName;
            std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char c) { return (char)tolower(c); });
            const bool Player = C->Controller && C->Controller->IsA(APlayerController::StaticClass());
            if (!Filter.empty() && Lower.find(Filter) == std::string::npos && !(Filter == "player" && Player)) continue;
            std::ofstream Out(Dir / ("anim-" + CName + ".txt"));
            auto M = C->CharacterMovement;
            const FVector V = C->GetVelocity();
            Out << CName << (Player ? " (player)" : " (bot)") << " role " << (int)C->Role << " remote " << (int)C->RemoteRole
                << " locally-controlled " << (int)C->IsLocallyControlled() << "\n";
            if (M) Out << "movement mode " << (int)M->MovementMode << " velocity " << V.X << " " << V.Y << " " << V.Z
                       << " accel " << M->GetCurrentAcceleration().X << " " << M->GetCurrentAcceleration().Y
                       << " max-walk " << M->MaxWalkSpeed << "\n";
            for (auto Component : C->GetComponentsByClass(UActorComponent::StaticClass()))
                if (Component && Component->Class && Component->Class->GetName().find("AnimationState") != std::string::npos)
                {
                    Out << "\n== component " << Component->GetName() << " (" << Component->Class->GetName() << ")\n";
                    DumpProperties(Component, Out, UActorComponent::StaticClass());
                }
            for (auto Component : C->GetComponentsByClass(USkeletalMeshComponent::StaticClass()))
            {
                auto Mesh = Component ? Component->Cast<USkeletalMeshComponent>() : nullptr;
                if (!Mesh || !Mesh->AnimScriptInstance) continue;
                Out << "\n== " << Mesh->GetName() << " visible " << (int)Mesh->IsVisible() << " anim " << Mesh->AnimScriptInstance->Class->GetName() << "\n";
                DumpProperties(Mesh->AnimScriptInstance, Out, UAnimInstance::StaticClass());
            }
            ++Files;
        }
        return "animdump: wrote " + std::to_string(Files) + " file(s) to logs/bots/anim-*.txt";
    }

    void Execute(const std::string& Line)
    {
        std::istringstream Stream(Line);
        std::string Command, Argument, Extra;
        Stream >> Command >> Argument >> Extra;
        std::transform(Command.begin(), Command.end(), Command.begin(), [](unsigned char c) { return (char)tolower(c); });
        std::transform(Argument.begin(), Argument.end(), Argument.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (Command == "botorder") { Reply("%s", LocalBots::Order(Argument == "goto" && Line.find("goto") != std::string::npos ? Line.substr(Line.find("goto")) : Argument).c_str()); return; }
        if (Command == "botskill" || Command == "botawareness" || Command == "botinfo" || Command == "botaudio") { Reply("%s", LocalBots::Configure(Command, Argument).c_str()); return; }
        if (Command == "brain") { Reply("%s", LocalBots::BrainStatus().c_str()); return; }
        if (Command == "rounds") { Reply("%s", RoundReferee::Command(Line).c_str()); return; }
        std::string Rest = Line.substr(std::min(Line.size(), Line.find_first_not_of(" \t")));
        Rest = Rest.substr(std::min(Rest.size(), Rest.find_first_of(" \t")));
        Rest = Rest.substr(std::min(Rest.size(), Rest.find_first_not_of(" \t")));
        while (!Rest.empty() && (Rest.back() == '\r' || Rest.back() == '\n')) Rest.pop_back();
        if (Command == "freecam") { Reply("%s", Freecam::Command(Argument == "at" || Argument == "sens" ? Rest : Argument).c_str()); return; }
        if (Command == "perf") { Ares::Profiling = Argument != "off"; Reply("perf: %s - timings every 5 s in logs/ares.log ('perf:' lines)", Ares::Profiling ? "on" : "off"); return; }
        if (Command == "mapprobe") { Reply("%s", MapSurvey::Probe().c_str()); return; }
        if (Command == "mapscan") { Reply("%s", MapSurvey::StartScan().c_str()); return; }
        if (Command == "animdump") { Reply("%s", AnimDump(Argument).c_str()); return; }
        if (Command == "tacpath") { Reply("%s", TacticalMap::Explain(Rest).c_str()); return; }
        if (Command == "tacbake") { Reply("%s", TacticalMap::StartBake().c_str()); return; }
        if (Command == "tactical") { Reply("tactical: %s", TacticalMap::Status().c_str()); return; }
        if (Command == "infinite" || Command == "infiniteabilities")
        {
            const bool bOn = Argument != "off";
            AresInventory::SetInfiniteAbilities(bOn);
            if (bOn)
                if (auto Pawn = LocalPawn()) AresInventory::RefillAbilities(Pawn);
            Reply("infinite abilities %s", bOn ? "on: charges and the ultimate refill as they are spent"
                                               : "off");
            return;
        }
        if (Command == "speed")
        {
            // world settings clamps it, read it back
            auto World = UWorld::GetWorld();
            if (!World) { Reply("speed: no world"); return; }
            if (Argument.empty())
            {
                Reply("speed: %.3fx  game clock %.1fs, real clock %.1fs",
                      UGameplayStatics::GetGlobalTimeDilation(World),
                      UGameplayStatics::GetTimeSeconds(World),
                      UGameplayStatics::GetRealTimeSeconds(World));
                return;
            }
            const float Wanted = (float)atof(Argument.c_str());
            if (!(Wanted > 0.f)) { Reply("speed: expected a multiplier above 0, e.g. speed 0.5"); return; }
            UGameplayStatics::SetGlobalTimeDilation(World, Wanted);
            Reply("speed: asked for %.3fx, world is running at %.3fx (game clock %.1fs)", Wanted,
                  UGameplayStatics::GetGlobalTimeDilation(World),
                  UGameplayStatics::GetTimeSeconds(World));
            return;
        }
        if (Command == "assets" && !Argument.empty())
        {
            LocalSkins::ListFolder(Argument, Extra == "deep", 60);
            Reply("assets: see the log for %s", Argument.c_str());
            return;
        }
        if (Command == "content")
        {
            auto Helpers = UAssetRegistryHelpers::StaticClass();
            auto Get = Helpers ? Helpers->FindFunction("GetAssetRegistry") : nullptr;
            if (!Get) { Reply("content: no asset registry helper"); return; }
            struct { FScriptInterface Registry; } Handle{};
            UAssetRegistryHelpers::GetDefaultObj()->ProcessEvent(Get, &Handle);
            auto Registry = (UObject*)Handle.Registry.ObjectPointer;
            if (!Registry) { Reply("content: asset registry unavailable"); return; }
            // interface funcs live on the interface class
            auto Paths = IAssetRegistry::StaticClass()->FindFunction("GetAllCachedPaths");
            if (!Paths) Paths = Registry->Class->FindFunction("GetAllCachedPaths");
            if (!Paths)
            {
                Reply("content: GetAllCachedPaths missing on %s", Registry->Class->GetName().c_str());
                return;
            }
            struct { TArray<FString> Out; } Result{};
            Registry->ProcessEvent(Paths, &Result);
            int Count = 0;
            for (auto& Path : Result.Out)
            {
                std::string Text = Path.ToString(), Lower = Text;
                std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                               [](unsigned char C) { return (char)tolower(C); });
                if (!Argument.empty() && Lower.find(Argument) == std::string::npos) continue;
                if (++Count <= 80) Reply("  %s", Text.c_str());
            }
            Reply("content: %d path(s) matching '%s'", Count, Argument.c_str());
            return;
        }
        if (Command == "cdo" && !Argument.empty())
        {
            for (int Index = 0; Index < UObject::GObjects->Num(); ++Index)
            {
                auto Object = UObject::GObjects->GetByIndex(Index);
                if (!Object || !Object->IsDefaultObject()) continue;
                std::string Lower = Object->GetName();
                std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                               [](unsigned char C) { return (char)tolower(C); });
                if (Lower.find(Argument) == std::string::npos) continue;
                Reply("%s", Object->GetFullName().c_str());
                int Count = 0;
                for (auto Class = (UStruct*)Object->Class; Class; Class = Class->Super)
                for (auto Field = Class->Children; Field; Field = Field->Next)
                {
                    auto Property = Field->Cast<UProperty>();
                    if (!Property || ++Count > 120 || Property->Offset < 0 ||
                        Property->Offset + Property->ElementSize > Object->Class->Size) continue;
                    auto Data = (uint8*)Object + Property->Offset;
                    std::string Value;
                    if (auto Bool = Property->Cast<UBoolProperty>()) Value = (Data[Bool->ByteOffset] & Bool->FieldMask) ? "true" : "false";
                    else if (Property->IsA<UObjectProperty>()) Value = Name(*(UObject**)Data);
                    else if (Property->IsA<UFloatProperty>()) Value = std::to_string(*(float*)Data);
                    else if (Property->IsA<UIntProperty>()) Value = std::to_string(*(int32*)Data);
                    Reply("  +%04X %s %s = %s", Property->Offset, Property->Class->GetName().c_str(),
                          Property->GetName().c_str(), Value.c_str());
                    auto Struct = Property->Cast<UStructProperty>();
                    if (!Struct || !Struct->Struct) continue;
                    for (auto Inner = Struct->Struct->Children; Inner; Inner = Inner->Next)
                    {
                        auto Member = Inner->Cast<UProperty>();
                        if (!Member) continue;
                        auto At = Data + Member->Offset;
                        std::string Text;
                        if (auto Bool = Member->Cast<UBoolProperty>()) Text = (At[Bool->ByteOffset] & Bool->FieldMask) ? "true" : "false";
                        else if (Member->IsA<UObjectProperty>()) Text = Name(*(UObject**)At);
                        else if (Member->IsA<UFloatProperty>()) Text = std::to_string(*(float*)At);
                        else if (Member->IsA<UIntProperty>()) Text = std::to_string(*(int32*)At);
                        Reply("      .%s (%s) = %s", Member->GetName().c_str(),
                              Member->Class->GetName().c_str(), Text.c_str());
                    }
                }
                return;
            }
            Reply("cdo: nothing matching '%s'", Argument.c_str());
            return;
        }
        if (Command == "skin" && !Argument.empty())
        {
            auto Pawn = LocalPawn();
            auto Bag = Pawn ? Pawn->Inventory : nullptr;
            if (!Bag) { Reply("skin: no inventory"); return; }
            auto Held = Bag->CurrentEquippable;
            auto Gun = Held ? WeaponEntryForClass(Held->Class) : nullptr;
            if (!Gun)
            {
                Reply("skin: %s is not one of the guns I know; give it first",
                      Held ? Name(Held).c_str() : "nothing equipped");
                return;
            }
            std::string Report;
            // custom skins arent in the pak, check them first
            std::string Why;
            if (auto Custom = CustomSkins::Find(Argument, Gun->Name, Why))
            {
                CustomSkins::Apply(Held, *Custom, Report);
                Reply("skin: %s", Report.c_str());
                return;
            }
            if (!Why.empty()) { Reply("skin: %s", Why.c_str()); return; }
            const int Level = Extra.empty() ? 0 : atoi(Extra.c_str());
            LocalSkins::Apply(Held, LocalSkins::FolderOf(Gun->Path), Argument, Level, Report);
            Reply("skin: %s", Report.c_str());
            return;
        }
        if (Command == "skincheck")
        {
            auto Pawn = LocalPawn();
            auto Weapon = Pawn && Pawn->Inventory ? Pawn->Inventory->CurrentEquippable : nullptr;
            if (!Weapon) { Reply("skincheck: no equipped weapon"); return; }
            Reply("skincheck: %s locally-controlled=%d slot=%d skin=%s", Name(Weapon).c_str(),
                  (int)Weapon->EquippableIsLocallyControlled(), (int)Weapon->EquippableSlot,
                  Name(Weapon->SkinDataAsset).c_str());
            for (auto Machine : Weapon->EquippableStateMachines)
                if (Machine) Reply("skincheck: state %s", Name(Machine->AuthCurrentState.CurrentState).c_str());
            if (Argument == "guninspect")
            {
                auto Fn = Weapon->Class->FindFunction("GunInspect");
                uint8 Emote = 1;
                if (Fn) Weapon->ProcessEvent(Fn, &Emote);
            }
            for (auto Component : GetObjectsOfClass<UActorComponent>())
            {
                if (!Component || Component->IsDefaultObject() || Component->Class->GetName() != "CosmeticActionsComponent_C" ||
                    Component->GetOwner() != Pawn) continue;
                struct { uint8 Slot; bool Result; } Check{(uint8)Weapon->EquippableSlot, false};
                auto Fn = Component->Class->FindFunction("CheckFidget");
                if (Fn) Component->ProcessEvent(Fn, &Check);
                struct { bool Result; } Cooldown{};
                Fn = Component->Class->FindFunction("CosmeticOnCooldown");
                if (Fn) Component->ProcessEvent(Fn, &Cooldown);
                Reply("skincheck: inspect component=%s eligible=%d cooldown=%d", Name(Component).c_str(),
                      (int)Check.Result, (int)Cooldown.Result);
                if (Argument == "inspect")
                {
                    Fn = Component->Class->FindFunction("LocalPlayInspect");
                    if (Fn) Component->ProcessEvent(Fn, nullptr);
                }
            }
            return;
        }
        if (Command == "boxes")
        {
            for (const auto& Line : CustomSkins::Describe()) Reply("%s", Line.c_str());
            return;
        }
        if (Command == "skinpreview")
        {
            CustomSkins::PreviewMaterial(Argument);
            Reply("skinpreview: %s", Argument.c_str());
            return;
        }
        if (Command == "skinrecipe")
        {
            auto Pawn = LocalPawn();
            auto Weapon = Pawn && Pawn->Inventory ? Pawn->Inventory->CurrentEquippable : nullptr;
            if (!Weapon) { Reply("skinrecipe: no equipped weapon"); return; }
            Reply("skinrecipe: %s banner=%s", Name(Weapon).c_str(), Name(Weapon->SkinKillBannerData).c_str());
            for (auto& Entry : Weapon->EffectOverrides)
                Reply("  %s -> %s", Entry.Key().ObjectID.AssetPathName.GetRawString().c_str(),
                      Entry.Value() ? Entry.Value()->GetFullName().c_str() : "none");
            auto Particles = Weapon->GetComponentsByClass(UParticleSystemComponent::StaticClass());
            for (auto Component : Particles)
                if (auto P = Component->Cast<UParticleSystemComponent>())
                    Reply("  particle %s template=%s", P->GetName().c_str(), Name(P->Template).c_str());
            Particles.Free();
            if (Argument == "materials")
                for (auto System : GetObjectsOfClass<UParticleSystem>())
                {
                    auto Label = System->GetName();
                    if (Label.find("Hype") == std::string::npos && Label.find("RedDot") == std::string::npos) continue;
                    Reply("particles %s", System->GetFullName().c_str());
                    for (auto Emitter : System->Emitters) if (Emitter)
                    for (auto LOD : Emitter->LODLevels) if (LOD && LOD->RequiredModule)
                    {
                        auto Material = LOD->RequiredModule->Material;
                        Reply("  material %s", Name(Material).c_str());
                        for (auto MI = Material ? Material->Cast<UMaterialInstance>() : nullptr; MI; MI = MI->Parent ? MI->Parent->Cast<UMaterialInstance>() : nullptr)
                        {
                            for (auto& P : MI->VectorParameterValues) Reply("    vector %s %.2f %.2f %.2f", P.ParameterInfo.Name.ToString().c_str(), P.ParameterValue.R, P.ParameterValue.G, P.ParameterValue.B);
                            for (auto& P : MI->TextureParameterValues) Reply("    texture %s %s", P.ParameterInfo.Name.ToString().c_str(), Name(P.ParameterValue).c_str());
                            for (auto& P : MI->ScalarParameterValues) Reply("    scalar %s %.2f", P.ParameterInfo.Name.ToString().c_str(), P.ParameterValue);
                        }
                    }
                }
            return;
        }
        if (Command == "skins")
        {
            if (auto Gun = WeaponEntryFor(Argument))
            {
                const auto Folder = LocalSkins::FolderOf(Gun->Path);
                auto Available = LocalSkins::For(Folder);
                for (auto Skin : Available) Reply("  %s", Skin->Name.c_str());
                for (const auto& Skin : CustomSkins::All())
                    if (!*Skin.Weapon || Argument == Skin.Weapon)
                        Reply("  %s  -> type `skin %s`  (%s)", Skin.Label, Skin.Name, Skin.Note);
                Reply("skins: %d for %s", (int)Available.size(), Gun->Name);
                return;
            }
            if (Argument == "dump")
            {
                auto Path = std::filesystem::path(Ares::Config().LogPath).parent_path() / "skins.txt";
                std::ofstream File(Path.string());
                if (!File) { Reply("skins: cannot write %s", Path.string().c_str()); return; }
                for (auto& Gun : Weapons)
                {
                    auto Available = LocalSkins::For(LocalSkins::FolderOf(Gun.Path));
                    File << Gun.Name << " (" << Available.size() << ")\n";
                    for (auto Skin : Available) File << "  " << Skin->Name << "\n";
                    File << "\n";
                }
                File << "everything in the registry (" << LocalSkins::Catalogue().size() << ")\n";
                for (const auto& Skin : LocalSkins::Catalogue())
                    File << "  " << Skin.Name << "  (" << Skin.Folder << ")\n";
                Reply("skins: wrote %s", Path.string().c_str());
                return;
            }
            int Count = 0;
            for (const auto& Skin : LocalSkins::Catalogue())
            {
                std::string Lower = Skin.Name;
                std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                               [](unsigned char C) { return (char)tolower(C); });
                if (!Argument.empty() && Lower.find(Argument) == std::string::npos) continue;
                if (++Count <= 60) Reply("  %s  (%s)", Skin.Name.c_str(), Skin.Folder.c_str());
            }
            for (const auto& Skin : CustomSkins::All())
                Reply("  %s  -> type `skin %s`  (%s)", Skin.Label, Skin.Name, Skin.Note);
            Reply("skins: %d of %d known skin(s), plus %d of ours. 'skins <weapon>' lists one gun's.",
                  Count, (int)LocalSkins::Catalogue().size(), (int)CustomSkins::All().size());
            return;
        }
        if (Command == "killhud")
        {
            // NotifyEnemyKilled (0x1E0C890) gates on victim vtable+0x6E8 and hud+0x9C0
            using FPredicate = bool (*)(void*);
            auto World = UWorld::GetWorld();
            auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
            auto HUD = PC && PC->MyHUD ? PC->MyHUD->Cast<AShooterHUD>() : nullptr;
            Reply("killhud: hud=%p slot+0x9C0=%p", HUD, HUD ? *(void**)((uint8*)HUD + 0x9C0) : nullptr);
            // banner only shows if killer == FirstPersonModel player state
            Reply("  your player state %s show-kill-banners=%d",
                  PC && PC->PlayerState ? Name(PC->PlayerState).c_str() : "none",
                  HUD ? (int)HUD->bShowKillBanners : -1);
            for (UCoordinatedHUDModel* Model : GetObjectsOfClass<UCoordinatedHUDModel>())
            {
                if (!Model || Model->IsDefaultObject()) continue;
                auto First = Model->FirstPersonModel;
                Reply("  hud model %s first-person=%s state=%s", Name(Model).c_str(),
                      First ? Name(First).c_str() : "none",
                      First && First->GetPlayerState() ? Name(First->GetPlayerState()).c_str() : "none");
            }
            if (!Argument.empty())
                if (auto Actor = FindActor(Argument, LocalPawn()))
                {
                    auto Table = *(void***)Actor;
                    Reply("  %s at %p vtable+0x6E8 = client+0x%llX -> %d", Name(Actor).c_str(), Actor,
                          (unsigned long long)((uintptr_t)Table[0x6E8 / 8] - ImageBase),
                          (int)((FPredicate)Table[0x6E8 / 8])(Actor));
                }
            return;
        }
        if (Command == "clearbodies" || Command == "cleanup")
        {
            const int Removed = LocalBots::ClearBodies();
            Reply("clearbodies: removed %d bod%s", Removed, Removed == 1 ? "y" : "ies");
            return;
        }
        if (Command == "localhide" && !Argument.empty())
        {
            auto Actor = FindActor(Argument, LocalPawn());
            if (!Actor) { Reply("localhide: no actor matching '%s'", Argument.c_str()); return; }
            const bool bHide = Extra != "off";
            Actor->SetActorLocallyHidden(bHide, FName(L"Console"));
            Reply("localhide: %s now locally-hidden=%d", Name(Actor).c_str(), (int)Actor->bLocallyHidden);
            return;
        }
        if (Command == "call" && !Extra.empty())
        {
            auto Actor = FindActor(Argument, LocalPawn());
            auto Function = Actor ? Actor->Class->FindFunction(Extra.c_str()) : nullptr;
            if (!Function) { Reply("call: actor/function not found"); return; }
            for (auto Field = Function->Children; Field; Field = Field->Next)
                if (auto Property = Field->Cast<UProperty>(); Property && (Property->PropertyFlags & 0x80) && !(Property->PropertyFlags & 0x400))
                { Reply("call: only functions with no input parameters are supported"); return; }
            std::vector<uint8> Params(std::max(1, Function->Size), 0);
            Actor->ProcessEvent(Function, Params.data());
            Reply("call: %s on %s", Extra.c_str(), Name(Actor).c_str());
            return;
        }
        if ((Command == "refs" || Command == "bytecode") && !Argument.empty())
        {
            for (int Index = 0; Index < UObject::GObjects->Num(); ++Index)
            {
                auto Object = UObject::GObjects->GetByIndex(Index);
                auto Function = Object ? Object->Cast<UFunction>() : nullptr;
                if (!Function) continue;
                auto Full = Function->GetFullName();
                std::transform(Full.begin(), Full.end(), Full.begin(), [](unsigned char C) { return (char)tolower(C); });
                if (Full.find(Argument) == std::string::npos) continue;
                Reply("%s", Function->GetFullName().c_str());
                // UStruct::ScriptObjectReferences
                auto& References = *(TArray<UObject*>*)((uint8*)Function + 0x90);
                if (Command == "bytecode")
                {
                    auto& Script = *(TArray<uint8>*)((uint8*)Function + 0x60);
                    if (Script.Num() < 0 || Script.Num() > 1000000 || References.Num() < 0 || References.Num() > 100000) continue;
                    auto Directory = std::filesystem::path(Ares::Config().LogPath).parent_path() / "blueprints";
                    std::error_code Error;
                    std::filesystem::create_directories(Directory, Error);
                    if (Error) { Reply("bytecode: cannot create dump directory"); return; }
                    auto Base = Directory / (std::to_string(Index) + "_" + Function->GetName().substr(0, 90));
                    std::ofstream Binary(Base.string() + ".bin", std::ios::binary);
                    if (Script.Num()) Binary.write((char*)&Script[0], Script.Num());
                    std::ofstream Names(Base.string() + ".refs");
                    Names << Function->GetFullName() << '\n';
                    for (auto Reference : References)
                        if (Reference) Names << std::hex << (uintptr_t)Reference << ' ' << Reference->GetFullName() << '\n';
                    Reply("bytecode: %s (%d bytes)", Base.string().c_str(), Script.Num());
                    continue;
                }
                for (int I = 0; I < References.Num() && I < 160; ++I)
                    if (References[I]) Reply("  %s", References[I]->GetFullName().c_str());
            }
            return;
        }
        if (Command == "reveal" && !Argument.empty())
        {
            auto Actor = FindActor(Argument, LocalPawn());
            if (Actor) Actor->ClearActorLocallyHidden();
            Describe(Actor);
            return;
        }
        if (Command == "usecheck")
        {
            auto Pawn = LocalPawn();
            if (!Pawn) return;
            for (auto Component : Pawn->GetComponentsByClass(UInteractableUserComponent::StaticClass()))
            {
                auto User = Component->Cast<UInteractableUserComponent>();
                Reply("user: %s owner=%s active=%d best=%s", User->GetFullName().c_str(),
                    Name(User->OwningShooterCharacter).c_str(), User->InteractionsActive, Name(User->BestUsableComponent).c_str());
                for (auto Usable : GetObjectsOfClass<UUsableComponent>())
                {
                    auto Owner = Usable->GetOwner();
                    if (!Owner || Owner->IsDefaultObject()) continue;
                    auto Lower = Owner->GetFullName();
                    std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C) { return (char)tolower(C); });
                    if (Argument.empty() || Lower.find(Argument) == std::string::npos) continue;
                    Reply("usable: %s select=%d use=%d predicate=%d", Usable->GetFullName().c_str(),
                        (int)User->CanSelectComponent(Usable), (int)User->CanUseComponent(Usable), (int)Usable->CanUse(User));
                }
                User->FindBestUsable();
                Reply("after selection: best=%s", Name(User->BestUsableComponent).c_str());
            }
            return;
        }
        if (Command == "widgets")
        {
            for (auto Widget : GetObjectsOfClass<UUserWidget>())
            {
                if (!Widget || !Widget->GetOwningPlayer()) continue;
                auto Full = Widget->GetFullName();
                auto Lower = Full;
                std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C) { return (char)tolower(C); });
                if (!Argument.empty() && Lower.find(Argument) == std::string::npos) continue;
                Reply("widget: %s visibility=%d viewport=%d", Full.c_str(), (int)Widget->GetVisibility(), (int)Widget->IsInViewport());
            }
            return;
        }
        if (Command == "view")
        {
            auto World = UWorld::GetWorld();
            auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
            if (!PC) return;
            auto ShooterPC = PC->Cast<AShooterPlayerController>();
            Reply("view: pawn=%s engine-target=%s ares-target=%s cached=%s",
                Name(PC->Pawn).c_str(), Name(PC->GetViewTarget()).c_str(),
                Name(ShooterPC ? ShooterPC->PlayerViewTarget : nullptr).c_str(),
                Name(ShooterPC ? ShooterPC->CachedShooterCharacter : nullptr).c_str());
            auto Camera = PC->PlayerCameraManager;
            if (Camera)
            {
                const auto Location = Camera->GetCameraLocation();
                const auto Rotation = Camera->GetCameraRotation();
                Reply("camera: %s location %.0f %.0f %.0f rotation %.1f %.1f %.1f",
                    Name(Camera).c_str(), Location.X, Location.Y, Location.Z, Rotation.Pitch, Rotation.Yaw, Rotation.Roll);
            }
            if (Extra == "reset" || Argument == "reset")
            {
                PC->SetViewTargetWithBlend(PC->Pawn, 0, EViewTargetBlendFunction::VTBlend_Linear, 0, false);
                Reply("view: reapplied pawn as engine view target");
            }
            return;
        }
        if (Command == "damage")
        {
            auto Pawn = LocalPawn();
            if (!Pawn) return;
            FVector Point{};
            bool Blocking = false;
            auto Target = Pawn->SimpleTraceFromCamera(20000.0f, ECollisionChannel::ECC_Visibility, &Point, &Blocking);
            Describe(Target);
            if (Target)
            {
                float Applied = UGameplayStatics::ApplyDamage(Target, 10.0f, Pawn->Controller, Pawn, UDamageType::StaticClass());
                Reply("damage: native ApplyDamage returned %.2f", Applied);
                if (Argument == "notify" || Argument == "gun")
                    if (auto Object = Target->Cast<AGameObject>(); Object && Object->DamageHandler)
                    {
                        FNetworkedDamageResponseData Response{};
                        Response.DamageTaken = Response.DamageDealt = 10;
                        Response.EventInstigator = Pawn->Controller;
                        Response.EventInstigatorPawn = Pawn;
                        Response.DamageCauser = Pawn;
                        Response.DamageType = UDamageType::StaticClass();
                        if (Argument == "gun")
                            for (auto Class : GetObjectsOfClass<UClass>())
                                if (Class->GetName() == "DmgType_GunBase_C")
                                {
                                    Response.DamageType = Class;
                                    break;
                                }
                        Object->DamageHandler->NotifyDamage_Base(Response);
                        Reply("damage: notified with %s (diagnostic; no health subtraction)",
                              Response.DamageType->GetName().c_str());
                    }
                Describe(Target);
            }
            return;
        }
        if (Command == "hit")
        {
            auto Pawn = LocalPawn();
            if (!Pawn) { Reply("No pawn."); return; }
            FVector Point{};
            bool bBlocking = false;
            AActor* Target = Pawn->SimpleTraceFromCamera(20000.0f, ECollisionChannel::ECC_Visibility,
                                                         &Point, &bBlocking);
            const FVector Eye = Pawn->K2_GetActorLocation();
            const double dx = Point.X - Eye.X, dy = Point.Y - Eye.Y, dz = Point.Z - Eye.Z;
            Reply("hit: blocking=%d point %.0f %.0f %.0f distance %.0f", (int)bBlocking,
                  Point.X, Point.Y, Point.Z, sqrt(dx * dx + dy * dy + dz * dz));
            Describe(Target);
            return;
        }
        if (Command == "tp" && !Argument.empty())
        {
            auto World = UWorld::GetWorld();
            auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
            auto Pawn = LocalPawn();
            if (!Pawn || !PC) { Reply("No pawn."); return; }
            AActor* Target = FindActor(Argument, Pawn);
            if (!Target) { Reply("tp: no actor matching '%s'", Argument.c_str()); return; }
            const FVector To = Target->K2_GetActorLocation();
            const FVector From = Pawn->K2_GetActorLocation();
            double dx = From.X - To.X, dy = From.Y - To.Y;
            double Length = sqrt(dx * dx + dy * dy);
            if (Length < 1.0) { dx = 1.0; dy = 0.0; Length = 1.0; }
            const double Standoff = Extra.empty() ? 300.0 : atof(Extra.c_str());
            FVector Destination{(float)(To.X + dx / Length * Standoff),
                                (float)(To.Y + dy / Length * Standoff),
                                (float)(To.Z + 100.0)};
            FRotator Facing = PC->GetControlRotation();
            Facing.Pitch = 0.0f;
            Facing.Yaw = (float)(atan2(To.Y - Destination.Y, To.X - Destination.X) * 180.0 / 3.14159265358979);
            const bool Moved = Pawn->K2_TeleportTo(Destination, Facing);
            PC->SetControlRotation(Facing);
            Reply("tp: %s -> %.0f %.0f %.0f facing %s (moved=%d)", Name(Pawn).c_str(),
                  Destination.X, Destination.Y, Destination.Z, Target->GetName().c_str(), (int)Moved);
            Describe(Target);
            return;
        }
        if (Command == "patchgrid")
        {
            for (auto Patch : GetObjectsOfClass<ACellularPatch>())
            {
                if (Patch->IsDefaultObject() || Patch->bActorIsBeingDestroyed || !Patch->Instigator ||
                    !Patch->Instigator->IsLocallyControlled()) continue;
                Reply("%s built=%d grid-valid=%d nodes=%d state=%d overlap=%.1f", Patch->GetName().c_str(),
                    (int)Patch->PatchBuilt(), (int)Patch->NodeGrid.bPatchIsValid, Patch->NodeGrid.NodeArray.Num(),
                    *((uint8*)Patch + 0x5F0), Patch->PatchOverlapExtent);
                Reply("  class=%s cell=%.0f cells=%d step=%.0f/%.0f volume=%d walls=%d track=%d overlaps=%d",
                    Patch->Class->GetName().c_str(), Patch->CellSize, Patch->MaxNumberOfCells, Patch->StepUp,
                    Patch->StepDown, Patch->MinimumValidVolume, (int)Patch->bExtendIntoWalls,
                    (int)Patch->bTrackPatchOverlaps, Patch->OverlappingActors.Num());
                Reply("  grid min=%d,%d max=%d,%d line=%d z=%.0f..%.0f maxdist=%.1f",
                    Patch->NodeGrid.Min.X, Patch->NodeGrid.Min.Y, Patch->NodeGrid.Max.X, Patch->NodeGrid.Max.Y,
                    Patch->NodeGrid.LineSize, Patch->NodeGrid.LowestNode, Patch->NodeGrid.HeighestNode,
                    Patch->NodeGrid.MaxDistance);
                if (auto Pawn = LocalPawn())
                {
                    const FVector Velocity = Pawn->GetVelocity();
                    Reply("  lifespan=%.2f", Patch->GetLifeSpan());
                    Reply("  pawn speed=%.0f edge=%.0f centre=%.0f", sqrtf(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y),
                        Patch->GetDistanceToEdgeOfPatch(Pawn->K2_GetActorLocation()),
                        Patch->GetDistanceToCenterOfPatch(Pawn->K2_GetActorLocation()));
                }
            }
            return;
        }
        if (Command == "speedlog")
        {
            SpeedEnabled = Argument != "off";
            Reply("speedlog %s", SpeedEnabled ? "on" : "off");
            return;
        }
        if (Command == "patchoverlap")
        {
            for (auto Patch : GetObjectsOfClass<ACellularPatch>())
            {
                if (Patch->IsDefaultObject() || Patch->bActorIsBeingDestroyed || !Patch->PatchBuilt()) continue;
                auto Box = Patch->BoxOverlap;
                if (!Box) { Reply("%s has no overlap box", Patch->GetName().c_str()); continue; }
                Reply("%s track=%d tracked=%d", Patch->GetName().c_str(), (int)Patch->bTrackPatchOverlaps,
                    Patch->OverlappingActors.Num());
                const FVector Extent = Box->BoxExtent;
                const FVector Where = Box->K2_GetComponentLocation();
                Reply("  box extent=%.0f %.0f %.0f at %.0f %.0f %.0f overlap-events=%d collision=%d profile=%s",
                    Extent.X, Extent.Y, Extent.Z, Where.X, Where.Y, Where.Z, (int)Box->bGenerateOverlapEvents,
                    (int)Box->BodyInstance.CollisionEnabled, Box->BodyInstance.CollisionProfileName.ToString().c_str());
                TArray<AActor*> Inside;
                Box->GetOverlappingActors(&Inside, nullptr);
                for (auto Actor : Inside)
                    if (Actor) Reply("  engine reports %s inside", Actor->GetName().c_str());
                Reply("  engine reports %d actor(s) inside", Inside.Num());
                Inside.Free();
                Reply("  filter include=%d exclude=%d", Patch->OverlapFilter.IncludedClasses.Num(),
                    Patch->OverlapFilter.ExcludedClasses.Num());
            }
            return;
        }
        if (Command == "forcemodule")
        {
            // raze satchel multiplies KnockUp in place on every init
            if (auto Pawn = LocalPawn())
            {
                auto Manager = Pawn->GetComponentByClass(UForceModuleManagerComponent::StaticClass());
                if (auto Force = Manager ? Manager->Cast<UForceModuleManagerComponent>() : nullptr)
                {
                    Reply("pawn manager: %d active module(s)", Force->ForceModules.Num());
                    for (auto Live : Force->ForceModules)
                        if (Live) Reply("  active %s", Live->Class->GetName().c_str());
                }
                else Reply("pawn has no force module manager");
            }
            for (auto Module : GetObjectsOfClass<UForceModule>())
            {
                const auto ClassName = Module->Class->GetName();
                if (ClassName.find("Knockback") == std::string::npos) continue;
                std::string Line = ClassName + (Module->IsDefaultObject() ? " (default)" : "") + ":";
                for (auto Class = (UStruct*)Module->Class; Class; Class = Class->Super)
                    for (auto Field = Class->Children; Field; Field = Field->Next)
                    {
                        auto Property = Field->Cast<UProperty>();
                        if (!Property || Property->Class->GetName() != "FloatProperty") continue;
                        char Buffer[128];
                        snprintf(Buffer, sizeof(Buffer), " %s=%.1f", Property->GetName().c_str(),
                                 *(float*)((uint8*)Module + Property->Offset));
                        Line += Buffer;
                    }
                Reply("%s", Line.c_str());
            }
            return;
        }
        if (Command == "fow")
        {
            const bool Force = Argument == "force";
            int Count = 0, Hidden = 0;
            for (auto Component : GetObjectsOfClass<UFogOfWarComponent>())
            {
                auto Owner = Component->IsDefaultObject() ? nullptr : Component->GetOwner();
                if (!Owner || Owner->bActorIsBeingDestroyed) continue;
                const auto Name = Owner->GetName();
                std::string Lower = Name;
                std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                               [](unsigned char C) { return (char)tolower(C); });
                if (!Argument.empty() && !Force && Lower.find(Argument) == std::string::npos) continue;
                ++Count;
                if (Component->IsHiddenForLocalClient()) ++Hidden;
                if (Force) { Component->AddRelevancyOverride(3600.f); continue; }
                if (Count > 24) continue;
                Reply("  %s relevant=%d hidden=%d (flags %d/%d)", Name.c_str(),
                      (int)Component->IsRelevantForLocalClient(), (int)Component->IsHiddenForLocalClient(),
                      (int)Component->bOwnerIsRelevantToClient, (int)Component->bOwnerIsHiddenForClient);
            }
            Reply("fow: %d tracked actor(s), %d hidden for this client%s", Count, Hidden,
                  Force ? ", all overridden" : "");
            return;
        }
        if (Command == "showai")
        {
            // ai pawns count as locally controlled so their 3p mesh gets hidden
            auto Player = LocalPawn();
            int Count = 0;
            for (auto Character : GetObjectsOfClass<AShooterCharacter>())
            {
                if (Character->IsDefaultObject() || Character->bActorIsBeingDestroyed) continue;
                if (Character == Player || !Character->Mesh) continue;
                Reply("  %s mesh hidden=%d controller=%s", Character->GetName().c_str(),
                      (int)Character->Mesh->bHiddenInGame,
                      Character->Controller ? Character->Controller->GetName().c_str() : "<none>");
                Character->Set3PMeshVisible(true);
                Character->Mesh->SetHiddenInGame(false, true);
                Reply("    after: hidden=%d", (int)Character->Mesh->bHiddenInGame);
                ++Count;
            }
            Reply("showai: unhid %d character mesh(es)", Count);
            return;
        }
        if (Command == "fnaddr" && !Argument.empty())
        {
            int Found = 0;
            for (auto Fn : GetObjectsOfClass<UFunction>())
            {
                if (!Fn) continue;
                std::string Full = Fn->GetFullName();
                std::string Lower = Full;
                std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C) { return (char)tolower(C); });
                if (Lower.find(Argument) == std::string::npos) continue;
                const uintptr_t Exec = (uintptr_t)Fn->ExecFunction;
                Reply("fnaddr: %s exec=+0x%llX", Full.c_str(), Exec >= ImageBase ? (unsigned long long)(Exec - ImageBase) : 0ull);
                if (++Found >= 20) break;
            }
            if (!Found) Reply("fnaddr: nothing matches '%s'", Argument.c_str());
            return;
        }
        if (Command == "fnat" && !Rest.empty())
        {
            // reverse fnaddr, nearest thunk below each rva
            std::vector<std::pair<uintptr_t, UFunction*>> Thunks;
            for (auto Fn : GetObjectsOfClass<UFunction>())
                if (Fn && (uintptr_t)Fn->ExecFunction >= ImageBase) Thunks.push_back({(uintptr_t)Fn->ExecFunction - ImageBase, Fn});
            std::sort(Thunks.begin(), Thunks.end(), [](auto& A, auto& B) { return A.first < B.first; });
            std::istringstream Offsets(Rest);
            std::string Hex;
            while (Offsets >> Hex)
            {
                const uintptr_t Rva = (uintptr_t)strtoull(Hex.c_str(), nullptr, 16);
                auto It = std::upper_bound(Thunks.begin(), Thunks.end(), Rva, [](uintptr_t V, auto& T) { return V < T.first; });
                if (It == Thunks.begin()) { Reply("fnat: +%llX below every thunk", (unsigned long long)Rva); continue; }
                --It;
                Reply("fnat: +%llX = %s +0x%llX", (unsigned long long)Rva, It->second->GetFullName().c_str(), (unsigned long long)(Rva - It->first));
            }
            return;
        }
        if (Command == "memdump" && !Argument.empty())
        {
            // memdump <rva> [len]
            const uintptr_t Rva = strtoull(Argument.c_str(), nullptr, 16);
            size_t Length = Extra.empty() ? 0x400 : (size_t)strtoull(Extra.c_str(), nullptr, 0);
            Length = std::min<size_t>(Length, 0x10000);
            if (Rva == 0 || Rva + Length > 0x7000000) { Reply("memdump: outside the client image"); return; }
            const auto File = std::filesystem::path(Ares::Config().LogPath).parent_path() / ("dump-" + Argument + ".bin");
            std::ofstream Out(File, std::ios::binary);
            Out.write((const char*)(ImageBase + Rva), (std::streamsize)Length);
            Reply("memdump: %zu bytes at +0x%llX -> %s", Length, (unsigned long long)Rva, File.string().c_str());
            return;
        }
        if (Command == "stripped")
        {
            // server funcs are stripped to a ret stub in client (0x12928B0 etc)
            const uintptr_t Lowest = ImageBase, Highest = ImageBase + 0x7000000;
            const auto IsStub = [&](const uint8* Body)
            {
                if (!Body) return false;
                const uintptr_t At = (uintptr_t)Body;
                if (At < Lowest || At + 8 > Highest) return false;
                // ret, ret imm16, xor eax/al + ret
                if (Body[0] == 0xC3) return true;
                if (Body[0] == 0xC2) return true;
                if ((Body[0] == 0x31 || Body[0] == 0x32 || Body[0] == 0x33) &&
                    Body[1] == 0xC0 && Body[2] == 0xC3)
                    return true;
                if (Body[0] == 0x0F && Body[1] == 0x57 && Body[2] == 0xC0 && Body[3] == 0xC3) return true;
                return false;
            };
            int Count = 0;
            for (auto Function : GetObjectsOfClass<UFunction>())
            {
                auto Exec = (const uint8*)Function->ExecFunction;
                if (!Exec) continue;
                bool Stripped = false;
                if ((uintptr_t)Exec < Lowest || (uintptr_t)Exec + 0x130 > Highest) continue;
                for (int At = 0; At < 0x120 && !Stripped; ++At)
                {
                    if (Exec[At] != 0xE9 && Exec[At] != 0xE8) continue;
                    int32 Relative = 0;
                    memcpy(&Relative, Exec + At + 1, sizeof(Relative));
                    Stripped = IsStub(Exec + At + 5 + Relative);
                }
                if (!Stripped) continue;
                const auto Name = Function->GetFullName();
                if (!Argument.empty())
                {
                    std::string Lower = Name;
                    std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                                   [](unsigned char C) { return (char)tolower(C); });
                    if (Lower.find(Argument) == std::string::npos) continue;
                }
                if (++Count <= 150) Reply("  %s", Name.c_str());
            }
            Reply("stripped: %d function(s) resolve to the empty stub", Count);
            return;
        }
        if ((Command == "spawn" || Command == "bot") && !Argument.empty())
        {
            static const struct { const char* Agent; const wchar_t* Codename; } Roster[] = {
                {"jett", L"Wushu"},   {"viper", L"Pandemic"}, {"raze", L"Clay"},
                {"brimstone", L"Sarge"}, {"breach", L"Breach"}, {"sage", L"Thorne"},
                {"sova", L"Hunter"},  {"reyna", L"Vampire"},  {"omen", L"Wraith"},
                {"cypher", L"Gumshoe"}, {"phoenix", L"Phoenix"}, {"killjoy", L"Killjoy"},
            };
            const wchar_t* Codename = nullptr;
            for (const auto& Entry : Roster)
                if (Argument == Entry.Agent) Codename = Entry.Codename;
            if (!Codename)
            {
                std::string Known;
                for (const auto& Entry : Roster) Known += std::string(Known.empty() ? "" : ", ") + Entry.Agent;
                Reply("spawn: unknown agent '%s'. Known: %s", Argument.c_str(), Known.c_str());
                return;
            }

            auto Pawn = LocalPawn();
            auto PC = UWorld::GetWorld() ? UGameplayStatics::GetPlayerController(UWorld::GetWorld(), 0) : nullptr;
            if (!Pawn || !PC) { Reply("spawn: no local pawn yet."); return; }

            wchar_t Path[256];
            swprintf(Path, 256, L"/Game/Characters/%s/%s_PC.%s_PC_C", Codename, Codename, Codename);
            auto PawnClass = FindObject<UClass>(Path);
            if (!PawnClass) { Reply("spawn: could not load %ls", Path); return; }

            const FRotator Facing = PC->GetControlRotation();
            const float Yaw = Facing.Yaw * 3.14159265358979f / 180.0f;
            const FVector From = Pawn->K2_GetActorLocation();
            const float Distance = 500.0f;
            static int Placed = 0;
            const float Sideways = (float)((Placed++ % 5) - 2) * 150.0f;
            FTransform Where{};
            Where.Translation = {From.X + cosf(Yaw) * Distance - sinf(Yaw) * Sideways,
                                 From.Y + sinf(Yaw) * Distance + cosf(Yaw) * Sideways, From.Z + 20.0f};
            Where.Rotation = RotatorToQuat({0.0f, Facing.Yaw + 180.0f, 0.0f});
            Where.Scale3D = {1.0f, 1.0f, 1.0f};

            {
                std::istringstream Placement(Line);std::string Token;
                while(Placement>>Token)if(Token=="at") {
                    FVector Position{};
                    if(!(Placement>>Position.X>>Position.Y>>Position.Z) ||
                       !std::isfinite(Position.X)||!std::isfinite(Position.Y)||!std::isfinite(Position.Z)) {
                        Reply("spawn: at requires finite X Y Z coordinates");return;
                    }
                    Where.Translation=Position;break;
                }
            }

            auto Spawned = (AShooterCharacter*)SpawnActor(PawnClass, Where, nullptr,
                ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
            if (!Spawned) { Reply("spawn: %ls would not spawn", Path); return; }

            std::string Side = "enemy";
            LocalBots::FOptions Options{};
            {
                std::istringstream Rest(Line);
                std::string Token;
                Rest >> Token >> Token; // command, agent
                while (Rest >> Token)
                {
                    std::transform(Token.begin(), Token.end(), Token.begin(),
                                   [](unsigned char C) { return (char)tolower(C); });
                    if (Token == "enemy" || Token == "ally" || Token == "teammate" || Token == "friend" ||
                        Token == "neutral")
                        Side = Token;
                    else if (Token == "combat" || Token == "tactical") { Options.Behaviour = LocalBots::EBehaviour::Combat; if (!Options.WeaponPath) { Options.WeaponPath=Weapons[10].Path; Options.WeaponName=Weapons[10].Name; } }
                    else if (Token == "at") { float X,Y,Z; Rest>>X>>Y>>Z; }
                    else if (Token == "walk") Options.Behaviour = LocalBots::EBehaviour::Walk;
                    else if (Token == "jump") Options.Behaviour = LocalBots::EBehaviour::Jump;
                    else if (Token == "strafe") Options.Behaviour = LocalBots::EBehaviour::Strafe;
                    else if (Token == "still") Options.Behaviour = LocalBots::EBehaviour::Still;
                    else if (Token == "physics") Options.bGravity = true;
                    else if (Token == "nophysics" || Token == "static") Options.bGravity = false;
                    else if (Token == "watch" || Token == "look" || Token == "aim") Options.bWatch = true;
                    else if (Token == "armed")
                    {
                        Options.WeaponPath = Weapons[10].Path;   // vandal
                        Options.WeaponName = Weapons[10].Name;
                    }
                    else
                    {
                        const WeaponEntry* Gun = nullptr;
                        for (auto& Entry : Weapons) if (Token == Entry.Name) Gun = &Entry;
                        if (Gun) { Options.WeaponPath = Gun->Path; Options.WeaponName = Gun->Name; }
                        else Reply("  ignoring '%s'; expected enemy/ally/neutral, still/walk/jump/strafe/combat, "
                                   "physics/nophysics, watch, armed or a weapon name", Token.c_str());
                    }
                }
            }

            if (Side != "neutral")
            {
                auto World = UWorld::GetWorld();
                auto State = World && World->GameState ? World->GameState->Cast<AShooterGameState>() : nullptr;
                if (!State || State->TeamComponents.Num() < 2)
                {
                    Reply("  '%s' needs two teams; this map registers %d, so it stays neutral",
                          Side.c_str(), State ? State->TeamComponents.Num() : 0);
                }
                else
                {
                    auto Ours = LocalTeams::Assigned(Pawn);
                    if (!Ours)
                    {
                        Ours = State->TeamComponents[0].Team;
                        LocalTeams::Assign(Pawn, Ours);
                        Reply("  put you on %s first", Ours->GetTeamName().ToString().c_str());
                    }
                    UBaseTeamComponent* Theirs = Ours;
                    if (Side == "enemy")
                        for (const auto& Entry : State->TeamComponents)
                            if (Entry.Team && Entry.Team != Ours) Theirs = Entry.Team;
                    LocalTeams::Assign(Spawned, Theirs);
                    Reply("  team %s", Theirs->GetTeamName().ToString().c_str());
                }
            }

            if (auto Brain = (AAresBotController*)SpawnActor(AAresBotController::StaticClass(), Where, nullptr,
                                                             ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
            {
                Brain->Possess(Spawned);
                // no player state = no kill banner, use the gamemode's class or it crashes
                auto World = UWorld::GetWorld();
                auto Mode = World ? World->AuthorityGameMode : nullptr;
                UClass* StateClass = Mode ? Mode->PlayerStateClass : nullptr;
                if (StateClass)
                    if (auto State = (APlayerState*)SpawnActor(StateClass, Where, Brain,
                                                               ESpawnActorCollisionHandlingMethod::AlwaysSpawn))
                    {
                        *((uint8*)State + 0x3CE) &= (uint8)~8; // bIsABot
                        Brain->PlayerState = State;
                        Spawned->PlayerState = State;
                        // reassign so the player state gets the team too
                        if (auto Team = LocalTeams::Assigned(Spawned))
                            LocalTeams::Assign(Spawned, Team);
                    }
                Reply("  controller %s, player state %s", Name(Brain).c_str(),
                      Spawned->PlayerState ? Name(Spawned->PlayerState).c_str() : "none");
            }

            // arm after possess, inventory needs an owner
            if (Options.WeaponPath)
            {
                auto Class = FindObject<UClass>(Options.WeaponPath);
                auto Bag = Spawned->Inventory;
                auto Gun = Class && Bag ? Bag->AuthCreateAndAddEquippable(Class) : nullptr;
                if (Gun)
                {
                    // SetDesiredEquippable never finishes on a bot
                    Bag->AuthForceDesiredEquippable(Gun, EEquipSpeed::Instant, EEquipSource::GrantEquippable);
                    Bag->MulticastCurrentEquippableForServerOwnedActors(Gun);
                }
                Reply("  %s %s", Options.WeaponName, Gun ? "equipped" : "could not be created");
            }

            Spawned->Set3PMeshVisible(true);
            // bots never get this so their gun stays invisible
            Spawned->OnUpdateCharacterMeshes.Process(Spawned, false, true);
            LocalBots::Register(Spawned, Options);
            auto Final = UBaseTeamComponent::GetTeamComponent(Spawned);
            static const char* const Names[] = {"still", "walk", "jump", "strafe", "combat"};
            Reply("spawn: %s as %s (%s, %s, %s%s%s) at %.0f %.0f %.0f", Name(Spawned).c_str(), Argument.c_str(),
                  Final ? Final->GetTeamName().ToString().c_str() : "neutral", Names[(int)Options.Behaviour],
                  Options.bGravity ? "gravity" : "no gravity", Options.bWatch ? ", watching" : "",
                  Options.WeaponName ? ", armed" : "", Where.Translation.X, Where.Translation.Y,
                  Where.Translation.Z);
            return;
        }
        if (Command == "teams")
        {
            // only the gamestate list, sweeping all team components crashes
            auto Pawn = LocalPawn();
            auto Mine = Pawn ? UBaseTeamComponent::GetTeamComponent(Pawn) : nullptr;
            Reply("you: component=%s team=%s", Mine ? "yes" : "no",
                  Mine ? Mine->GetTeamName().ToString().c_str() : "<none>");
            auto World = UWorld::GetWorld();
            auto State = World && World->GameState ? World->GameState->Cast<AShooterGameState>() : nullptr;
            if (!State) { Reply("teams: no shooter game state"); return; }
            const int Count = State->TeamComponents.Num();
            Reply("game state lists %d team(s)", Count);
            for (int Index = 0; Index < Count; ++Index)
            {
                auto Team = State->TeamComponents[Index].Team;
                if (!Team) { Reply("  %d: <null>", Index); continue; }
                Reply("  %d: %s valid=%d role=%d enemy-to-you=%d", Index,
                      Team->GetTeamName().ToString().c_str(), (int)Team->HasValidTeam(),
                      (int)Team->GetTeamRole(), Mine ? (int)Team->IsTeamEnemy(Mine) : -1);
            }
            if (Mine)
            {
                auto Slot = [&](int Offset) -> const uint8*
                {
                    auto Table = (const uint8**)Mine->VTable;
                    return Table ? Table[Offset / 8] : nullptr;
                };
                auto Describe = [&](const char* Label, const uint8* Body)
                {
                    if (!Body) { Reply("  %s <null>", Label); return; }
                    const bool Empty = Body[0] == 0xC3 || Body[0] == 0xC2 ||
                                       (Body[1] == 0xC0 && Body[2] == 0xC3 &&
                                        (Body[0] == 0x31 || Body[0] == 0x32 || Body[0] == 0x33));
                    Reply("  %s client+0x%llX stub=%d", Label,
                          (unsigned long long)((uintptr_t)Body - ImageBase), (int)Empty);
                };
                Describe("AuthSetTeamFromTeamName", Slot(0x438));
                Describe("AuthCopyTeam", Slot(0x440));
            }
            if (!Argument.empty() && Mine)
            {
                if (Argument.find_first_not_of("0123456789") == std::string::npos)
                {
                    const int Wanted = atoi(Argument.c_str());
                    if (Wanted < 0 || Wanted >= Count) { Reply("teams: pick 0..%d", Count - 1); return; }
                    auto Team = State->TeamComponents[Wanted].Team;
                    if (!Team) { Reply("teams: slot %d is empty", Wanted); return; }
                    // both setters are stripped
                    LocalTeams::Assign(Pawn, Team);
                }
                else
                {
                    const std::wstring Wide(Argument.begin(), Argument.end());
                    const bool Took = Mine->AuthSetTeamFromTeamName(FName(Wide.c_str()));
                    Reply("teams: name '%s' accepted=%d", Argument.c_str(), (int)Took);
                }
                auto Now = UBaseTeamComponent::GetTeamComponent(Pawn);
                Reply("teams: you are now %s (valid=%d)",
                      Now ? Now->GetTeamName().ToString().c_str() : "<none>",
                      Now ? (int)Now->HasValidTeam() : 0);
            }
            return;
        }
        if (Command == "health" && !Argument.empty())
        {
            auto Actor = FindActor(Argument, LocalPawn());
            if (!Actor) { Reply("health: no actor matching '%s'", Argument.c_str()); return; }
            auto Character = Actor->Cast<AShooterCharacter>();
            auto Handler = Character ? Character->DamageHandler
                                     : (Actor->Cast<AGameObject>() ? Actor->Cast<AGameObject>()->DamageHandler
                                                                   : nullptr);
            if (!Handler) { Reply("health: %s has no damage handler", Name(Actor).c_str()); return; }
            Reply("%s alive=%d health=%.0f shield=%.0f", Name(Actor).c_str(), (int)Handler->bAlive,
                  Handler->GetRawHealth(), Handler->GetRawShield());
            for (auto Section : Handler->DamageSections)
                if (Section)
                    Reply("  section type=%d life=%.0f/%.0f", (int)Section->Type,
                          Handler->GetSectionLife(Section), Section->GetMaximumLife());
            // TakeDamage vtable+0x5B0
            if (auto Table = (const uint8**)Actor->VTable)
            {
                auto Body = Table[0x5B0 / 8];
                const bool Empty = Body && (Body[0] == 0xC3 || Body[0] == 0xC2 ||
                                            (Body[1] == 0xC0 && Body[2] == 0xC3 &&
                                             (Body[0] == 0x31 || Body[0] == 0x32 || Body[0] == 0x33)) ||
                                            (Body[0] == 0x0F && Body[1] == 0x57 && Body[2] == 0xC0));
                Reply("  TakeDamage client+0x%llX stub=%d",
                      (unsigned long long)((uintptr_t)Body - ImageBase), (int)Empty);
            }
            return;
        }
        if (Command == "alliance" && !Argument.empty())
        {
            auto Actor = FindActor(Argument, LocalPawn());
            auto Pawn = LocalPawn();
            if (!Actor || !Pawn) { Reply("alliance: need a local pawn and a target"); return; }
            auto Mine = UBaseTeamComponent::GetTeamComponent(Pawn);
            auto Theirs = UBaseTeamComponent::GetTeamComponent(Actor);
            Reply("%s: your team=%s theirs=%s", Name(Actor).c_str(),
                  Mine ? Mine->GetTeamName().ToString().c_str() : "<none>",
                  Theirs ? Theirs->GetTeamName().ToString().c_str() : "<none>");
            Reply("  GetObjectAlliance=%d (0 ally, 1 enemy, 2 neutral)",
                  (int)UBaseTeamComponent::GetObjectAlliance(Pawn, Actor));
            Reply("  IsAlly=%d IsEnemy=%d", (int)UBaseTeamComponent::IsAlly(Pawn, Actor),
                  (int)UBaseTeamComponent::IsEnemy(Pawn, Actor));
            Reply("  UseEnemyColoring=%d ShowAllyInformation=%d",
                  (int)UBaseTeamComponent::UseEnemyColoring(Actor),
                  (int)UBaseTeamComponent::ShowAllyInformation(Actor));
            if (Mine && Theirs)
                Reply("  team-to-team alliance=%d ally=%d enemy=%d", (int)Mine->GetAlliance(Theirs),
                      (int)Mine->IsTeamAlly(Theirs), (int)Mine->IsTeamEnemy(Theirs));
            return;
        }
        if (Command == "radial")
        {
            // diff vtables vs UBaseProjectileEffectComponent
            auto Base = (const uint8**)UBaseProjectileEffectComponent::GetDefaultObj()->VTable;
            auto Point = (const uint8**)UDamageProjectileEffectComponent::GetDefaultObj()->VTable;
            auto Radial = (const uint8**)URadialDamageProjectileEffectComponent::GetDefaultObj()->VTable;
            if (!Base || !Point || !Radial) { Reply("radial: no vtables"); return; }
            for (int Slot = 0; Slot < 200; ++Slot)
            {
                const bool PointDiffers = Point[Slot] != Base[Slot];
                const bool RadialDiffers = Radial[Slot] != Base[Slot];
                if (!PointDiffers && !RadialDiffers) continue;
                Reply("  slot %3d base=+0x%llX point=+0x%llX radial=+0x%llX", Slot,
                      (unsigned long long)((uintptr_t)Base[Slot] - ImageBase),
                      (unsigned long long)((uintptr_t)Point[Slot] - ImageBase),
                      (unsigned long long)((uintptr_t)Radial[Slot] - ImageBase));
            }
            Reply("radial: done (point apply is known to be +0x1D16AF0)");
            return;
        }
        if (Command == "patchtrace")
        {
            // riot has custom channels, find which one hits the floor
            auto Pawn = LocalPawn();
            if (!Pawn) { Reply("No pawn."); return; }
            const FVector Foot = Pawn->K2_GetActorLocation();
            const FVector Start{Foot.X, Foot.Y, Foot.Z + 100.f};
            const FVector End{Foot.X, Foot.Y, Foot.Z - 500.f};
            Reply("patchtrace: %.0f %.0f %.0f down 600", Foot.X, Foot.Y, Foot.Z);
            TArray<AActor*> Ignore;
            Ignore.Add(Pawn);
            for (int Type = 0; Type < 18; ++Type)
            {
                TArray<EObjectTypeQuery> Types;
                Types.Add(EObjectTypeQuery(Type));
                FHitResult Hit{};
                if (UKismetSystemLibrary::LineTraceSingleForObjects(Pawn, Start, End, Types, true, Ignore,
                        EDrawDebugTrace(0), &Hit, true, {}, {}, 0))
                    Reply("  object %2d hit z=%.0f normal=%.2f %s", Type, Hit.ImpactPoint.Z, Hit.ImpactNormal.Z,
                          Hit.Actor.Get() ? Hit.Actor.Get()->GetName().c_str() : "<none>");
                Types.Free();
            }
            for (int Channel = 0; Channel < 12; ++Channel)
            {
                FHitResult Hit{};
                if (UKismetSystemLibrary::LineTraceSingle(Pawn, Start, End, ETraceTypeQuery(Channel), true, Ignore,
                        EDrawDebugTrace(0), &Hit, true, {}, {}, 0))
                    Reply("  trace  %2d hit z=%.0f normal=%.2f %s", Channel, Hit.ImpactPoint.Z, Hit.ImpactNormal.Z,
                          Hit.Actor.Get() ? Hit.Actor.Get()->GetName().c_str() : "<none>");
            }
            for (const wchar_t* Profile : {L"BlockAll", L"WorldStatic", L"Pawn", L"BlockAllDynamic"})
            {
                FHitResult Hit{};
                if (UKismetSystemLibrary::LineTraceSingleByProfile(Pawn, Start, End, FName(Profile), true, Ignore,
                        EDrawDebugTrace(0), &Hit, true, {}, {}, 0))
                    Reply("  profile %ls hit z=%.0f normal=%.2f", Profile, Hit.ImpactPoint.Z, Hit.ImpactNormal.Z);
            }
            Ignore.Free();
            Reply("patchtrace: done");
            return;
        }
        if (Command == "attachments")
        {
            auto Pawn = LocalPawn();
            auto Item = Pawn && Pawn->Inventory ? Pawn->Inventory->CurrentEquippable : nullptr;
            if (!Item) return;
            const std::string Suffix = "." + Item->Class->GetName();
            for (auto Asset : GetObjectsOfClass<UEquippableDataAsset>())
            {
                const auto Path = Asset->Equippable.ObjectID.AssetPathName.ToString();
                if (Path.size() < Suffix.size() || Path.compare(Path.size() - Suffix.size(), Suffix.size(), Suffix)) continue;
                Reply("attachment data: %s", Asset->GetFullName().c_str());
                for (auto Attachment : Asset->ForcedAttachments)
                {
                    Reply("  forced: %s", Attachment.ObjectID.AssetPathName.ToString().c_str());
                    auto Class = Attachment.Get();
                    auto Data = Class && Class->DefaultObject ? Class->DefaultObject->Cast<UEquippableAttachmentDataAsset>() : nullptr;
                    if (Data)
                    {
                        Reply("    component: %s", Data->Attachment.ObjectID.AssetPathName.ToString().c_str());
                        auto ComponentClass = Data->Attachment.Get();
                        Reply("    present=%d", ComponentClass && Item->GetComponentByClass(ComponentClass) ? 1 : 0);
                    }
                }
                return;
            }
            Reply("No loaded equippable data asset for %s", Item->Class->GetName().c_str());
            return;
        }
        if (Command == "zoom")
        {
            auto Pawn = LocalPawn();
            auto Item = Pawn && Pawn->Inventory ? Pawn->Inventory->CurrentEquippable : nullptr;
            if (!Item) { Reply("Nothing equipped."); return; }
            Reply("%s hide-1p-while-zoomed=%d zoom-in-hide=%d zoom-out-show=%d",
                  Name(Item).c_str(), (int)Item->bHideFirstPersonWhileZoomed,
                  (int)Item->ZoomInHideOption, (int)Item->ZoomOutShowOption);
            Reply("character wants-hide-1p=%d", (int)Pawn->WasEquippableWantsToHideFirstPersonMesh);
            for (UActorComponent* Component : Item->GetComponentsByClass(UZoomComponent::StaticClass()))
            {
                auto Zoom = Component ? Component->Cast<UZoomComponent>() : nullptr;
                if (!Zoom) continue;
                if (Argument == "sync")
                {
                    Zoom->AuthZoomTransition = Zoom->ClientPredictedZoomTransition;
                    Reply("zoom: copied local predicted transition to authority for diagnosis");
                }
                Reply("  %s zoomed=%d level=%d/%d target=%d transitioning=%d cooldown=%d",
                      Name(Zoom).c_str(), (int)Zoom->IsZoomed(), Zoom->GetCurrentZoomLevel(),
                      Zoom->GetNumZoomLevels(), Zoom->GetTargetZoomLevel(),
                      (int)Zoom->IsZoomTransitioning(), (int)Zoom->IsZoomOnCooldown());
                for (auto Level : Zoom->ZoomLevelInfo)
                    Reply("    authored multiplier=%.3f default-1p-fov=%d 1p-fov=%.3f", Level.Multiplier, (int)Level.bUseDefaultFov1P, Level.Fov1P);
                const auto& Auth = Zoom->AuthZoomTransition.TransitionDescription;
                const auto& Pred = Zoom->ClientPredictedZoomTransition.TransitionDescription;
                Reply("    auth fov=%.3f 1p=%.3f predicted fov=%.3f 1p=%.3f", Auth.TargetFov, Auth.TargetFov1P, Pred.TargetFov, Pred.TargetFov1P);
            }
            struct MeshRow { const char* Label; USkeletalMeshComponent* Mesh; };
            const MeshRow Rows[] = {{"weapon 1P", Item->Mesh1P},
                                    {"weapon 3P", Item->Mesh3P},
                                    {"character 1P", Pawn->Mesh1P}};
            for (const MeshRow& Row : Rows)
            {
                if (!Row.Mesh) { Reply("  %-12s <none>", Row.Label); continue; }
                Reply("  %-12s %s visible=%d hidden-in-game=%d owner-no-see=%d only-owner-see=%d",
                      Row.Label, Name(Row.Mesh).c_str(), (int)Row.Mesh->bVisible,
                      (int)Row.Mesh->bHiddenInGame, (int)Row.Mesh->bOwnerNoSee,
                      (int)Row.Mesh->bOnlyOwnerSee);
            }
            return;
        }
        if (Command == "hide1p" && (Argument == "on" || Argument == "off"))
        {
            auto Pawn = LocalPawn();
            if (!Pawn) { Reply("No pawn."); return; }
            Pawn->SetHideFirstPersonOverride(Argument == "on");
            Reply("hide1p: SetHideFirstPersonOverride(%s)", Argument.c_str());
            return;
        }
        if (Command == "props" && !Argument.empty())
        {
            for (int Index = 0; Index < UObject::GObjects->Num(); ++Index)
            {
                auto Object = UObject::GObjects->GetByIndex(Index);
                if (!Object || Object->IsDefaultObject()) continue;
                std::string Lower = Object->GetFullName();
                std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char c) { return (char)tolower(c); });
                if (Lower.find(Argument) == std::string::npos || Object->IsA<UClass>() || Object->IsA<UField>()) continue;
                Reply("%s object=%p vtable=%p", Object->GetFullName().c_str(), Object, Object->VTable);
                int Count = 0;
                for (auto Class = (UStruct*)Object->Class; Class; Class = Class->Super)
                for (auto Field = Class->Children; Field; Field = Field->Next)
                {
                    auto Property = Field->Cast<UProperty>();
                    if (!Property || ++Count > 160 || Property->Offset < 0 || Property->Offset + Property->ElementSize > Object->Class->Size) continue;
                    auto Data = (uint8*)Object + Property->Offset;
                    std::string Value;
                    if (auto Bool = Property->Cast<UBoolProperty>()) Value = (Data[Bool->ByteOffset] & Bool->FieldMask) ? "true" : "false";
                    else if (Property->IsA<UObjectProperty>()) Value = Name(*(UObject**)Data);
                    else if (Property->IsA<UFloatProperty>()) Value = std::to_string(*(float*)Data);
                    else if (Property->IsA<UIntProperty>()) Value = std::to_string(*(int32*)Data);
                    else if (Property->ElementSize == 1) Value = std::to_string(*Data);
                    Reply("  +%04X %s %s = %s", Property->Offset, Property->Class->GetName().c_str(), Property->GetName().c_str(), Value.c_str());
                }
                return;
            }
            Reply("No object matching '%s'", Argument.c_str());
            return;
        }
        if (Command == "functions" && !Argument.empty())
        {
            int Count = 0;
            for (auto Function : GetObjectsOfClass<UFunction>())
            {
                std::string FullName = Function->GetFullName(), Lower = FullName;
                std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char c) { return (char)tolower(c); });
                if (Lower.find(Argument) == std::string::npos) continue;
                if (++Count <= 100)
                    Reply("%s exec=client+0x%llX flags=%X object=%p", FullName.c_str(),
                          (unsigned long long)((uintptr_t)Function->ExecFunction - ImageBase), Function->FunctionFlags, Function);
            }
            Reply("%d matching function(s)", Count);
            return;
        }
        if (Command == "presentation")
        {
            auto Pawn = LocalPawn();
            if (!Pawn || !Pawn->Inventory) { Reply("No pawn."); return; }
            Reply("pawn=%p role=%d remote=%d client=%d server=%d", Pawn, (int)Pawn->Role, (int)Pawn->RemoteRole,
                  *(bool*)(ImageBase + 0x6C67BE9), *(bool*)(ImageBase + 0x6C67BEA));
            for (auto Item : Pawn->Inventory->GetEquippables())
            {
                Reply("%s item=%p owner-vfunc=client+0x%llX", Name(Item).c_str(), Item,
                      (unsigned long long)((uintptr_t)Item->VTable[0x660 / 8] - ImageBase));
                for (auto Component : Item->GetComponentsByClass(UFiringStateComponent::StaticClass()))
                {
                    auto Firing = Component->Cast<UFiringStateComponent>();
                    Reply("  %s pawn=%s projectile=%s", Name(Firing).c_str(), Name(Firing->OwningCharacter).c_str(), Name(Firing->ReusableHitScanProjectile).c_str());
                }
                for (auto Component : Item->GetComponentsByClass(UEffectManagerComponent::StaticClass()))
                {
                    auto Effects = Component->Cast<UEffectManagerComponent>();
                    Reply("  effects=%p defer=%d pending=%d server=%d start=%d replication=%d", Effects,
                          Effects->bDeferActiveEffects, Effects->PendingEffects.Num(), Effects->ServerActiveEffects.Num(),
                          Effects->PendingStartEffects.Num(), Effects->EffectsPendingReplication.Num());
                }
            }
            return;
        }
        if (Command == "help")
        {
            Reply("spawn sage enemy combat vandal | botorder hold/push/auto: tactical opponents");
            Reply("mapprobe | mapscan: export native navigation, spawn paths and a conservative walking graph to logs/maps");
            Reply("help | status | phase [round] | states | state <n> | waiting | force <input> | press <input> | look <pitch> [yaw] | actors <name> | spy on/off/reset | input <name> | slot <n> | desire <n> | give <sidearm> | equip <knife/sidearm> | ammo | abilities | trace on/off");
            Reply("give <weapon> [skin] [level] | skin <name>|path <objectpath> [level] | skins [weapon] | infinite [off] | speed [multiplier] | clearbodies");
            Reply("Weapons: classic frenzy ghost sheriff shorty bucky judge spectre bulldog phantom vandal marshal guardian operator ares odin");
            Reply("F8 / Escape closes this panel. Up / Down recalls commands. Commands run on the game thread.");
            return;
        }
        if (!Extra.empty() && Command != "look" && Command != "give" && Command != "equip" &&
            Command != "skin" && Command != "assets")
        {
            Reply("Too many arguments. Type help.");
            return;
        }
        if (Command == "phase")
        {
            auto World = UWorld::GetWorld();
            auto State = World && World->GameState ? World->GameState->Cast<AShooterGameState>() : nullptr;
            if (!State) { Reply("No AShooterGameState yet."); return; }
            if (Argument == "round")
                MatchFlow::EnterRoundPhase(State);
            if (Argument == "endless")
            {
                const bool bOn = Extra != "off";
                MatchFlow::SetEndlessRound(bOn);
                Reply("phase: endless round %s", bOn ? "on" : "off");
                return;
            }
            // EAllowedGameInputBlueprintBitMask
            const int32 Mask = State->PhaseAllowedGameInput;
            Reply("Phase=%d (InRound=3) allowed-input=0x%X shopping=%d round=%d gameplay-active=%d",
                  (int)State->Phase, Mask, (int)State->bIsShoppingEnabled, State->RoundNumber,
                  (int)State->IsGameplayActive());
            if (Argument == "where")
            {
                struct { UClass* Class; const char* Function; } Probe[] = {
                    {AShooterGameState::StaticClass(), "IsGameplayActive"},
                    {AShooterGameState::StaticClass(), "UpdateGameplayActive"},
                    {AShooterGameMode::StaticClass(), "AuthSetGameplayEnabled"},
                };
                for (auto& Entry : Probe)
                    if (auto Function = Entry.Class->FindFunction(Entry.Function))
                        Reply("  %s exec=client+0x%llX", Entry.Function,
                              (unsigned long long)((uintptr_t)Function->ExecFunction - ImageBase));
            }
            const char* Names[] = {"Movement", "Aiming", "ItemUsing", "Abilities", "WorldUsing"};
            for (int Bit = 0; Bit < 5; ++Bit)
                Reply("  %-10s %s", Names[Bit], (Mask & (1 << Bit)) ? "allowed" : "BLOCKED");
            return;
        }
        if (Command == "spy" && (Argument == "on" || Argument == "off" || Argument == "reset"))
        {
            {
                std::lock_guard<std::mutex> Lock(SpyMutex);
                if (Argument != "on")
                    SpySeen.clear();
            }
            SpyEnabled.store(Argument == "on");
            Reply("Function spy %s. Each distinct function is logged once; "
                  "run 'spy reset' between presses.", Argument.c_str());
            return;
        }
        if (Command == "look" && !Argument.empty())
        {
            auto World = UWorld::GetWorld();
            auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
            if (!PC) { Reply("No player controller."); return; }
            FRotator Rotation = PC->GetControlRotation();
            Rotation.Pitch = (float)atof(Argument.c_str());
            if (!Extra.empty())
                Rotation.Yaw = (float)atof(Extra.c_str());
            PC->SetControlRotation(Rotation);
            Reply("look: pitch %.1f yaw %.1f", Rotation.Pitch, Rotation.Yaw);
            return;
        }
        if (Command == "actors" && !Argument.empty())
        {
            int Count = 0;
            for (AActor* Actor : GetObjectsOfClass<AActor>())
            {
                if (!Actor || Actor->IsDefaultObject() || !Actor->Class)
                    continue;
                std::string ClassName = Actor->Class->GetName();
                std::string Lower = ClassName;
                std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                               [](unsigned char c) { return (char)tolower(c); });
                if (Lower.find(Argument) == std::string::npos)
                    continue;
                if (++Count > 20) continue;
                const FVector Location = Actor->K2_GetActorLocation();
                Reply("  %s at %.0f %.0f %.0f hidden=%d locally-hidden=%d owner=%s",
                      Actor->GetName().c_str(), Location.X, Location.Y, Location.Z,
                      (int)Actor->bHidden, (int)Actor->bLocallyHidden, Name(Actor->Owner).c_str());
            }
            Reply("%d actor(s) matching '%s'", Count, Argument.c_str());
            return;
        }
        if (Command == "face" && !Argument.empty())
        {
            auto World = UWorld::GetWorld();
            auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
            auto Pawn = PC ? PC->Pawn : nullptr;
            if (!Pawn) { Reply("No pawn."); return; }
            if (auto Actor = FindActor(Argument, Pawn))
            {
                const FVector Eye = PC->PlayerCameraManager ? PC->PlayerCameraManager->GetCameraLocation() : Pawn->K2_GetActorLocation();
                const FVector Target = Actor->K2_GetActorLocation();
                const double dx = Target.X - Eye.X, dy = Target.Y - Eye.Y, dz = Target.Z - Eye.Z;
                const double Flat = sqrt(dx * dx + dy * dy);
                FRotator Rotation = PC->GetControlRotation();
                Rotation.Yaw = (float)(atan2(dy, dx) * 180.0 / 3.14159265358979);
                Rotation.Pitch = (float)(atan2(dz, Flat) * 180.0 / 3.14159265358979);
                PC->SetControlRotation(Rotation);
                Reply("face: %s at %.0f %.0f %.0f, distance %.0f (pitch %.1f yaw %.1f)",
                      Actor->GetName().c_str(), Target.X, Target.Y, Target.Z,
                      sqrt(Flat * Flat + dz * dz), Rotation.Pitch, Rotation.Yaw);
                return;
            }
            Reply("face: no actor matching '%s'", Argument.c_str());
            return;
        }
        if (Command == "inspect" && !Argument.empty())
        {
            for (AActor* Actor : GetObjectsOfClass<AActor>())
            {
                if (!Actor || Actor->IsDefaultObject() || !Actor->Class)
                    continue;
                std::string Lower = Actor->Class->GetName();
                std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                               [](unsigned char c) { return (char)tolower(c); });
                if (Lower.find(Argument) == std::string::npos)
                    continue;

                Reply("%s hidden=%d locally-hidden=%d role=%d remote-role=%d owner=%s",
                      Actor->GetName().c_str(), (int)Actor->bHidden, (int)Actor->bLocallyHidden,
                      (int)Actor->Role, (int)Actor->RemoteRole, Name(Actor->Owner).c_str());
                for (UActorComponent* Component :
                         Actor->GetComponentsByClass(USceneComponent::StaticClass()))
                {
                    auto Scene = Component ? Component->Cast<USceneComponent>() : nullptr;
                    if (!Scene) continue;
                    std::string Asset;
                    if (auto StaticMesh = Scene->Cast<UStaticMeshComponent>())
                        Asset = " mesh=" + Name(StaticMesh->StaticMesh);
                    else if (auto SkeletalMesh = Scene->Cast<USkeletalMeshComponent>())
                        Asset = " mesh=" + Name(SkeletalMesh->SkeletalMesh);
                    std::string Owner;
                    if (auto Primitive = Scene->Cast<UPrimitiveComponent>())
                    {
                        char Buffer[96];
                        snprintf(Buffer, sizeof(Buffer),
                                 " owner-no-see=%d only-owner-see=%d main-pass=%d",
                                 (int)Primitive->bOwnerNoSee, (int)Primitive->bOnlyOwnerSee,
                                 (int)Primitive->bRenderInMainPass);
                        Owner = Buffer;
                    }
                    Reply("  %-42s visible=%d hidden-in-game=%d active=%d scale=%.2f%s%s",
                          Scene->Class->GetName().c_str(), (int)Scene->bVisible,
                          (int)Scene->bHiddenInGame, (int)Scene->bIsActive,
                          Scene->RelativeScale3D.X, Asset.c_str(), Owner.c_str());
                    if (Scene->AttachParent)
                        Reply("      on %s socket=%s rel=%.1f %.1f %.1f",
                              Scene->AttachParent->Class->GetName().c_str(),
                              Scene->AttachSocketName.ToString().c_str(), Scene->RelativeLocation.X,
                              Scene->RelativeLocation.Y, Scene->RelativeLocation.Z);
                }
                return;
            }
            Reply("inspect: no actor matching '%s'", Argument.c_str());
            return;
        }
        if (Command == "press" && !Argument.empty())
        {
            // straight into the inventory, skips the pc bindings
            struct { const char* Name; EAresEquippableInput Value; } Inputs[] = {
                {"primary",   EAresEquippableInput::EquippableInput_Primary_Trigger},
                {"secondary", EAresEquippableInput::EquippableInput_Secondary_Trigger},
                {"reload",    EAresEquippableInput::EquippableInput_Reload},
                {"zoom",      EAresEquippableInput::EquippableInput_ZoomToggle},
                {"use",       EAresEquippableInput::EquippableInput_Use},
                {"drop",      EAresEquippableInput::EquippableInput_Drop},
                {"activate",  EAresEquippableInput::EquippableInput_Activate},
            };
            auto Pawn = LocalPawn();
            if (!Pawn || !Pawn->Inventory) { Reply("No local pawn yet."); return; }
            for (auto& Entry : Inputs)
            {
                if (Argument != Entry.Name)
                    continue;
                const float Now = (float)UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
                Pawn->Inventory->OnInputStateChanged(Entry.Value, EAresInputState::Pressed, Now, Now, Now);
                Pawn->Inventory->OnInputStateChanged(Entry.Value, EAresInputState::Released, Now, Now, Now);
                Reply("press: %s down and up at %.2f", Entry.Name, Now);
                return;
            }
            Reply("press: primary, secondary, reload, zoom, use, drop, activate");
            return;
        }
        if (Command == "force" && !Argument.empty())
        {
            struct { const char* Name; EAresEquippableInput Value; } Inputs[] = {
                {"primary",   EAresEquippableInput::EquippableInput_Primary_Trigger},
                {"secondary", EAresEquippableInput::EquippableInput_Secondary_Trigger},
                {"use",       EAresEquippableInput::EquippableInput_Use},
                {"activate",  EAresEquippableInput::EquippableInput_Activate},
            };
            auto Pawn = LocalPawn();
            auto Item = Pawn && Pawn->Inventory ? Pawn->Inventory->CurrentEquippable : nullptr;
            if (!Item) { Reply("Nothing equipped."); return; }
            for (auto& Entry : Inputs)
            {
                if (Argument != Entry.Name)
                    continue;
                int Asked = 0;
                for (auto Machine : Item->EquippableStateMachines)
                {
                    if (!Machine) continue;
                    auto Current = Machine->AuthCurrentState.CurrentState;
                    auto Responder = Current ? Current->Cast<URespondToEventStateComponent>() : nullptr;
                    if (!Responder) continue;
                    FTriggerInput Trigger{};
                    Trigger.TriggerInput = Entry.Value;
                    Trigger.TriggerAction = EAresEquippableInputAction::EquippableInputAction_WentDown;
                    const bool Responded = Responder->BP_RespondToInput(Trigger, true);
                    Reply("force: %s in %s responded=%d", Name(Machine).c_str(),
                          Name(Current).c_str(), (int)Responded);
                    ReportQueue(Current);
                    ++Asked;
                }
                if (!Asked)
                    Reply("force: no state that responds to input is current");
                return;
            }
            Reply("force: primary, secondary, use, activate");
            return;
        }
        if (Command == "waiting")
        {
            auto Pawn = LocalPawn();
            auto Item = Pawn && Pawn->Inventory ? Pawn->Inventory->CurrentEquippable : nullptr;
            if (!Item) { Reply("Nothing equipped."); return; }

            const char* InputNames[] = {"Primary_Trigger", "Secondary_Trigger", "Reload", "ZoomToggle",
                                        "Use", "Drop", "Activate", "SwipeUp", "SwipeDown",
                                        "SwipeLeft", "SwipeRight"};
            const char* ActionNames[] = {"WentDown", "IsDown", "WentUp", "IsUp"};

            Reply("Equipped %s", Name(Item).c_str());
            for (auto Machine : Item->EquippableStateMachines)
            {
                if (!Machine) continue;
                auto Current = Machine->AuthCurrentState.CurrentState;
                Reply("  %s is in %s", Name(Machine).c_str(), Name(Current).c_str());
                auto Responder = Current ? Current->Cast<URespondToEventStateComponent>() : nullptr;
                if (!Responder)
                {
                    Reply("    (state does not respond to input at all)");
                    continue;
                }
                if (!Responder->StateChangeInputs.Num())
                    Reply("    no input responses registered");
                for (auto& Response : Responder->StateChangeInputs)
                {
                    const int In = (int)Response.Input.TriggerInput;
                    const int Act = (int)Response.Input.TriggerAction;
                    Reply("    %s / %s -> %s (delay %.2f, out-of-gameplay %d)",
                          In >= 0 && In < 11 ? InputNames[In] : "?",
                          Act >= 0 && Act < 4 ? ActionNames[Act] : "?",
                          Name(Response.State).c_str(), Response.Delay,
                          (int)Response.bCanRespondOutOfGameplay);
                }
                Reply("    responds to Primary_Trigger: %d",
                      (int)Responder->RespondsToInput(EAresEquippableInput::EquippableInput_Primary_Trigger));
                ReportQueue(Current);
            }
            return;
        }
        if (Command == "fn" && !Argument.empty())
        {
            struct { const char* Label; UClass* Class; } Classes[] = {
                {"RespondToEventStateComponent", URespondToEventStateComponent::StaticClass()},
                {"ScriptStateComponent", UScriptStateComponent::StaticClass()},
                {"StateComponent", UStateComponent::StaticClass()},
                {"StateMachineComponent", UStateMachineComponent::StaticClass()},
                {"AresInventory", UAresInventory::StaticClass()},
                {"AresEquippable", AAresEquippable::StaticClass()},
            };
            const char* Names[] = {
                "BP_RespondToInput", "RespondsToInput", "GoToState", "GoToNextState",
                "ClearInputResponse", "OnActivationInputStateChanged", "OnInputStateChanged",
                "InputIsDisabled", "EquippableSlotIsDisabled",
            };
            int Found = 0;
            for (auto& Entry : Classes)
            {
                if (!Entry.Class) continue;
                for (const char* FunctionName : Names)
                {
                    std::string Lower = FunctionName;
                    std::transform(Lower.begin(), Lower.end(), Lower.begin(),
                                   [](unsigned char c) { return (char)tolower(c); });
                    if (Argument != "all" && Lower.find(Argument) == std::string::npos)
                        continue;
                    UFunction* Function = Entry.Class->FindFunction(FunctionName);
                    if (!Function) continue;
                    Reply("  %s::%s exec=client+0x%llX", Entry.Label, FunctionName,
                          (unsigned long long)((uintptr_t)Function->ExecFunction - ImageBase));
                    ++Found;
                }
            }
            if (!Found)
                Reply("fn: nothing matching '%s' (try: all)", Argument.c_str());
            return;
        }
        if (Command == "starts")
        {
            int Count = 0;
            for (APlayerStart* Start : GetObjectsOfClass<APlayerStart>())
            {
                if (!Start || Start->IsDefaultObject())
                    continue;
                if (++Count > 40) continue;
                const FVector Location = Start->K2_GetActorLocation();
                Reply("  %-28s at %.0f %.0f %.0f", Start->GetName().c_str(),
                      Location.X, Location.Y, Location.Z);
            }
            Reply("%d player start(s)", Count);
            return;
        }
        if (Command == "agents")
        {
            int Count = 0;
            for (UCharacterDataAsset* Asset : GetObjectsOfClass<UCharacterDataAsset>())
            {
                if (!Asset)
                    continue;
                Reply("  %s developer=%s shipping=%s playable=%d%s", Name(Asset).c_str(),
                      Asset->DeveloperName.ToString().c_str(), Asset->ShippingName.ToString().c_str(),
                      (int)Asset->bIsPlayableCharacter, Asset->IsDefaultObject() ? "  (CDO)" : "");
                ++Count;
            }
            Reply("%d character data asset(s) loaded.", Count);
            return;
        }
        if (Command == "states" || Command == "state")
        {
            auto World = UWorld::GetWorld();
            auto Mode = World && World->AuthorityGameMode ? World->AuthorityGameMode->Cast<AShooterGameMode>() : nullptr;
            auto Machine = Mode ? Mode->StateMachine : nullptr;
            if (!Machine) { Reply("No game mode state machine yet."); return; }

            auto Current = Machine->AuthCurrentState.CurrentState;
            Reply("current state: %s", Name(Current).c_str());

            int Index = 0;
            const int Wanted = Command == "state" ? atoi(Argument.c_str()) : -1;
            for (auto& Pair : Machine->States)
            {
                UStateComponent* State = Pair.Key();
                if (!State) { ++Index; continue; }
                auto GameState = State->Cast<UBaseGameStateComponent>();
                if (Command == "states")
                {
                    Reply("  [%d] %s%s%s", Index, State->GetName().c_str(),
                          State == Current ? "  <- current" : "",
                          GameState ? "" : "  (not a game state)");
                }
                else if (Index == Wanted && GameState)
                {
                    // timed states ignore plain GoToState
                    if (auto Timed = Current ? Current->Cast<UTimeGameStateComponent>() : nullptr)
                        Timed->GoToStateAndSkipTimedEvents(GameState, 0.05f);
                    else if (auto CurrentGameState = Current ? Current->Cast<UBaseGameStateComponent>() : nullptr)
                        CurrentGameState->GoToState(GameState, 0.05f);
                    else { Reply("Current state cannot drive a transition."); return; }
                    Reply("requested %s -> %s", Name(Current).c_str(), State->GetName().c_str());
                    return;
                }
                ++Index;
            }
            if (Command == "state")
                Reply("No game state at index %d. Run states.", Wanted);
            return;
        }
        if (Command == "input")
        {
            struct { const char* Name; EAresAutomationInput Value; } Inputs[] = {
                {"fire",      EAresAutomationInput::PrimaryTrigger},
                {"altfire",   EAresAutomationInput::SecondaryTrigger},
                {"reload",    EAresAutomationInput::Reload},
                {"zoom",      EAresAutomationInput::ZoomToggle},
                {"primary",   EAresAutomationInput::EquipPrimary},
                {"secondary", EAresAutomationInput::EquipSecondary},
                {"melee",     EAresAutomationInput::EquipMelee},
                {"grenade",   EAresAutomationInput::EquipGrenade},
                {"q",         EAresAutomationInput::EquipQ},
                {"e",         EAresAutomationInput::EquipE},
                {"ult",       EAresAutomationInput::EquipUltimate},
            };
            auto World = UWorld::GetWorld();
            auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
            auto Shooter = PC ? PC->Cast<AShooterPlayerController>() : nullptr;
            if (!Shooter) { Reply("No AShooterPlayerController yet."); return; }
            for (auto& Entry : Inputs)
            {
                if (Argument != Entry.Name)
                    continue;
                Shooter->AutomationSimulateInput(Entry.Value);
                Reply("input: simulated %s", Entry.Name);
                return;
            }
            Reply("input: fire, altfire, reload, zoom, primary, secondary, melee, grenade, q, e, ult");
            return;
        }
        if (Command == "trace" && (Argument == "on" || Argument == "off"))
        {
            TraceEnabled = Argument == "on";
            LastStates.clear();
            Reply("Gameplay state tracing %s.", TraceEnabled ? "on" : "off");
            return;
        }
        auto Pawn = LocalPawn();
        if (!Pawn || !Pawn->Inventory) { Reply("No local pawn yet. Wait for the map to finish loading."); return; }
        auto Inventory = Pawn->Inventory;
        if (Command == "status" && Argument.empty()) { Status(Pawn); return; }
        if ((Command == "slot" || Command == "desire") && !Argument.empty())
        {
            const int SlotIndex = atoi(Argument.c_str());
            const int SlotCount = (int)(sizeof(Inventory->ItemSlots) / sizeof(Inventory->ItemSlots[0]));
            if (SlotIndex < 0 || SlotIndex >= SlotCount) { Reply("Slot must be 0..%d.", SlotCount - 1); return; }
            auto Slot = Inventory->ItemSlots[SlotIndex];
            auto Item = Slot && Slot->Contents ? Slot->Contents->Cast<AAresEquippable>() : nullptr;
            if (!Item) { Reply("Slot %d is empty.", SlotIndex); return; }
            if (Command == "slot")
            {
                const float Now = (float)Ares::Seconds();
                Inventory->OnActivationInputStateChanged((EAresItemSlot)SlotIndex, EAresInputState::Pressed,
                                                         EAresActivationInputType::StandardInput, Now);
                Inventory->OnActivationInputStateChanged((EAresItemSlot)SlotIndex, EAresInputState::Released,
                                                         EAresActivationInputType::StandardInput, Now);
                Reply("slot %d: pressed and released %s", SlotIndex, Name(Item).c_str());
            }
            else
            {
                const bool Accepted = Inventory->SetDesiredEquippable(Item, EEquipSpeed::Normal, EEquipSource::ActivationInput);
                Reply("slot %d: equip request for %s %s", SlotIndex, Name(Item).c_str(),
                      Accepted ? "accepted" : "REJECTED");
            }
            return;
        }
        if (Command == "repair")
        {
            for (auto Weapon : Inventory->GetEquippables())
            {
                if (!Weapon) continue;
                if (Argument == "inventory")
                {
                    Weapon->InInventory = EEquippableInInventory::Yes;
                    Weapon->MyPawn = Pawn;
                    Weapon->AttributeOwner = Pawn;
                }
                else if (Argument == "owner") Weapon->AuthSetOwnerTo(Pawn);
                else if (Argument == "notify") Weapon->OnRep_OwnerWithPrevOwner(nullptr);
                else if (Argument == "scope" && Weapon == Inventory->CurrentEquippable)
                {
                    Weapon->bHideFirstPersonWhileZoomed = true;
                    Weapon->ZoomInHideOption = EZoomInHideOption::WhenFullyZoomedIn;
                    Weapon->ZoomOutShowOption = EZoomOutShowOption::WhenFullyZoomedOut;
                }
                else if (Argument == "client" || Argument == "auth")
                {
                    // bp overrides, sdk wrapper caches the wrong function
                    auto Function = Weapon->Class->FindFunction(Argument == "client" ? "ReceiveClientSetOwningCharacter" : "ReceiveAuthSetOwningCharacter");
                    struct { AShooterCharacter* NewCharacter; } Params{Pawn};
                    if (Function) Weapon->ProcessEvent(Function, &Params);
                }
            }
            Status(Pawn);
            return;
        }
        if ((Command == "give" || Command == "equip") && !Argument.empty())
        {
            AAresEquippable* Weapon = nullptr;
            std::string SkinWanted;
            int SkinLevel = 0;
            if (Argument == "knife")
                Weapon = Inventory->GetFirstEquippableFromSlot(EAresItemSlot::Melee);
            else
            {
                const WeaponEntry* Found = WeaponEntryFor(Argument);
                if (!Found) { Reply("Unsupported weapon. Type help for supported sidearms."); return; }
                auto Class = FindObject<UClass>(Found->Path);
                if (!Class) { Reply("Weapon asset is unavailable: %s", Argument.c_str()); return; }

                // give vandal dragon 2
                if (!Extra.empty())
                {
                    std::istringstream Rest(Extra);
                    std::string Token, Wanted;
                    int Level = 0;
                    while (Rest >> Token)
                    {
                        if (Token.find_first_not_of("0123456789") == std::string::npos)
                            Level = atoi(Token.c_str());
                        else
                            Wanted = Token;
                    }
                    SkinWanted = Wanted;
                    SkinLevel = Level;
                }

                Weapon = Inventory->FindEquippableByClass(Class);
                if (!Weapon && Command == "give")
                    Weapon = Inventory->AuthCreateAndAddEquippable(Class);
                if (Weapon && !SkinWanted.empty())
                {
                    std::string Report;
                    std::string Why;
                    if (auto Custom = CustomSkins::Find(SkinWanted, Found->Name, Why))
                        CustomSkins::Apply(Weapon, *Custom, Report);
                    else if (!Why.empty())
                        Report = Why;
                    else
                        LocalSkins::Apply(Weapon, LocalSkins::FolderOf(Found->Path), SkinWanted, SkinLevel, Report);
                    Reply("  skin: %s", Report.c_str());
                }
            }
            if (!Weapon) { Reply("Weapon is not in the inventory. Use give <sidearm> first."); return; }
            bool Accepted = Inventory->SetDesiredEquippable(Weapon, EEquipSpeed::Instant, EEquipSource::GrantEquippable);
            Reply("%s: %s; equip request %s", Command.c_str(), Name(Weapon).c_str(), Accepted ? "accepted" : "rejected");
            return;
        }
        if (Command == "ammo" && Argument.empty())
        {
            int Count = 0;
            for (auto Weapon : Inventory->GetEquippables())
            {
                if (!Weapon) continue;
                for (auto Component : Weapon->GetComponentsByClass(UAmmoComponent::StaticClass()))
                {
                    auto Ammo = Component->Cast<UAmmoComponent>();
                    if (!Ammo || Ammo->MaxAmmo <= 0) continue;
                    Ammo->Ammo = Ammo->MaxAmmo;
                    Count++;
                }
            }
            Reply("Refilled %d magazine(s).", Count);
            return;
        }
        if (Command == "abilities" && Argument.empty())
        {
            int Count = AresInventory::RefillAbilities(Pawn);
            Reply("Refilled %d ability resource(s). Use status to inspect charges.", Count);
            return;
        }
        Reply("Unknown command or arguments. Type help.");
    }

    void Hide()
    {
        ShowWindow(Panel, SW_HIDE);
        if (IsWindow(GameWindow)) SetForegroundWindow(GameWindow);
    }

    void Submit()
    {
        char Text[512]{};
        GetWindowTextA(Entry, Text, sizeof(Text));
        std::string Line(Text);
        if (Line.find_first_not_of(" \t\r\n") == std::string::npos) return;
        {
            std::lock_guard<std::mutex> Lock(QueueMutex);
            if (Commands.size() >= 32) return;
            Commands.push_back(Line);
            Output.emplace_back("> " + Line);
            HasCommands.store(true);
        }
        if (History.empty() || History.back() != Line) History.push_back(Line);
        if (History.size() > 100) History.erase(History.begin());
        HistoryIndex = History.size();
        SetWindowTextA(Entry, "");
        SetFocus(Entry);
    }

    LRESULT CALLBACK InputProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        if (Message == WM_KEYDOWN)
        {
            if (WParam == VK_RETURN) { PostMessageW(Panel, kSubmit, 0, 0); return 0; }
            if (WParam == VK_ESCAPE) { Hide(); return 0; }
            if (WParam == VK_UP || WParam == VK_DOWN)
            {
                if (WParam == VK_UP && HistoryIndex > 0) --HistoryIndex;
                if (WParam == VK_DOWN && HistoryIndex < History.size()) ++HistoryIndex;
                SetWindowTextA(Entry, HistoryIndex < History.size() ? History[HistoryIndex].c_str() : "");
                SendMessageW(Entry, EM_SETSEL, -1, -1);
                return 0;
            }
        }
        if (Message == WM_CHAR && (WParam == VK_RETURN || WParam == VK_ESCAPE)) return 0;
        return CallWindowProcW(EditProc, Window, Message, WParam, LParam);
    }

    LRESULT CALLBACK PanelProc(HWND Window, UINT Message, WPARAM WParam, LPARAM LParam)
    {
        if (Message == WM_CLOSE) { Hide(); return 0; }
        if (Message == kSubmit || (Message == WM_COMMAND && LOWORD(WParam) == 1)) { Submit(); return 0; }
        if (Message == WM_SETFOCUS && Entry) SetFocus(Entry);
        if (Message == WM_SIZE && Transcript && Entry)
        {
            int W = LOWORD(LParam), H = HIWORD(LParam);
            MoveWindow(Transcript, 12, 12, W - 24, H - 64, TRUE);
            MoveWindow(Entry, 12, H - 40, W - 108, 28, TRUE);
            MoveWindow(GetDlgItem(Window, 1), W - 84, H - 40, 72, 28, TRUE);
        }
        return DefWindowProcW(Window, Message, WParam, LParam);
    }

    BOOL CALLBACK FindGame(HWND Window, LPARAM)
    {
        DWORD Pid = 0;
        GetWindowThreadProcessId(Window, &Pid);
        wchar_t Class[64]{};
        GetClassNameW(Window, Class, 64);
        if (Pid == GetCurrentProcessId() && wcscmp(Class, L"UnrealWindow") == 0)
        { GameWindow = Window; return FALSE; }
        return TRUE;
    }

    void WindowThread()
    {
        while (!GameWindow) { EnumWindows(FindGame, 0); Sleep(200); }
        WNDCLASSW Class{};
        Class.lpfnWndProc = PanelProc;
        Class.hInstance = GetModuleHandleW(nullptr);
        Class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
        Class.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
        Class.lpszClassName = L"AresCommandConsole";
        if (!RegisterClassW(&Class)) { Ares::Log("console: failed to register window class"); return; }
        Panel = CreateWindowExW(WS_EX_TOOLWINDOW, Class.lpszClassName, L"VALORANT 1.08 - Developer console (F8)",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME,
                                CW_USEDEFAULT, CW_USEDEFAULT, 850, 430, GameWindow, nullptr, Class.hInstance, nullptr);
        if (!Panel) { Ares::Log("console: failed to create window"); return; }
        Transcript = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL |
                                     ES_MULTILINE | ES_AUTOVSCROLL | ES_READONLY, 12, 12, 800, 320, Panel, nullptr, Class.hInstance, nullptr);
        Entry = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                               12, 344, 710, 28, Panel, nullptr, Class.hInstance, nullptr);
        auto Button = CreateWindowW(L"BUTTON", L"Run", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                    740, 344, 72, 28, Panel, (HMENU)1, Class.hInstance, nullptr);
        if (!Transcript || !Entry || !Button) { DestroyWindow(Panel); return; }
        auto Font = GetStockObject(DEFAULT_GUI_FONT);
        for (auto Control : {Transcript, Entry, Button}) SendMessageW(Control, WM_SETFONT, (WPARAM)Font, TRUE);
        SendMessageW(Entry, EM_SETLIMITTEXT, 500, 0);
        EditProc = (WNDPROC)SetWindowLongPtrW(Entry, GWLP_WNDPROC, (LONG_PTR)InputProc);
        RECT Client; GetClientRect(Panel, &Client);
        SendMessageW(Panel, WM_SIZE, 0, MAKELPARAM(Client.right, Client.bottom));
        Reply("Local developer console. Type help. Press F8 in the game to open/close.");
        bool WasDown = false;
        for (;;)
        {
            MSG Message;
            while (PeekMessageW(&Message, nullptr, 0, 0, PM_REMOVE))
            { TranslateMessage(&Message); DispatchMessageW(&Message); }
            bool Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            HWND Foreground = GetForegroundWindow();
            if (Down && !WasDown && (Foreground == GameWindow || Foreground == Panel))
            {
                if (IsWindowVisible(Panel)) Hide();
                else
                {
                    RECT Rect; GetWindowRect(GameWindow, &Rect);
                    SetWindowPos(Panel, HWND_TOP, Rect.left + 40, Rect.top + 70, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
                    SetForegroundWindow(Panel);
                    SetFocus(Entry);
                }
            }
            WasDown = Down;
            std::vector<std::string> Lines;
            { std::lock_guard<std::mutex> Lock(QueueMutex); Lines.swap(Output); }
            for (auto& Line : Lines)
            {
                if (GetWindowTextLengthW(Transcript) > 50000) SetWindowTextW(Transcript, L"");
                Line += "\r\n";
                SendMessageW(Transcript, EM_SETSEL, -1, -1);
                SendMessageA(Transcript, EM_REPLACESEL, FALSE, (LPARAM)Line.c_str());
            }
            Sleep(15);
        }
    }
}

void DeveloperConsole::Run(const std::string& Line)
{
    Execute(Line);
}

void DeveloperConsole::Start()
{
    if (Ares::Config().Mode == Ares::EMode::Standalone)
        std::thread(WindowThread).detach();
}

void DeveloperConsole::Tick()
{
    // bot update re-enters ProcessEvent, spawning here breaks its loop
    if(LocalBots::IsUpdating())return;
    static bool ReadingRequest=false;
    if(!ReadingRequest && Ares::Config().Mode==Ares::EMode::Standalone) {
        FReentrancyGuard RequestGuard(ReadingRequest);
        static double NextRequest=0;
        const double RequestNow=Ares::Seconds();
        if(RequestNow>=NextRequest && LocalPawn()) {
            NextRequest=RequestNow+0.5;
            const auto Dir=std::filesystem::path(Ares::Config().LogPath).parent_path()/"bots";
            std::ifstream Request(Dir/"request.txt");std::string Line;
            if(std::getline(Request,Line) && !Line.empty()) {
                Request.close();std::ofstream(Dir/"request.txt",std::ios::trunc).close();
                static const char* Allowed[]={"spawn ","botorder ","botskill ","botawareness ","botinfo","botaudio","rounds","brain","tactical","tacpath ","animdump","fnat ","fnaddr ","memdump ","freecam","clearbodies","perf"};
                bool Ok=false; for(auto A:Allowed) if(Line.rfind(A,0)==0) Ok=true;
                if(Ok) Execute(Line);
                else Reply("bots request rejected: only spawn, botorder, botskill, botawareness, botinfo, rounds, brain and tactical are supported");
            }
        }
    }

    if (SpeedEnabled)
    {
        static double LastSpeed = 0.0;
        const double Now = Ares::Seconds();
        if (Now - LastSpeed >= 0.25)
        {
            LastSpeed = Now;
            if (auto Pawn = LocalPawn())
            {
                const FVector Velocity = Pawn->GetVelocity();
                Ares::Log("speed: %.0f", sqrtf(Velocity.X * Velocity.X + Velocity.Y * Velocity.Y));
            }
        }
    }
    if (!HasCommands.load() && !TraceEnabled) return;
    static bool Busy = false;
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered()) return;
    std::vector<std::string> Pending;
    {
        std::lock_guard<std::mutex> Lock(QueueMutex);
        Pending.swap(Commands);
        HasCommands.store(false);
    }
    for (auto& Command : Pending) Execute(Command);
    if (TraceEnabled)
    {
        static double LastTrace = 0.0;
        double Now = Ares::Seconds();
        if (Now - LastTrace < 0.025) return;
        LastTrace = Now;
        auto Pawn = LocalPawn();
        if (Pawn != TracedPawn) { TracedPawn = Pawn; LastStates.clear(); }
        if (!Pawn || !Pawn->Inventory) return;
        for (auto Weapon : Pawn->Inventory->GetEquippables())
        {
            if (!Weapon) continue;
            for (auto Machine : Weapon->EquippableStateMachines)
            {
                if (!Machine) continue;
                auto State = Machine->AuthCurrentState.CurrentState;
                if (LastStates[Machine] == State) continue;
                LastStates[Machine] = State;
                Reply("trace %s / %s -> %s", Name(Weapon).c_str(), Name(Machine).c_str(), Name(State).c_str());
            }
        }
    }
}

bool DeveloperConsole::SpyArmed()
{
    return SpyEnabled.load(std::memory_order_relaxed);
}

void DeveloperConsole::SpyRecord(UObject* Object, UFunction* Function)
{
    if (!Function)
        return;
    {
        std::lock_guard<std::mutex> Lock(SpyMutex);
        if (!SpySeen.insert(Function).second)
            return;
    }
    const std::string FullName = Function->GetFullName();
    if (IsSpyNoise(FullName))
        return;
    Ares::Log("spy: %s   on %s", FullName.c_str(), Name(Object).c_str());
}

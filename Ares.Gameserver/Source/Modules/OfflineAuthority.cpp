#include <pch.h>
#include <Headers/Modules/OfflineAuthority.h>
#include <unordered_set>
#include <cstring>

void OfflineAuthority::Tick()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    static bool Busy = false;
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered()) return;
    static double LastCheck = -1;
    const double Now = Ares::Seconds();
    if (Now - LastCheck < 0.05) return;
    LastCheck = Now;
    auto World = UWorld::GetWorld();
    auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    auto Pawn = PC && PC->Pawn ? PC->Pawn->Cast<AShooterCharacter>() : nullptr;
    auto Item = Pawn && Pawn->Inventory ? Pawn->Inventory->CurrentEquippable : nullptr;
    if (!Item || !Item->EquippableIsLocallyControlled()) return;
    const auto ClassName = Item->Class->GetName();
    if (ClassName != "Ability_Thorne_E_Wall_Fortifying_C" &&
        ClassName != "Ability_Thorne_E_Wall_Production_C") return;
    for (auto Machine : Item->EquippableStateMachines)
    {
        auto State = Machine ? Machine->AuthCurrentState.CurrentState : nullptr;
        auto Wait = State ? State->Cast<UScriptStateComponent>() : nullptr;
        if (!Wait || Wait->GetName() != "WaitForConfirmedRotation" ||
            !Wait->CurrentContext || Wait->QueuedStateTransition.bIsSet) continue;
        // server arm waits on a client rotation rpc that never comes
        if (auto Confirm = Item->Class->FindFunction("AuthConfirmRotation"))
        {
            std::vector<uint8> Params(Confirm->Size > 0 ? Confirm->Size : 1, 0);
            Item->ProcessEvent(Confirm, Params.data());
            Ares::Log("authority: confirmed local Sage wall rotation");
        }
    }
}

namespace
{
    bool AppliesDamage(const TArray<uint8>& Script)
    {
        static UFunction* Apply = UGameplayStatics::StaticClass()->FindFunction("ApplyDamage");
        static UFunction* Axial = UShooterBlueprintLibrary::StaticClass()->FindFunction("FindActorsInAxialArea");
        for (int I = 0; I <= Script.Num() - 8; ++I)
        {
            UFunction* Callee = nullptr;
            memcpy(&Callee, &Script[I], sizeof(Callee));
            if ((Apply && Callee == Apply) || (Axial && Callee == Axial))
                return true;
        }
        return false;
    }

    // sova recon, server arm turns collision on, client arm turns it off
    bool EnablesCollisionOffline(const TArray<uint8>& Script, int ArmStart, int ArmEnd)
    {
        static const FName Collision(L"SetCollisionEnabled");
        if (ArmEnd <= ArmStart || ArmEnd > Script.Num() || ArmEnd - ArmStart > 400)
            return false;
        for (int At = ArmStart; At <= ArmEnd - 13; ++At)
        {
            if (Script[At] != 0x1B) continue;      // EX_VirtualFunction
            int32 Comparison = 0;                  // FScriptName leads with it
            memcpy(&Comparison, &Script[At + 1], sizeof(Comparison));
            if (Comparison == Collision.ComparisonIndex)
                return true;
        }
        return false;
    }

    bool SplicesExplosionDamage(UFunction* Function, TArray<uint8>& Script, UFunction* Dedicated)
    {
        if (!AppliesDamage(Script))
            return false;

        int Spliced = 0;
        for (int I = 0; I <= Script.Num() - 35; ++I)
        {
            uint8* Code = &Script[I];
            // IsDedicatedServer -> JumpIfNot else
            if (Code[0] != 0x14 || Code[1] != 0x00 || Code[10] != 0x68 || Code[19] != 0x17 ||
                Code[20] != 0x16 || Code[21] != 0x07 || Code[26] != 0x00) continue;
            UFunction* Callee = nullptr;
            memcpy(&Callee, Code + 11, sizeof(Callee));
            if (Callee != Dedicated || memcmp(Code + 2, Code + 27, 8)) continue;

            uint32 Else = 0;
            memcpy(&Else, Code + 22, sizeof(Else));
            const int ArmStart = I + 35, ArmEnd = (int)Else;
            if (ArmEnd <= ArmStart || ArmEnd > Script.Num() || ArmEnd - ArmStart > 200)
                continue;

            for (int At = ArmStart; At <= ArmEnd - 5; ++At)
            {
                if (Script[At] != 0x5B) continue;
                uint32 Resume = 0;
                memcpy(&Resume, &Script[At + 1], sizeof(Resume));
                if (Resume == 0 || (int)Resume >= Script.Num()) continue;
                if (Script[Resume - 1] != 0x4D) continue;
                Script[Resume - 1] = 0x0B; // EX_Nothing, fall through
                ++Spliced;
                break;
            }
        }
        if (Spliced)
            Ares::Log("authority: %s runs its explosion damage locally (%d splice(s))",
                      Function->GetName().c_str(), Spliced);
        return Spliced > 0;
    }
} // namespace

void OfflineAuthority::Prepare(UObject* Object)
{
    if (!Object || Ares::Config().Mode != Ares::EMode::Standalone) return;
    static bool Busy = false;
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered()) return;
    static std::unordered_set<UStruct*> Checked;
    static UFunction* Dedicated = UKismetSystemLibrary::StaticClass()->FindFunction("IsDedicatedServer");
    static UFunction* Server = UKismetSystemLibrary::StaticClass()->FindFunction("IsServer");
    if (!Dedicated || !Server) return;

    for (auto Class = (UStruct*)Object->Class; Class; Class = Class->Super)
    {
        if (!Checked.insert(Class).second) break;
        int Repaired = 0;
        for (auto Field = Class->Children; Field; Field = Field->Next)
        {
            auto Function = Field->Cast<UFunction>();
            if (!Function) continue;
            auto& Script = *(TArray<uint8>*)((uint8*)Function + 0x60); // UStruct::Script
            if (Script.Num() < 31 || Script.Num() > 1000000) continue;
            // SpraysAndEmotes toggle is off without services
            if (Class->GetName() == "CosmeticActionsComponent_C" && Function->GetName() == "CheckFidget")
            {
                static UFunction* Feature = UShooterBlueprintLibrary::StaticClass()->FindFunction("IsGameFeatureToggleEnabled");
                for (int I = 0; Feature && I <= Script.Num() - 13; ++I)
                {
                    UFunction* Callee = nullptr;
                    if (Script[I] != 0x68 || Script[I + 9] != 0x17 || Script[I + 10] != 0x24 ||
                        Script[I + 11] != (uint8)EGameFeatureToggleName::SpraysAndEmotes || Script[I + 12] != 0x16) continue;
                    memcpy(&Callee, &Script[I + 1], sizeof(Callee));
                    if (Callee != Feature) continue;
                    Script[I] = 0x27; // EX_True
                    memset(&Script[I + 1], 0x0B, 12); // EX_Nothing, keeps offsets
                    Ares::Log("presentation: enabled local Y inspect eligibility");
                }
                continue;
            }
            // skin meshes are behind !IsServer || IsStandalone, listen server skips them
            if (Function->GetName() == "ExecuteUbergraph_Weapon_Skin_Base" ||
                Function->GetName() == "ExecuteUbergraph_Weapon_Chroma_Base")
            {
                static UFunction* Standalone = UKismetSystemLibrary::StaticClass()->FindFunction("IsStandalone");
                int Count = 0;
                for (int I = 0; Standalone && I <= Script.Num() - 11; ++I)
                {
                    UFunction* Callee = nullptr;
                    if (Script[I] != 0x68 || Script[I + 9] != 0x17 || Script[I + 10] != 0x16) continue;
                    memcpy(&Callee, &Script[I + 1], sizeof(Callee));
                    if (Callee != Server) continue;
                    // finisher check has no IsStandalone next to it, leave that one
                    bool Paired = false;
                    for (int At = I + 11; At <= I + 60 && At <= Script.Num() - 11; ++At)
                    {
                        if (Script[At] != 0x68 || Script[At + 9] != 0x17 || Script[At + 10] != 0x16) continue;
                        UFunction* Other = nullptr;
                        memcpy(&Other, &Script[At + 1], sizeof(Other));
                        if (Other == Standalone) { Paired = true; break; }
                    }
                    if (!Paired) continue;
                    memcpy(&Script[I + 1], &Dedicated, sizeof(Dedicated));
                    ++Count;
                }
                if (Count)
                {
                    auto& References = *(TArray<UObject*>*)((uint8*)Function + 0x90);
                    References.Add(Dedicated);
                    Ares::Log("presentation: %s enabled local skin meshes/materials (%d mode checks)",
                              Function->GetName().c_str(), Count);
                }
                continue;
            }
            // same deal for reflex sights
            if (Function->GetName() == "ExecuteUbergraph_Attachment_Base_Reflex")
            {
                int Count = 0;
                for (int I = 0; I <= Script.Num() - 11; ++I)
                {
                    UFunction* Callee = nullptr;
                    if (Script[I] != 0x68 || Script[I + 9] != 0x17 || Script[I + 10] != 0x16) continue;
                    memcpy(&Callee, &Script[I + 1], sizeof(Callee));
                    if (Callee != Server) continue;
                    memcpy(&Script[I + 1], &Dedicated, sizeof(Dedicated));
                    ++Count;
                }
                if (Count)
                {
                    auto& References = *(TArray<UObject*>*)((uint8*)Function + 0x90);
                    References.Add(Dedicated);
                    Ares::Log("presentation: enabled local reflex sight setup (%d mode checks)", Count);
                }
                continue;
            }
            // boom bot just sits there otherwise, deploy is dedicated only
            if (Function->GetName() == "ExecuteUbergraph_Pawn_Clay_E_Boomba")
            {
                int Count = 0;
                for (int I = 0; I <= Script.Num() - 35; ++I)
                {
                    uint8* Code = &Script[I];
                    if (Code[0] != 0x14 || Code[1] != 0x00 || Code[10] != 0x68 || Code[19] != 0x17 ||
                        Code[20] != 0x16 || Code[21] != 0x07 || Code[26] != 0x00) continue;
                    UFunction* Callee = nullptr;
                    memcpy(&Callee, Code + 11, sizeof(Callee));
                    if (Callee != Dedicated || memcmp(Code + 2, Code + 27, 8)) continue;
                    memcpy(Code + 11, &Server, sizeof(Server));
                    ++Count;
                }
                if (Count)
                {
                    auto& References = *(TArray<UObject*>*)((uint8*)Function + 0x90);
                    References.Add(Server);
                    Ares::Log("authority: enabled the local Boom Bot deploy (%d guard(s))", Count);
                }
                continue;
            }
            // explosion damage sits behind a dedicated only delay, client arm stops 1 byte short
            if (SplicesExplosionDamage(Function, Script, Dedicated))
                continue;

            bool Changed = false;
            for (int I = 0; I <= Script.Num() - 31; ++I)
            {
                uint8* Code = &Script[I];
                // IsDedicatedServer -> PopExecutionFlowIfNot
                if (Code[0] != 0x14 || Code[1] != 0x00 || Code[10] != 0x68 ||
                    Code[19] != 0x17 || Code[20] != 0x16) continue;
                UFunction* Callee = nullptr;
                memcpy(&Callee, Code + 11, sizeof(Callee));
                if (Callee != Dedicated) continue;
                bool PositiveReturn = Code[21] == 0x4F && Code[22] == 0x00 &&
                    !memcmp(Code + 2, Code + 23, 8);
                // jump to final return only, else blocks usually hold the visuals
                if (!PositiveReturn && I <= Script.Num() - 35 &&
                    Code[21] == 0x07 && Code[26] == 0x00 && !memcmp(Code + 2, Code + 27, 8))
                {
                    uint32 Target = 0;
                    memcpy(&Target, Code + 22, sizeof(Target));
                    PositiveReturn = Target == (uint32)Script.Num() - 3 &&
                        Script[Target] == 0x04 && Script[Target + 1] == 0x0B && Script[Target + 2] == 0x53;
                }
                if (!PositiveReturn && Code[21] == 0x07 && Code[26] == 0x00 &&
                    I <= Script.Num() - 35 && !memcmp(Code + 2, Code + 27, 8))
                {
                    uint32 Else = 0;
                    memcpy(&Else, Code + 22, sizeof(Else));
                    PositiveReturn = EnablesCollisionOffline(Script, I + 35, (int)Else);
                }
                if (!PositiveReturn) continue;
                memcpy(Code + 11, &Server, sizeof(Server));
                ++Repaired;
                Changed = true;
            }
            if (Changed)
            {
                auto& References = *(TArray<UObject*>*)((uint8*)Function + 0x90);
                bool HasServer = false;
                for (auto Reference : References) if (Reference == Server) HasServer = true;
                if (!HasServer) References.Add(Server);
            }
        }
        if (Repaired) Ares::Log("authority: %s admits the local server at %d positive dedicated-server guard(s)", Class->GetName().c_str(), Repaired);
    }
}

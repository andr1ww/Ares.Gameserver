#include <pch.h>
#include <Headers/Modules/ShooterGame/ShooterGameMode.h>
INIT_MODULE(ShooterGameMode);

bool ShooterGameMode::ReadyToStartMatch(AShooterGameMode* _this)
{
    AShooterGameState* GameState = _this->GameState->Cast<AShooterGameState>();

    static bool bInit = false;
    if (!bInit)
    {
        GameState->MatchInfo.MatchID = L"88a1b12a-52ac-42b6-b443-a59bee67977e";
        AAresWorldSettings* WorldSettings = UWorld::GetWorld()->K2_GetWorldSettings()->Cast<AAresWorldSettings>();
        for (auto Level : WorldSettings->GetSublevelsToStreamForGameMode(_this->Class, _this->GameModeSublevelKeys))
        {
            bool Success = false;
            FName LevelName = Level.ObjectID.AssetPathName;
            ULevelStreamingDynamic::LoadLevelInstance(UWorld::GetWorld(), UKismetStringLibrary::Conv_NameToString(LevelName), FVector(), FRotator(), &Success);
        }
        bInit = true;
        return false;
    }

    static bool bFirst = true;
    printf("[Runtime] ReadyToStartMatch called, gamemode class: %s\n", _this->Class->GetName().c_str());
    bool bReady = UWorld::GetWorld()->NetDriver ? UWorld::GetWorld()->NetDriver->ClientConnections.Num() > 0 : false;
    if (bReady && bFirst)
    {
    /*    bFirst = false;

        UStateComponent* StateComponent = _this->StateMachine->GetCurrentState();
        if (StateComponent)
        {
            if (UTimeGameStateComponent* TimeState = StateComponent->Cast<UTimeGameStateComponent>())
            {
                printf("[Runtime] NextGameState: %s\n", TimeState->NextGameState->GetName().c_str());
                _this->OnRoundPlayersReady.Process();
                TimeState->SetNewTimeoutTime(0.05f);
                UStateComponent* Next = nullptr;

                for (auto& Pair : _this->StateMachine->States)
                {
                    UStateComponent* State = Pair.Key();
                    if (!State)
                        continue;

                    std::string Name = State->GetName();

                    if (Name.find("Setup") != std::string::npos)
                    {
                        Next = State;
                        break;
                    }
                }

                if (UTimeGameStateComponent* NextState = Next->Cast<UTimeGameStateComponent>())
                {
                    TimeState->GoToStateAndSkipTimedEvents(NextState, 0.05f);

                    UFunction* ResetAllPlayers = _this->Class->FindFunction("ResetAllPlayers");
                    if (ResetAllPlayers)
                        _this->ProcessEvent(ResetAllPlayers, nullptr);
                    _this->AuthResetRound(false);

                    const TArray<UBaseTeamComponent*>& Teams = GameState->GetAllTeamComponents();

                    for (UBaseTeamComponent* Team : Teams)
                    {
                        if (!Team)
                            continue;

                        _this->DisablePlayerStartsByTagAndAlliance(TEXT("None"), Team, EAresAlliance::Alliance_Neutral);
                        _this->EnablePlayerStartsByTagAndAlliance(TEXT("None"), Team, EAresAlliance::Alliance_Ally);
                    }
                }
            }
        }*/
    }
    return bReady;
}

APawn* ShooterGameMode::SpawnDefaultPawnFor(AShooterGameMode* _this, AShooterPlayerController* NewPlayer, AActor* StartSpot)
{
    AShooterPlayerState* PlayerState = NewPlayer->PlayerState->Cast<AShooterPlayerState>();
    AShooterGameState* GameState = _this->GameState->Cast<AShooterGameState>();

    APawn* Pawn = (APawn*)SpawnActor(_this->GetDefaultPawnClassForController(NewPlayer), StartSpot->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);

    while (!Pawn)
    {
        auto PlayerStart = _this->ChoosePlayerStart(NewPlayer);
        if (PlayerStart)
            Pawn = (APawn*)SpawnActor(_this->GetDefaultPawnClassForController(NewPlayer), StartSpot->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
    }

    GameState->MulticastSetPhase(EAresGamePhase::InRound);
    GameState->Phase = EAresGamePhase::InRound;
    _this->OnPhaseChange.Process(EAresGamePhase::InRound);
    _this->OnPlayerSpawned.Process(Pawn);

    NewPlayer->CachedShooterCharacter = Pawn->Cast<AShooterCharacter>();
    PlayerState->PossessedCharacter = NewPlayer->CachedShooterCharacter;
    PlayerState->SpawnedCharacterState.SpawnedCharacter = NewPlayer->CachedShooterCharacter;
    PlayerState->SpawnedCharacterState.bIsAlive = true;
    PlayerState->OnRep_PossessedCharacter();
    PlayerState->OnRep_SpawnedCharacterState({});

    NewPlayer->PlayerViewTarget = Pawn;
    NewPlayer->OnRep_ViewTarget(nullptr);

    auto Equippables = NewPlayer->CachedShooterCharacter->StartingEquippableClasses;

    for (int i = 0; i < Equippables.Num(); i++)
    {
        TSubclassOf<AAresEquippable> EquippableClass = Equippables[i];
        if (!EquippableClass)
            continue;

        NewPlayer->CachedShooterCharacter->Inventory->AuthCreateAndAddEquippable(EquippableClass);
    }

    AAresEquippable* Melee = NewPlayer->CachedShooterCharacter->Inventory->ItemSlots[(uint8)EAresItemSlot::Melee]->Contents->Cast<AAresEquippable>();

    NewPlayer->CachedShooterCharacter->Inventory->OnRep_ItemSlots();
    NewPlayer->CachedShooterCharacter->OnInventoryItemsChanged();

    NewPlayer->CachedShooterCharacter->Inventory->SetDesiredEquippable(Melee, EEquipSpeed::Instant, EEquipSource::GrantEquippable);
    NewPlayer->CachedShooterCharacter->Inventory->CurrentEquippable = Melee;
    NewPlayer->CachedShooterCharacter->Inventory->CurrentEquippable->MyPawn = NewPlayer->CachedShooterCharacter;
    NewPlayer->CachedShooterCharacter->Inventory->EquippableChange.Timestamp.Character = NewPlayer->CachedShooterCharacter;
    NewPlayer->CachedShooterCharacter->Inventory->EquippableChange.Timestamp.RespawnNumber = 0;
    NewPlayer->CachedShooterCharacter->Inventory->EquippableChange.Timestamp.NetTimestamp = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
    NewPlayer->CachedShooterCharacter->Inventory->EquippableChange.NewCurrentEquippable = Melee;
    NewPlayer->CachedShooterCharacter->Inventory->CurrentEquippable->AttributeOwner = NewPlayer->CachedShooterCharacter;
    NewPlayer->CachedShooterCharacter->Inventory->CurrentEquippable->ClientItemEquipped();
    NewPlayer->CachedShooterCharacter->Inventory->LatestDesiredEquippableSlot = EAresItemSlot::Melee;

    FPendingEquippableChange PendingChange;
    PendingChange.NewCurrentEquippable = Melee;
    PendingChange.Timestamp.Character = NewPlayer->CachedShooterCharacter;
    PendingChange.Timestamp.RespawnNumber = 0;
    PendingChange.Timestamp.NetTimestamp = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

    NewPlayer->CachedShooterCharacter->Inventory->PendingEquippableChanges.Add(PendingChange);
    NewPlayer->CachedShooterCharacter->Inventory->UpdatePendingEquippableChange(UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()));
    NewPlayer->CachedShooterCharacter->Inventory->OnRep_EquippableChange();

    NewPlayer->CachedShooterCharacter->Inventory->AuthCorrectionState = EServerCorrectionState::CorrectionIssued;
    NewPlayer->CachedShooterCharacter->Inventory->OwningClientCurrentCorrectionIndex = 0;
    NewPlayer->CachedShooterCharacter->Inventory->AuthServerCorrectRepVariables.CorrectionIndex = 0;
    NewPlayer->CachedShooterCharacter->Inventory->AuthServerCorrectRepVariables.CurrentEquippable = Melee;
    NewPlayer->CachedShooterCharacter->Inventory->ClientHandleEquippableDisagreement(Melee, 0);

    NewPlayer->CachedShooterCharacter->Inventory->bInitialServerCorrectionSent = true;
    NewPlayer->CachedShooterCharacter->Inventory->bInitialServerCorrectionProcessed = true;
    NewPlayer->CachedShooterCharacter->Inventory->AuthCorrectionState = EServerCorrectionState::Agreement;
    NewPlayer->CachedShooterCharacter->Inventory->ServerRequestCorrection(0);
    NewPlayer->CachedShooterCharacter->Inventory->ClientOnEquippableAdded(Melee);
    NewPlayer->CachedShooterCharacter->InputStateComponent->OnEquippableChanged(Melee);
    NewPlayer->CachedShooterCharacter->InputStateComponent->CurrentEquippableTarget = Melee;

    printf("[Runtime] SpawnDefaultPawnFor x=%f, y=%f, z=%f\n", StartSpot->K2_GetActorLocation().X, StartSpot->K2_GetActorLocation().Y, StartSpot->K2_GetActorLocation().Z);

    return Pawn;
}

void (*HandleStartingNewPlayerOG)(AShooterGameMode* _this, AAresPlayerController* NewPlayer);
void ShooterGameMode::HandleStartingNewPlayer(AShooterGameMode* _this, AShooterPlayerController* NewPlayer)
{
    _this->bStartPlayersAsSpectators = false;
    NewPlayer->MatchID = L"88a1b12a-52ac-42b6-b443-a59bee67977e";
    NewPlayer->PlayerState->Cast<AShooterPlayerState>()->DesiredClass = FGuid(0xadd6443a, 0x41bde414, 0xf6ade58d, 0x267f4e95);
    printf("[Runtime] HandleStartingNewPlayer called");
    HandleStartingNewPlayerOG(_this, NewPlayer);
}

UClass* GetDefaultPawnClassForController(AShooterGameMode* _this, AController* InController)
{
    return FindObject<UClass>(L"/Game/Characters/Wushu/Wushu_PC.Wushu_PC_C");
}

float AuthGetPhaseRemainingTime(AShooterGameMode* _this)
{
    AShooterGameState* GameState = _this->GameState->Cast<AShooterGameState>();
    return 20.0f - UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
}

void ShooterGameMode::Init()
{
    Hooking::Hook<AShooterGameMode>(0x838 / 8, ReadyToStartMatch);
    Hooking::Hook<AShooterGameMode>(0x668 / 8, SpawnDefaultPawnFor);
    Hooking::Hook<AShooterGameMode>(0x698 / 8, HandleStartingNewPlayer, HandleStartingNewPlayerOG);
    Hooking::Hook(ImageBase + 0x19FFC00, AuthGetPhaseRemainingTime);
    Hooking::Hook<AShooterGameMode>(0x778 / 8, AGameMode::GetDefaultObj()->VTable[0x778 / 8]); // postlogin, fuck you riot..
    Hooking::Hook<AShooterGameMode>(0x7A8 / 8, AGameMode::GetDefaultObj()->VTable[0x7A8 / 8]); // restartplayer
    Hooking::Hook<AShooterGameMode>(0x6A8 / 8, GetDefaultPawnClassForController);
}

#include <pch.h>
#include <Headers/Modules/ShooterGame/ShooterGameMode.hpp>
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
        bFirst = false;

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
        }
    }
    return bReady;
}
void ClientDrawDebugSpheres(const TArray<struct FAresDebugSphereReplicated>& DebugSpheres);
void ClientEndOnlineGame();
void ClientForceSetControlRotation(const struct FRotator& NewRotation);
void ClientGamePhaseBegin(EAresGamePhase NewPhase);
void ClientGamePhaseEnded(EAresGamePhase OldPhase);
void ClientGameStarted();
void ClientOnWinningTeam(const class UBaseTeamComponent* WinningTeam);
void ClientReceiveRemoteCharacterUpdates(const TArray<struct FRemoteCharacterUpdate>& AllyRemoteCharacterUpdates, const TArray<struct FRemoteCharacterUpdate>& EnemyAndNeutralRemoteCharacterUpdates);
void ClientReceiveRemoteCharacterUpdatesNoAlly(const TArray<struct FRemoteCharacterUpdate>& EnemyAndNeutralRemoteCharacterUpdates);
void ClientSaveRiotProfiling();
void ClientSendSystemMessage(class APlayerState* SourcePlayerState, class APlayerState* TargetPlayerState, const class FText& S);
void ClientStartOnlineGame();
APawn* ShooterGameMode::SpawnDefaultPawnFor(AShooterGameMode* _this, AShooterPlayerController* NewPlayer, AActor* StartSpot)
{
    APawn* Pawn = (APawn*)SpawnActor(_this->GetDefaultPawnClassForController(NewPlayer), StartSpot->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);

    while (!Pawn)
    {
        auto PlayerStart = _this->ChoosePlayerStart(NewPlayer);
        if (PlayerStart)
            Pawn = (APawn*)SpawnActor(_this->GetDefaultPawnClassForController(NewPlayer), StartSpot->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
    }
    printf("[Runtime] SpawnDefaultPawnFor called: pawn: %s\n", Pawn->GetName().c_str());

    NewPlayer->CachedShooterCharacter = Pawn->Cast<AShooterCharacter>();
    NewPlayer->Pawn = Pawn;
    NewPlayer->CachedShooterCharacter->SetOwner(NewPlayer);
    NewPlayer->AuthPossessSpawnedCharacter();
    NewPlayer->OnCharacterRespawned.Process(NewPlayer->CachedShooterCharacter);
    NewPlayer->ClientStartOnlineGame();
    NewPlayer->ClientGamePhaseBegin(EAresGamePhase::GameStarted);
    NewPlayer->ClientGameStarted();
    _this->OnPhaseChanged(EAresGamePhase::GameStarted);
    _this->GameState->Cast<AShooterGameState>()->MulticastSetPhase(EAresGamePhase::GameStarted);
    NewPlayer->PlayerState->Cast<AShooterPlayerState>()->PossessedCharacter = NewPlayer->CachedShooterCharacter;
    NewPlayer->PlayerState->Cast<AShooterPlayerState>()->SpawnedCharacterState.SpawnedCharacter = NewPlayer->CachedShooterCharacter;
    NewPlayer->PlayerState->Cast<AShooterPlayerState>()->SpawnedCharacterState.bIsAlive = true;
    NewPlayer->PlayerState->Cast<AShooterPlayerState>()->OnRep_PossessedCharacter();
    NewPlayer->PlayerState->Cast<AShooterPlayerState>()->OnRep_SpawnedCharacterState({});
    NewPlayer->PlayerViewTargetMode = EAresPlayerViewTargetMode::WatchPossessed;
    NewPlayer->OnRep_ViewTargetMode();
    NewPlayer->CachedShooterCharacter->UpdatePawnMeshes();
    NewPlayer->CachedShooterCharacter->UpdateTeam();
    NewPlayer->CachedShooterCharacter->UpdateTeamColorsAndThemes();

    printf("[Runtime] SpawnDefaultPawnFor x=%f, y=%f, z=%f\n", StartSpot->K2_GetActorLocation().X, StartSpot->K2_GetActorLocation().Y, StartSpot->K2_GetActorLocation().Z);
    
    return Pawn;
}

void (*HandleStartingNewPlayerOG)(AShooterGameMode* _this, AAresPlayerController* NewPlayer);
void ShooterGameMode::HandleStartingNewPlayer(AShooterGameMode* _this, AShooterPlayerController* NewPlayer)
{
    _this->bStartPlayersAsSpectators = false; 
    NewPlayer->MatchID = L"88a1b12a-52ac-42b6-b443-a59bee67977e";

    printf("[Runtime] HandleStartingNewPlayer called");
    HandleStartingNewPlayerOG(_this, NewPlayer);
}

bool AuthIsServerStreamingLevels(AShooterGameMode* _this, FFrame* Stack, bool* Ret)
{
    printf("[Runtime] AuthIsServerStreamingLevels called");
    return *Ret = false;
}

char(*NotifyControlMessageOG)(__int64 a1, __int64 a2, unsigned __int8 a3, __int64 a4);
char __fastcall NotifyControlMessage(__int64 a1, __int64 a2, unsigned __int8 a3, __int64 a4)
{
    printf("[Runtime] NotifyControlMessage called, type: %d\n", a3);
    return NotifyControlMessageOG(a1, a2, a3, a4);
}

UClass* GetDefaultPawnClassForController(AShooterGameMode* _this, AController* InController)
{
    return FindObject<UClass>(L"/Game/Characters/Wushu/Wushu_PC.Wushu_PC_C");
}

void ShooterGameMode::Init()
{
    Hooking::Hook<AShooterGameMode>(0x838 / 8, ReadyToStartMatch);
    Hooking::Hook<AShooterGameMode>(0x668 / 8, SpawnDefaultPawnFor);
    Hooking::Hook<AShooterGameMode>(0x698 / 8, HandleStartingNewPlayer, HandleStartingNewPlayerOG);
    Hooking::ExecHook(FindObject<UFunction>(L"/Script/ShooterGame.ShooterGameMode.AuthIsServerStreamingLevels"), AuthIsServerStreamingLevels);
    Hooking::Hook(ImageBase + 0x3A74410, NotifyControlMessage, NotifyControlMessageOG);
    Hooking::Hook<AShooterGameMode>(0x778 / 8, AGameMode::GetDefaultObj()->VTable[0x778 / 8]); //postlogin, fuck you riot..
    Hooking::Hook<AShooterGameMode>(0x7A8 / 8, AGameMode::GetDefaultObj()->VTable[0x7A8 / 8]); // restartplayer
    Hooking::Hook<AShooterGameMode>(0x6A8 / 8, GetDefaultPawnClassForController);
}

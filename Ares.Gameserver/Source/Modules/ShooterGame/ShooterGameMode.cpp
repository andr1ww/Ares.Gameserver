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

APawn* ShooterGameMode::SpawnDefaultPawnFor(AShooterGameMode* GameMode, AAresPlayerController* NewPlayer, AActor* StartSpot)
{
    printf("[Runtime] SpawnDefaultPawnFor called");
    APawn* Pawn = (APawn*)SpawnActor(GameMode->GetDefaultPawnClassForController(NewPlayer), StartSpot->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);

    while (!Pawn)
    {
        auto PlayerStart = GameMode->ChoosePlayerStart(NewPlayer);
        if (PlayerStart)
            Pawn = (APawn*)SpawnActor(GameMode->GetDefaultPawnClassForController(NewPlayer), StartSpot->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
    }
    
    return Pawn;
}

void (*HandleStartingNewPlayerOG)(AShooterGameMode* _this, AAresPlayerController* NewPlayer);
void ShooterGameMode::HandleStartingNewPlayer(AShooterGameMode* _this, AAresPlayerController* NewPlayer)
{
    printf("[Runtime] HandleStartingNewPlayer called");
    HandleStartingNewPlayerOG(_this, NewPlayer);
}

bool AuthIsServerStreamingLevels(AShooterGameMode* _this, FFrame* Stack, bool* Ret)
{
    printf("[Runtime] AuthIsServerStreamingLevels called");
    return *Ret = false;
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
    Hooking::Hook<AShooterGameMode>(0x6A8 / 8, GetDefaultPawnClassForController);
    Hooking::ExecHook(FindObject<UFunction>(L"/Script/ShooterGame.ShooterGameMode.AuthIsServerStreamingLevels"), AuthIsServerStreamingLevels);
}

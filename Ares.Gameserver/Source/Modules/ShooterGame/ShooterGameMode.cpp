#include <pch.h>
#include <Headers/Modules/ShooterGame/ShooterGameMode.hpp>
INIT_MODULE(ShooterGameMode);

bool ShooterGameMode::ReadyToStartMatch(AShooterGameMode* _this)
{
    static bool bInit = false;
    if (!bInit)
    {
        AShooterGameState* GameState = _this->GameState->Cast<AShooterGameState>();
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

    printf("[Runtime] ReadyToStartMatch called, gamemode class: %s\n", _this->Class->GetName().c_str());
    bool bReady = UWorld::GetWorld()->NetDriver ? UWorld::GetWorld()->NetDriver->ClientConnections.Num() > 0 : false;
    if (bReady)
    {
        _this->AuthStartMatch();
        printf("[Runtime] Match started!\n");
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

void ShooterGameMode::Init()
{
    Hooking::Hook<AShooterGameMode>(0x838 / 8, ReadyToStartMatch);
    Hooking::Hook<AShooterGameMode>(0x668 / 8, SpawnDefaultPawnFor);
    Hooking::Hook<AShooterGameMode>(0x698 / 8, HandleStartingNewPlayer, HandleStartingNewPlayerOG);
}

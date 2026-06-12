#pragma once
#include "pch.h"

class ShooterGameMode
{
public:
    static void Init();

private:
    static bool ReadyToStartMatch(AShooterGameMode* _this);
    static APawn* SpawnDefaultPawnFor(AShooterGameMode* GameMode, AAresPlayerController* NewPlayer, AActor* StartSpot);
    static void HandleStartingNewPlayer(AShooterGameMode* _this, AAresPlayerController* NewPlayer);
};
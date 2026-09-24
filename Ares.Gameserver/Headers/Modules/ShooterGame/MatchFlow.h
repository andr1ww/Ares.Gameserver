#pragma once

// intro state has no next state offline, push it along
namespace MatchFlow
{
    void Tick();

    void EnterRoundPhase(AShooterGameState* State);

    bool EndlessRound();
    void SetEndlessRound(bool bOn);

    void SetHoldRespawn(bool bOn);
    bool RespawnNow();
}

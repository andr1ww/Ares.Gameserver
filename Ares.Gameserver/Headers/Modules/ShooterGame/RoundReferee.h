#pragma once
#include <string>

namespace RoundReferee
{
    enum class EPhase { Off, Buy, Live, Planted, Ended };

    struct FView
    {
        EPhase Phase = EPhase::Off;
        int Round = 0, AttackScore = 0, DefenseScore = 0;
        double PhaseStarted = 0, PhaseEnds = 0;
        class UBaseTeamComponent* Attackers = nullptr;
        class UBaseTeamComponent* Defenders = nullptr;
        class AShooterCharacter* Carrier = nullptr;
        bool SpikeDown = false;
        float SpikeX = 0, SpikeY = 0, SpikeZ = 0;
        int PlantSite = -1;                              // 0 = A, 1 = B
        float PlantProgress = 0, DefuseProgress = 0;
    };

    constexpr float PlantSeconds = 4.0f, DefuseSeconds = 7.0f, SpikeSeconds = 45.0f;
    constexpr float RoundSeconds = 100.0f, BuySeconds = 10.0f, EndSeconds = 5.0f;

    const FView& View();
    bool Active();
    double TimeLeft();

    int SiteAt(float X, float Y, float Z);
    bool WorkPlant(class AShooterCharacter* Who, float Dt);
    bool WorkDefuse(class AShooterCharacter* Who, float Dt);

    std::string Command(const std::string& Line);
    void Tick();
}

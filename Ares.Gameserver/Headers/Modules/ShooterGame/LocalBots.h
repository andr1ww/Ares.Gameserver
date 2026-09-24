#pragma once
#include <string>
#include <vector>
namespace LocalBots
{
    enum class EBehaviour
    {
        Still,
        Walk,
        Jump,
        Strafe,
        Combat,
    };

    struct FOptions
    {
        EBehaviour Behaviour = EBehaviour::Still;
        bool bGravity = true;
        bool bWatch = false;
        const wchar_t* WeaponPath = nullptr;
        const char* WeaponName = nullptr;
    };

    void Register(class AShooterCharacter* Bot, const FOptions& Options);
    int ClearBodies();
    int ClearTactical();
    std::vector<class AShooterCharacter*> Tactical();
    void DestroyCharacter(class AShooterCharacter* Character);
    // destroying the player body crashes next pawn setup, so just hide it
    void RetireCharacter(class AShooterCharacter* Character);
    void SetIgnored(class APawn* Pawn);
    std::vector<class AShooterCharacter*> Characters();
    void Tick();
    bool IsUpdating();
    // botorder hold|push|auto|stop|clear
    std::string Order(const std::string& Text);
    // botskill easy|normal|hard|pro, botawareness fair|exact, botinfo
    std::string Configure(const std::string& Command, const std::string& Argument);
    std::string BrainStatus();
}

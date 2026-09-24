#include <pch.h>
#include <Headers/Modules/ShooterGame/MatchFlow.h>
#include <Headers/Modules/ShooterGame/AresInventory.h>
#include <Headers/Modules/ShooterGame/LocalBots.h>

namespace
{
    // hide corpses, destroying them crashes respawns
    struct FCorpse { TWeakObjectPtr<AActor> Body; double At = 0; };
    std::vector<FCorpse> Corpses;
    constexpr double kStuckSeconds = 3.0;
    constexpr double kPawnGraceSeconds = 5.0;

    constexpr int kPlayRank = 50;

    int RankOf(const std::string& Name)
    {
        if (Name.find("LoadIn") != std::string::npos)
            return 0;
        if (Name.find("Intro") != std::string::npos)
            return 10;
        if (Name.find("Setup") != std::string::npos)
            return 20;
        // buy phase isnt play, gameplay is inactive there
        if (Name.find("BuyPhase") != std::string::npos)
            return 30;
        if (Name.find("BombPlanted") != std::string::npos)
            return 60;
        if (Name.find("PostRound") != std::string::npos)
            return 80;
        if (Name.find("TeamSwitch") != std::string::npos)
            return 85;
        if (Name.find("GameOver") != std::string::npos)
            return 90;
        if (Name.find("Vote") != std::string::npos)
            return 95;
        if (Name.find("AwaitingShutdown") != std::string::npos)
            return 99;
        return kPlayRank;
    }

    constexpr int kTerminalRank = 80;

    bool bEndlessRound = true;
    bool bHoldRespawn = false;

    UBaseGameStateComponent* PlayState(UStateMachineComponent* Machine)
    {
        for (auto& Pair : Machine->States)
        {
            UStateComponent* State = Pair.Key();
            if (!State)
                continue;
            if (RankOf(State->GetName()) != kPlayRank)
                continue;
            if (auto GameState = State->Cast<UBaseGameStateComponent>())
                return GameState;
        }
        return nullptr;
    }

    UBaseGameStateComponent* NextState(UStateMachineComponent* Machine, int CurrentRank)
    {
        UBaseGameStateComponent* Best = nullptr;
        int BestRank = 1000;

        for (auto& Pair : Machine->States)
        {
            UStateComponent* State = Pair.Key();
            if (!State)
                continue;

            UBaseGameStateComponent* GameState = State->Cast<UBaseGameStateComponent>();
            if (!GameState)
                continue;

            const int Rank = RankOf(State->GetName());
            if (Rank > CurrentRank && Rank <= kPlayRank && Rank < BestRank)
            {
                Best = GameState;
                BestRank = Rank;
            }
        }
        return Best;
    }

    // hud reads DesiredClass not the pawn, shows jett icons otherwise
    void MatchClassToPawn(AShooterPlayerState* State, UClass* PawnClass)
    {
        if (!State || !PawnClass)
            return;

        // Wraith_PC_C -> Wraith
        std::string Codename = PawnClass->GetName();
        const size_t Suffix = Codename.find("_PC");
        if (Suffix != std::string::npos)
            Codename = Codename.substr(0, Suffix);

        // these only exist as CDOs
        for (UCharacterDataAsset* Asset : GetObjectsOfClass<UCharacterDataAsset>())
        {
            if (!Asset)
                continue;
            if (Asset->DeveloperName.ToString() != Codename)
                continue;

            if (State->AuthSetDesiredClass(Asset->Uuid))
                Ares::Log("agent: player state class set to %s", Codename.c_str());
            else
                Ares::Log("agent: AuthSetDesiredClass refused %s", Codename.c_str());

            // Auth* is stubbed sometimes, set it anyway
            State->DesiredClass = Asset->Uuid;
            State->OnRep_DesiredClass();
            return;
        }
        Ares::Log("agent: no UCharacterDataAsset named %s", Codename.c_str());
    }

    // range side spawns (bhop course etc), dont spawn there
    bool IsSideAttraction(const std::string& Name)
    {
        static const char* Attractions[] = {"BHop", "DanceHall", "Aimbots", "NPE_", "Defuse"};
        for (const char* Pattern : Attractions)
            if (Name.find(Pattern) != std::string::npos)
                return true;
        return false;
    }

    AActor* FindStart(AShooterGameMode* GameMode, APlayerController* PC)
    {
        APlayerStart* Fallback = nullptr;
        for (APlayerStart* Start : GetObjectsOfClass<APlayerStart>())
        {
            if (!Start || Start->IsDefaultObject())
                continue;

            const std::string Name = Start->GetName();
            if (Name.find("Main") != std::string::npos)
            {
                Ares::Log("flow: using the map's main spawn (%s)", Name.c_str());
                return Start;
            }
            if (!Fallback && !IsSideAttraction(Name))
                Fallback = Start;
        }

        // bomb maps have no main start
        if (AActor* Chosen = GameMode->ChoosePlayerStart(PC))
            return Chosen;

        return Fallback;
    }

    // riot's PlayerCanRestart refuses outside their flow
    void EnsurePawn(AShooterGameMode* GameMode)
    {
        for (APlayerController* PC : GetObjectsOfClass<APlayerController>())
        {
            if (!PC || PC->IsDefaultObject())
                continue;

            // respawn manager is stripped, dead players keep the corpse
            if (auto Body = PC->Pawn ? PC->Pawn->Cast<AShooterCharacter>() : nullptr)
            {
                if (!Body->IsAlive())
                {
                    if (bHoldRespawn)
                        continue;
                    Ares::Log("flow: %s is dead; releasing %s", PC->GetName().c_str(),
                              Body->GetName().c_str());
                    PC->UnPossess();
                    Corpses.push_back({Body, Ares::Seconds()});
                }
            }
            if (PC->Pawn)
                continue;

            GameMode->EnablePlayerStarts();

            AActor* Start = FindStart(GameMode, PC);
            if (!Start)
            {
                Ares::Log("flow: no player start available for %s", PC->GetName().c_str());
                continue;
            }

            // 0x668 SpawnDefaultPawnFor, hooked in ShooterGameMode
            using SpawnDefaultPawnForFn = APawn* (*)(AShooterGameMode*, APlayerController*, AActor*);
            auto Spawn = (SpawnDefaultPawnForFn)GameMode->VTable[0x668 / 8];

            Ares::Log("flow: spawning a pawn for %s at %s (thread %lu, game thread %lu)",
                      PC->GetName().c_str(), Start->GetName().c_str(), GetCurrentThreadId(), Ares::GameThreadId);
            APawn* Pawn = Spawn(GameMode, PC, Start);
            if (!Pawn)
                continue;

            PC->Possess(Pawn);
            PC->ClientRestart(Pawn);
            if (auto State = PC->PlayerState ? PC->PlayerState->Cast<AShooterPlayerState>() : nullptr)
                MatchClassToPawn(State, Pawn->Class);
            if (auto Character = Pawn->Cast<AShooterCharacter>())
            {
                Ares::Log("loadout: filled %d ability resource(s)", AresInventory::RefillAbilities(Character));
            }
            Ares::Log("flow: %s now possesses %s", PC->GetName().c_str(), Pawn->GetName().c_str());
        }
    }
} // namespace

void MatchFlow::EnterRoundPhase(AShooterGameState* State)
{
    if (!State)
        return;

    constexpr int32 kAllInputAllowed = (1 << 5) - 1; // every EAllowedGameInput bit

    // this is what gates abilities, -1 timestamps so it doesnt get flipped back
    FAresGameInfo& Info = State->GameInfo;
    const bool bGameplayInactive = !Info.bGameplayActive;
    if (bGameplayInactive)
    {
        Info.bGameplayActive = true;
        Info.LocalGameplayStartTimestamp = -1.0f;
        Info.LocalGameplayEndTimestamp = -1.0f;
        State->OnGameplayActiveChanged.Process(true);
    }

    const bool bPhaseChanged = State->Phase != EAresGamePhase::InRound;
    const bool bInputChanged = State->PhaseAllowedGameInput != kAllInputAllowed;
    if (!bPhaseChanged && !bInputChanged && !bGameplayInactive)
        return;

    Ares::Log("phase: %d -> InRound, allowed input 0x%X -> 0x%X, gameplay active %d -> %d",
              (int)State->Phase, State->PhaseAllowedGameInput, kAllInputAllowed,
              (int)!bGameplayInactive, (int)Info.bGameplayActive);

    State->PhaseAllowedGameInput = kAllInputAllowed;
    if (bPhaseChanged)
        State->MulticastSetPhase(EAresGamePhase::InRound);

    State->Phase = EAresGamePhase::InRound;
}

bool MatchFlow::EndlessRound()
{
    return bEndlessRound;
}

void MatchFlow::SetEndlessRound(bool bOn)
{
    bEndlessRound = bOn;
}

void MatchFlow::SetHoldRespawn(bool bOn)
{
    bHoldRespawn = bOn;
}

bool MatchFlow::RespawnNow()
{
    UWorld* World = UWorld::GetWorld();
    AShooterGameMode* GameMode = World && World->AuthorityGameMode ? World->AuthorityGameMode->Cast<AShooterGameMode>() : nullptr;
    if (!GameMode)
        return false;
    EnsurePawn(GameMode);
    return true;
}

void MatchFlow::Tick()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone)
        return;

    static bool bBusy = false;
    FReentrancyGuard Guard(bBusy);
    if (!Guard.Entered())
        return;

    static double LastCheck = -1000.0;
    const double Now = Ares::Seconds();
    if (Now - LastCheck < 1.0)
        return;
    LastCheck = Now;

    for (auto It = Corpses.begin(); It != Corpses.end();)
    {
        auto Body = It->Body.Get();
        if (!Body || Body->bActorIsBeingDestroyed) { It = Corpses.erase(It); continue; }
        if (Now - It->At > 12.0) { Body->SetActorHiddenInGame(true); It = Corpses.erase(It); continue; }
        ++It;
    }

    UWorld* World = UWorld::GetWorld();
    if (!World || !World->AuthorityGameMode)
        return;

    AShooterGameMode* GameMode = World->AuthorityGameMode->Cast<AShooterGameMode>();
    if (!GameMode || GameMode->MatchState.ToString() != "InProgress")
        return;

    UStateMachineComponent* Machine = GameMode->StateMachine;
    if (!Machine)
        return;

    UStateComponent* Current = Machine->AuthCurrentState.CurrentState;
    if (!Current)
        return;

    static void* LastSeenState = nullptr;
    static double StateSince = 0.0;
    if (Current != LastSeenState)
    {
        LastSeenState = Current;
        StateSince = Now;
        Ares::Log("flow: state is now %s", Current->GetName().c_str());
    }

    const std::string CurrentName = Current->GetName();
    const int CurrentRank = RankOf(CurrentName);

    if (bEndlessRound && CurrentRank >= kTerminalRank)
    {
        if (Now - StateSince < kStuckSeconds)
            return;
        UBaseGameStateComponent* Round = PlayState(Machine);
        if (!Round)
            return;
        StateSince = Now;
        Ares::Log("flow: %s is past the round; restarting %s", CurrentName.c_str(), Round->GetName().c_str());
        if (auto Timed = Current->Cast<UTimeGameStateComponent>())
            Timed->GoToStateAndSkipTimedEvents(Round, 0.05f);
        else if (auto AsGameState = Current->Cast<UBaseGameStateComponent>())
            AsGameState->GoToState(Round, 0.05f);
        return;
    }

    if (CurrentRank >= kPlayRank)
    {
        // every tick, the mode keeps resetting it
        if (World->GameState)
            EnterRoundPhase(World->GameState->Cast<AShooterGameState>());

        if (Now - StateSince > kPawnGraceSeconds)
        {
            static double LastPawnCheck = 0.0;
            if (Now - LastPawnCheck > 5.0)
            {
                LastPawnCheck = Now;
                EnsurePawn(GameMode);
            }
        }
        return;
    }

    if (Now - StateSince < kStuckSeconds)
        return;

    UBaseGameStateComponent* Target = NextState(Machine, CurrentRank);
    if (!Target)
    {
        static bool bWarned = false;
        if (!bWarned)
        {
            bWarned = true;
            Ares::Log("flow: stuck in %s with no state to advance to", CurrentName.c_str());
        }
        return;
    }

    UBaseGameStateComponent* CurrentGameState = Current->Cast<UBaseGameStateComponent>();
    if (!CurrentGameState)
        return;

    StateSince = Now; // don't retry the same transition every second

    // timed states refuse plain GoToState
    if (UTimeGameStateComponent* Timed = Current->Cast<UTimeGameStateComponent>())
    {
        Ares::Log("flow: skipping timed state %s -> %s", CurrentName.c_str(), Target->GetName().c_str());
        Timed->GoToStateAndSkipTimedEvents(Target, 0.05f);
        return;
    }

    Ares::Log("flow: advancing %s -> %s", CurrentName.c_str(), Target->GetName().c_str());
    CurrentGameState->GoToState(Target, 0.05f);
}

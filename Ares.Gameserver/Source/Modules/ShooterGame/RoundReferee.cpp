#include <pch.h>
#include <Headers/Modules/ShooterGame/RoundReferee.h>
#include <Headers/Modules/ShooterGame/LocalBots.h>
#include <Headers/Modules/ShooterGame/LocalTeams.h>
#include <Headers/Modules/ShooterGame/MatchFlow.h>
#include <Headers/Modules/ShooterGame/TacticalMap.h>
#include <Headers/Modules/DeveloperConsole.h>
#include <sstream>
#include <chrono>

// AuthWinRound/AuthStartRound are stubs in the client
namespace
{
    using namespace RoundReferee;

    FView View_;
    struct FConfig { int Allies = 4, Enemies = 5; bool PlayerAttacks = false; } Config;
    struct FBox { char Name; FVector Min, Max; };
    std::vector<FBox> Sites;
    std::vector<FVector> AttackStarts, DefendStarts;
    TWeakObjectPtr<UWorld> SetupWorld;
    TWeakObjectPtr<AActor> SpikeActor;
    FVector SpikeHome{};
    TWeakObjectPtr<AShooterCharacter> CarrierRef, WorkerRef;
    double LastWork = -1;
    FVector WorkAt{};
    bool DefuseCheckpoint = false;
    TWeakObjectPtr<APawn> PlacedPawn;
    bool Busy = false;

    const char* Agents[] = {"sage", "jett", "omen", "sova", "phoenix", "reyna", "brimstone", "cypher", "killjoy", "raze", "viper", "breach"};

    double Clock()
    {
        auto World = UWorld::GetWorld();
        return World ? UGameplayStatics::GetTimeSeconds(World) : 0.0;
    }

    bool InWorld(UObject* Object, UWorld* World)
    {
        for (int Depth = 0; Object && Depth < 32; ++Depth, Object = Object->Outer)
        {
            if (Object == World) return true;
            if (auto Level = Object->Cast<ULevel>()) return Level->OwningWorld == World;
        }
        return false;
    }

    bool Alive(AShooterCharacter* C) { return C && !C->bActorIsBeingDestroyed && C->DamageHandler && C->DamageHandler->bAlive; }
    float HalfHeight(AShooterCharacter* C) { return C->CapsuleComponent ? C->CapsuleComponent->GetScaledCapsuleHalfHeight() : 98.f; }
    FVector FloorOf(AShooterCharacter* C) { auto P = C->K2_GetActorLocation(); P.Z -= HalfHeight(C); return P; }
    float Speed(AShooterCharacter* C) { auto V = C->GetVelocity(); return sqrtf(V.X * V.X + V.Y * V.Y); }

    AShooterCharacter* Player()
    {
        auto World = UWorld::GetWorld();
        auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
        return PC && PC->Pawn ? PC->Pawn->Cast<AShooterCharacter>() : nullptr;
    }

    UBaseTeamComponent* TeamOf(AShooterCharacter* C)
    {
        auto T = UBaseTeamComponent::GetTeamComponent(C);
        return T && T->HasValidTeam() ? T : nullptr;
    }

    double ScanMs = 0; long ScanCount = 0; double NextPerf = 0;
    std::vector<AShooterCharacter*> AliveOn(UBaseTeamComponent* Team)
    {
        const auto T0 = std::chrono::steady_clock::now();
        struct FDone { std::chrono::steady_clock::time_point T0; ~FDone() { ScanMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - T0).count(); ++ScanCount; } } Done{T0};
        std::vector<AShooterCharacter*> Out;
        auto World = UWorld::GetWorld();
        // cached roster, full object scan is way too slow
        for (auto C : LocalBots::Characters())
            if (C && !C->IsDefaultObject() && InWorld(C, World) && Alive(C) && TeamOf(C) == Team) Out.push_back(C);
        return Out;
    }

    bool Setup()
    {
        auto World = UWorld::GetWorld();
        if (!World) return false;
        if (SetupWorld.Get() == World && !Sites.empty()) return true;
        Sites.clear(); AttackStarts.clear(); DefendStarts.clear(); SpikeActor = nullptr;
        FVector BombSpawn{}; bool HaveBomb = false;
        std::vector<std::pair<std::string, FVector>> Starts;
        for (auto A : GetObjectsOfClass<AActor>())
        {
            if (!A || A->IsDefaultObject() || A->bActorIsBeingDestroyed || !InWorld(A, World)) continue;
            const std::string Class = A->Class->GetName(), Name = A->GetName();
            if (Class == "BP_BombDestinationVolume_C" && !Name.empty())
            {
                FVector C{}, E{}; A->GetActorBounds(false, &C, &E);
                Sites.push_back({Name.back(), C - E, C + E});
            }
            else if (Class == "BombSpawnLocation_C") { BombSpawn = A->K2_GetActorLocation(); HaveBomb = true; }
            else if (Class == "BombEquippable_C") { SpikeActor = A; SpikeHome = A->K2_GetActorLocation(); }
            else if (auto Start = A->Cast<AShooterTeamStart>())
                Starts.push_back({Start->TeamComponent ? Start->TeamComponent->GetTeamName().ToString() : "none", A->K2_GetActorLocation()});
        }
        std::sort(Sites.begin(), Sites.end(), [](auto& A, auto& B) { return A.Name < B.Name; });
        if (Sites.size() < 2 || Starts.empty() || !HaveBomb) return false;
        // starts have no team here, split by distance to the spike spawn
        auto Dist = [](const FVector& A, const FVector& B) { const FVector D = A - B; return sqrtf(D.X * D.X + D.Y * D.Y + D.Z * D.Z); };
        FVector Atk = Starts[0].second, Def = Starts[0].second;
        for (auto& [Team, P] : Starts)
        {
            if (Dist(P, BombSpawn) < Dist(Atk, BombSpawn)) Atk = P;
            if (Dist(P, BombSpawn) > Dist(Def, BombSpawn)) Def = P;
        }
        for (auto& [Team, P] : Starts) (Dist(P, Atk) < Dist(P, Def) ? AttackStarts : DefendStarts).push_back(P);
        SetupWorld = World;
        Ares::Log("rounds: sites %c/%c, %zu attacker and %zu defender starts, spike actor %s", Sites[0].Name, Sites[1].Name,
                  AttackStarts.size(), DefendStarts.size(), SpikeActor.Get() ? "found" : "missing");
        return !AttackStarts.empty() && !DefendStarts.empty();
    }

    void Announce(const char* Format, ...)
    {
        char Text[512];
        va_list Args; va_start(Args, Format); vsnprintf(Text, sizeof Text, Format, Args); va_end(Args);
        Ares::Log("rounds: %s", Text);
    }

    // clock first, GetTimeSeconds reenters Tick
    void SetPhase(EPhase Phase, double Duration)
    {
        const double Now = Clock();
        View_.PhaseStarted = Now;
        View_.PhaseEnds = Now + Duration;
        View_.Phase = Phase;
    }

    void MoveSpikeActor(const FVector& Where)
    {
        if (auto A = SpikeActor.Get())
        {
            FHitResult Hit{};
            A->K2_SetActorLocation(Where, false, &Hit, true);
        }
    }

    void EndRound(bool AttackersWin, const char* Why)
    {
        (AttackersWin ? View_.AttackScore : View_.DefenseScore)++;
        Announce("round %d: %s win (%s). Score attack %d - defense %d", View_.Round, AttackersWin ? "ATTACKERS" : "DEFENDERS", Why,
                 View_.AttackScore, View_.DefenseScore);
        SetPhase(EPhase::Ended, EndSeconds);
        View_.Carrier = nullptr; CarrierRef = nullptr;
        WorkerRef = nullptr;
    }

    // some starts are inside a blocking volume, snap to the nav grid
    FVector Snap(FVector At)
    {
        if (auto G = TacticalMap::Get())
        {
            const int N = G->Nearest(At.X, At.Y, At.Z - 92.f, 900.f);
            if (N >= 0) At = FVector{G->Nodes[N].X, G->Nodes[N].Y, G->Nodes[N].Z + 100.f};
        }
        return At;
    }

    void Spawn(const std::string& Agent, bool Ally, const char* Weapon, FVector At)
    {
        At = Snap(At);
        char Line[256];
        snprintf(Line, sizeof Line, "spawn %s %s combat %s at %.1f %.1f %.1f", Agent.c_str(), Ally ? "ally" : "enemy", Weapon, At.X, At.Y, At.Z + 20);
        DeveloperConsole::Run(Line);
    }

    const char* WeaponFor(int Round, bool Attacking, int Slot)
    {
        const bool Pistol = Round == 1 || Round == 13;
        if (Pistol) { static const char* P[] = {"classic", "ghost", "sheriff", "ghost", "classic"}; return P[Slot % 5]; }
        (void)Attacking;
        return (Slot % 2) ? "phantom" : "vandal";
    }

    void BeginRound()
    {
        auto World = UWorld::GetWorld();
        auto State = World && World->GameState ? World->GameState->Cast<AShooterGameState>() : nullptr;
        if (!State || State->TeamComponents.Num() < 2) { Announce("need two teams on this map"); View_.Phase = EPhase::Off; return; }
        ++View_.Round;
        if (View_.Round == 13)
        {
            Config.PlayerAttacks = !Config.PlayerAttacks;
            std::swap(View_.AttackScore, View_.DefenseScore);
            Announce("half time: sides switch (you now %s)", Config.PlayerAttacks ? "attack" : "defend");
        }
        auto Me = Player();
        UBaseTeamComponent* Mine = Me ? LocalTeams::Assigned(Me) : nullptr;
        if (!Mine && Me && Me->PlayerState) Mine = LocalTeams::Assigned(Me->PlayerState);
        if (!Mine) Mine = State->TeamComponents[0].Team;
        UBaseTeamComponent* Theirs = nullptr;
        for (const auto& E : State->TeamComponents) if (E.Team && E.Team != Mine) Theirs = E.Team;
        if (Me) LocalTeams::Assign(Me, Mine);
        View_.Attackers = Config.PlayerAttacks ? Mine : Theirs;
        View_.Defenders = Config.PlayerAttacks ? Theirs : Mine;

        LocalBots::ClearTactical();
        LocalBots::ClearBodies();
        MatchFlow::SetHoldRespawn(false);
        if (Me)
        {
            if (auto PC = UGameplayStatics::GetPlayerController(World, 0)) PC->UnPossess();
            LocalBots::RetireCharacter(Me);   // destroying it crashes respawns
        }
        MatchFlow::RespawnNow();
        PlacedPawn = nullptr;

        auto& MyStarts = Config.PlayerAttacks ? AttackStarts : DefendStarts;
        auto& OtherStarts = Config.PlayerAttacks ? DefendStarts : AttackStarts;
        int AgentIndex = (View_.Round * 3) % 12;
        for (int I = 0; I < Config.Allies; ++I)
        {
            FVector At = MyStarts[(I + 1) % MyStarts.size()];
            if (I + 1 >= (int)MyStarts.size()) At.X += 120.f * (1 + I / (int)MyStarts.size());
            Spawn(Agents[AgentIndex++ % 12], true, WeaponFor(View_.Round, Config.PlayerAttacks, I), At);
        }
        for (int I = 0; I < Config.Enemies; ++I)
        {
            FVector At = OtherStarts[I % OtherStarts.size()];
            if (I >= (int)OtherStarts.size()) At.X += 120.f * (I / (int)OtherStarts.size());
            Spawn(Agents[AgentIndex++ % 12], false, WeaponFor(View_.Round, !Config.PlayerAttacks, I), At);
        }
        View_.Carrier = nullptr; CarrierRef = nullptr;
        std::vector<AShooterCharacter*> Candidates;
        for (auto B : LocalBots::Tactical()) if (TeamOf(B) == View_.Attackers) Candidates.push_back(B);
        if (!Candidates.empty()) CarrierRef = Candidates[View_.Round % Candidates.size()];
        View_.Carrier = CarrierRef.Get();
        View_.SpikeDown = false; View_.PlantSite = -1; View_.PlantProgress = View_.DefuseProgress = 0;
        DefuseCheckpoint = false; WorkerRef = nullptr;
        MoveSpikeActor(SpikeHome);
        SetPhase(EPhase::Buy, BuySeconds);
        Announce("round %d: you %s with %d allies against %d, spike carrier %s. Buy phase %.0fs",
                 View_.Round, Config.PlayerAttacks ? "ATTACK" : "DEFEND", Config.Allies, Config.Enemies,
                 View_.Carrier ? View_.Carrier->GetName().c_str() : (Config.PlayerAttacks ? "you (crouch still in a site for 4s)" : "none"),
                 BuySeconds);
    }

    // crouch still = plant/defuse
    bool PlayerWorking(AShooterCharacter* Me)
    {
        return Me && Me->CharacterMovement && Me->CharacterMovement->IsCrouching() && Speed(Me) < 20.f;
    }

    bool Continuing(AShooterCharacter* Who)
    {
        const double Now = Clock();
        const FVector Here = FloorOf(Who);
        const FVector D = Here - WorkAt;
        const bool Same = WorkerRef.Get() == Who && Now - LastWork < 0.3 && sqrtf(D.X * D.X + D.Y * D.Y) < 40.f;
        LastWork = Now;
        if (!Same) { WorkerRef = Who; WorkAt = Here; }
        return Same;
    }
}

const FView& RoundReferee::View() { return View_; }
bool RoundReferee::Active() { return View_.Phase != EPhase::Off; }
double RoundReferee::TimeLeft() { return View_.Phase == EPhase::Off ? 0.0 : std::max(0.0, View_.PhaseEnds - Clock()); }

int RoundReferee::SiteAt(float X, float Y, float Z)
{
    for (size_t I = 0; I < Sites.size(); ++I)
    {
        auto& S = Sites[I];
        if (X >= S.Min.X && X <= S.Max.X && Y >= S.Min.Y && Y <= S.Max.Y && Z >= S.Min.Z - 60 && Z <= S.Max.Z) return (int)I;
    }
    return -1;
}

bool RoundReferee::WorkPlant(AShooterCharacter* Who, float Dt)
{
    if (View_.Phase != EPhase::Live || !Alive(Who) || Who != CarrierRef.Get()) return false;
    const FVector F = FloorOf(Who);
    const int Site = SiteAt(F.X, F.Y, F.Z);
    if (Site < 0 || Speed(Who) > 30.f) { View_.PlantProgress = 0; return false; }
    if (!Continuing(Who)) View_.PlantProgress = 0;
    View_.PlantProgress += Dt;
    if (View_.PlantProgress < PlantSeconds) return false;
    View_.SpikeDown = true; View_.SpikeX = F.X; View_.SpikeY = F.Y; View_.SpikeZ = F.Z;
    View_.PlantSite = Site; View_.Carrier = nullptr; CarrierRef = nullptr; View_.DefuseProgress = 0;
    SetPhase(EPhase::Planted, SpikeSeconds);
    MoveSpikeActor(F + FVector{0, 0, 5});
    Announce("SPIKE PLANTED on %c by %s at %.0f %.0f %.0f - %.0fs to detonation", Sites[Site].Name, Who->GetName().c_str(), F.X, F.Y, F.Z, SpikeSeconds);
    return true;
}

bool RoundReferee::WorkDefuse(AShooterCharacter* Who, float Dt)
{
    if (View_.Phase != EPhase::Planted || !Alive(Who) || TeamOf(Who) != View_.Defenders) return false;
    const FVector F = FloorOf(Who);
    const float DX = F.X - View_.SpikeX, DY = F.Y - View_.SpikeY, DZ = F.Z - View_.SpikeZ;
    if (DX * DX + DY * DY > 130.f * 130.f || fabsf(DZ) > 120.f || Speed(Who) > 30.f) return false;
    // half defuse checkpoint
    if (!Continuing(Who)) View_.DefuseProgress = DefuseCheckpoint ? DefuseSeconds * 0.5f : 0.f;
    View_.DefuseProgress += Dt;
    if (View_.DefuseProgress >= DefuseSeconds * 0.5f && !DefuseCheckpoint) { DefuseCheckpoint = true; Announce("half defuse by %s", Who->GetName().c_str()); }
    if (View_.DefuseProgress < DefuseSeconds) return false;
    Announce("SPIKE DEFUSED by %s", Who->GetName().c_str());
    EndRound(false, "defuse");
    return true;
}

std::string RoundReferee::Command(const std::string& Line)
{
    // spawning reenters PE, keep Tick out
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered()) return "rounds: busy, retry";
    std::istringstream In(Line);
    std::string Word, Verb;
    In >> Word >> Verb;
    if (Verb == "start")
    {
        int Allies = 4, Enemies = 5; std::string Side = "defend";
        std::string Token;
        std::vector<int> Numbers;
        while (In >> Token)
        {
            if (Token == "attack" || Token == "defend") Side = Token;
            else if (isdigit((unsigned char)Token[0])) Numbers.push_back(std::clamp(atoi(Token.c_str()), 0, 9));
        }
        if (Numbers.size() >= 1) Allies = Numbers[0];
        if (Numbers.size() >= 2) Enemies = Numbers[1];
        if (Enemies < 1) return "rounds: need at least one enemy";
        if (!Setup()) return "rounds: this map has no bomb sites/team starts (bomb maps only)";
        Config = {Allies, Enemies, Side == "attack"};
        View_ = FView{};
        BeginRound();
        return "rounds: started - you " + Side + " with " + std::to_string(Allies) + " allies vs " + std::to_string(Enemies) +
               ". Plant/defuse: crouch and stand still (4s plant in a site, 7s defuse at the spike).";
    }
    if (Verb == "skirmish")
    {
        int Allies = 1, Enemies = 2; std::vector<int> Numbers; std::string Token; bool Attack = false;
        while (In >> Token)
        {
            if (isdigit((unsigned char)Token[0])) Numbers.push_back(std::clamp(atoi(Token.c_str()), 0, 9));
            else if (Token == "attack") Attack = true;
        }
        auto& MyStarts = Attack ? AttackStarts : DefendStarts;
        auto& TheirStarts = Attack ? DefendStarts : AttackStarts;
        if (Numbers.size() >= 1) Allies = Numbers[0];
        if (Numbers.size() >= 2) Enemies = Numbers[1];
        if (!Setup()) return "rounds: this map has no team starts (bomb maps only)";
        View_.Phase = EPhase::Off;
        MatchFlow::SetHoldRespawn(false);
        LocalBots::ClearTactical();
        LocalBots::ClearBodies();
        if (auto Me = Player()) Me->K2_TeleportTo(Snap(MyStarts[0]) + FVector{0, 0, 10}, Me->K2_GetActorRotation());
        int AgentIndex = 0;
        auto Place = [](const std::vector<FVector>& Starts, int I) { FVector At = Starts[I % Starts.size()]; At.X += 120.f * (I / (int)Starts.size()); return At; };
        for (int I = 0; I < Allies; ++I) Spawn(Agents[AgentIndex++ % 12], true, "vandal", Place(MyStarts, I + 1));
        for (int I = 0; I < Enemies; ++I) Spawn(Agents[AgentIndex++ % 12], false, "vandal", Place(TheirStarts, I));
        return "rounds: skirmish - you + " + std::to_string(Allies) + " vs " + std::to_string(Enemies) + " on Vandals, you at the " +
               (Attack ? "attackers' spawn, enemies at the defenders'" : "defenders' spawn, enemies at the attackers'") +
               ". No rounds; 'botorder clear' ends it.";
    }
    if (Verb == "stop")
    {
        View_.Phase = EPhase::Off;
        MatchFlow::SetHoldRespawn(false);
        const int Removed = LocalBots::ClearTactical();
        MoveSpikeActor(SpikeHome);
        return "rounds: stopped, removed " + std::to_string(Removed) + " bots";
    }
    if (Verb == "next" && Active()) { EndRound(false, "skipped"); View_.PhaseEnds = Clock(); return "rounds: skipping to the next round"; }
    std::ostringstream S;
    static const char* Names[] = {"off", "buy", "live", "planted", "ended"};
    S << "rounds: " << Names[(int)View_.Phase];
    if (Active())
    {
        S << ", round " << View_.Round << ", attack " << View_.AttackScore << " - defense " << View_.DefenseScore
          << ", " << (int)TimeLeft() << "s left, attackers alive " << AliveOn(View_.Attackers).size()
          << ", defenders alive " << AliveOn(View_.Defenders).size();
        if (View_.Carrier) S << ", spike carried by " << View_.Carrier->GetName();
        else if (View_.SpikeDown) S << ", spike " << (View_.Phase == EPhase::Planted ? "planted" : "dropped") << " at " << (int)View_.SpikeX << " " << (int)View_.SpikeY;
        if (View_.Phase == EPhase::Planted) S << ", defuse " << View_.DefuseProgress << "/" << DefuseSeconds << "s";
    }
    else S << " - 'rounds start [allies] [enemies] [attack|defend]'";
    return S.str();
}

void RoundReferee::Tick()
{
    if (View_.Phase == EPhase::Off || Ares::Config().Mode != Ares::EMode::Standalone) return;
    // runs every PE, the game clock is a UFunction call too
    {
        static auto LastRun = std::chrono::steady_clock::time_point{};
        const auto Wall = std::chrono::steady_clock::now();
        if (Wall - LastRun < std::chrono::milliseconds(50)) return;
        LastRun = Wall;
    }
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered() || LocalBots::IsUpdating()) return;
    static double Last = 0;
    const double Now = Clock();
    if (Now - Last < 0.05 && Now >= Last) return;
    const float Dt = (float)std::clamp(Now - Last, 0.0, 0.2);
    Last = Now;
    if (SetupWorld.Get() != UWorld::GetWorld()) { View_.Phase = EPhase::Off; Announce("world changed; rounds stopped"); return; }
    if (Ares::Seconds() >= NextPerf)
    {
        if (NextPerf > 0 && Ares::Profiling) Ares::Log("perf: referee character scans %.1f ms/s (%.0f scans/s)", ScanMs / 5, ScanCount / 5.0);
        ScanMs = 0; ScanCount = 0; NextPerf = Ares::Seconds() + 5;
    }

    auto Me = Player();
    if (Me && Alive(Me) && PlacedPawn.Get() != Me && View_.Phase == EPhase::Buy)
    {
        auto& Starts = Config.PlayerAttacks ? AttackStarts : DefendStarts;
        Me->K2_TeleportTo(Snap(Starts[0]) + FVector{0, 0, 10}, Me->K2_GetActorRotation());
        PlacedPawn = Me;
        auto World = UWorld::GetWorld();
        auto State = World && World->GameState ? World->GameState->Cast<AShooterGameState>() : nullptr;
        if (State) LocalTeams::Assign(Me, Config.PlayerAttacks ? View_.Attackers : View_.Defenders);
    }

    switch (View_.Phase)
    {
    case EPhase::Buy:
        if (Now >= View_.PhaseEnds)
        {
            SetPhase(EPhase::Live, RoundSeconds);
            MatchFlow::SetHoldRespawn(true);
            Announce("round %d LIVE - %.0fs", View_.Round, RoundSeconds);
        }
        break;
    case EPhase::Live:
    case EPhase::Planted:
    {
        const auto Attack = AliveOn(View_.Attackers), Defend = AliveOn(View_.Defenders);
        if (View_.Phase == EPhase::Live)
        {
            if (auto C = CarrierRef.Get(); CarrierRef.Get() && !Alive(C))
            {
                const FVector F = FloorOf(C);
                View_.SpikeDown = true; View_.SpikeX = F.X; View_.SpikeY = F.Y; View_.SpikeZ = F.Z;
                CarrierRef = nullptr; View_.Carrier = nullptr;
                MoveSpikeActor(F + FVector{0, 0, 5});
                Announce("spike dropped at %.0f %.0f", F.X, F.Y);
            }
            if (View_.SpikeDown)
                for (auto A : Attack)
                {
                    const FVector F = FloorOf(A);
                    const float DX = F.X - View_.SpikeX, DY = F.Y - View_.SpikeY;
                    if (DX * DX + DY * DY < 110.f * 110.f && fabsf(F.Z - View_.SpikeZ) < 120.f)
                    {
                        CarrierRef = A; View_.Carrier = A; View_.SpikeDown = false;
                        Announce("spike picked up by %s", A->GetName().c_str());
                        break;
                    }
                }
            if (Me && Me == CarrierRef.Get() && PlayerWorking(Me)) WorkPlant(Me, Dt);
            if (View_.Phase != EPhase::Live) break;
            if (Attack.empty()) { EndRound(false, "attackers eliminated"); break; }
            if (Defend.empty()) { EndRound(true, "defenders eliminated"); break; }
            if (Now >= View_.PhaseEnds) { EndRound(false, "time ran out"); break; }
        }
        else
        {
            if (Me && TeamOf(Me) == View_.Defenders && PlayerWorking(Me)) WorkDefuse(Me, Dt);
            if (View_.Phase != EPhase::Planted) break;
            if (Now >= View_.PhaseEnds) { Announce("SPIKE DETONATED"); EndRound(true, "detonation"); break; }
            if (Defend.empty()) { EndRound(true, "defenders eliminated after plant"); break; }
        }
        break;
    }
    case EPhase::Ended:
        if (Now >= View_.PhaseEnds)
        {
            if (View_.AttackScore >= 13 || View_.DefenseScore >= 13)
            {
                Announce("MATCH OVER: %s win %d-%d. Starting a new match.", View_.AttackScore > View_.DefenseScore ? "attackers" : "defenders",
                         std::max(View_.AttackScore, View_.DefenseScore), std::min(View_.AttackScore, View_.DefenseScore));
                View_.Round = 0; View_.AttackScore = View_.DefenseScore = 0;
            }
            BeginRound();
        }
        break;
    default: break;
    }
}

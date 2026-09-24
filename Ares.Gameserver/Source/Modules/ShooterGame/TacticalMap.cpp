#include <pch.h>
#include <Headers/Modules/ShooterGame/TacticalMap.h>
#include <SDK/NavigationSystem_classes.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace
{
    namespace fs = std::filesystem;

    enum class EStage { Idle, Nodes, Links, Jumps, Visibility };

    constexpr float Spacing = 200.f;
    constexpr float MergeXY = 130.f, MergeZ = 90.f;
    constexpr float LinkRange = 430.f;
    constexpr float SightRange = 5500.f;

    struct FBake
    {
        EStage Stage = EStage::Idle;
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AShooterCharacter> Pawn;
        FVector Min{}, Max{};
        int NX = 0, NY = 0, NZ = 0, Index = 0, A = 0, B = 0;
        float Radius = 42, HalfHeight = 98;
        ECollisionChannel Channel = ECollisionChannel::ECC_Pawn;
        TacticalGraph Graph;
        std::vector<TacticalGraph::Node> Pending;
        std::unordered_map<long long, std::vector<int>> Hash;
        long long Traces = 0, Visible = 0, PathQueries = 0, Sampled = 0, Disagree = 0, Rejected = 0;
        long long NoPath = 0, TooShort = 0, TooLong = 0, Steep = 0, Walled = 0, Bridges = 0, Jumps = 0, JumpTests = 0;
        float JumpZ = 0, Gravity = 0, RunSpeed = 540;
        double Started = 0;
    } Bake;

    TacticalGraph Loaded;
    TWeakObjectPtr<UWorld> LoadedWorld;
    bool Busy = false;

    fs::path LogsDir() { return fs::path(Ares::Config().LogPath).parent_path(); }

    long long Cell(float X, float Y)
    {
        return ((long long)(int)std::floor(X / Spacing) << 32) ^ (unsigned)(int)std::floor(Y / Spacing);
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

    // overlap not sweep, sweeps miss capsules starting inside stuff
    bool ClearPose(UWorld* World, const FVector& Floor, const TArray<AActor*>& Ignore)
    {
        TArray<EObjectTypeQuery> Types{};
        Types.Add(EObjectTypeQuery::ObjectTypeQuery1); // WorldStatic
        Types.Add(EObjectTypeQuery::ObjectTypeQuery2); // WorldDynamic
        TArray<UPrimitiveComponent*> Components{};
        UKismetSystemLibrary::CapsuleOverlapComponents(World, Floor + FVector{0, 0, Bake.HalfHeight + 2},
            Bake.Radius, Bake.HalfHeight, Types, nullptr, Ignore, &Components);
        bool Clear = true;
        for (auto Component : Components)
            if (Component && Component->K2_IsQueryCollisionEnabled() &&
                Component->GetCollisionResponseToChannel(Bake.Channel) == ECollisionResponse::ECR_Block) { Clear = false; break; }
        Types.Free(); Components.Free();
        return Clear;
    }

    FVector Eye(const TacticalGraph::Node& N) { return FVector{N.X, N.Y, N.Z + Bake.Graph.Eye}; }

    bool TraceClear(UWorld* World, const FVector& From, const FVector& To, const TArray<AActor*>& Ignore, bool ByChannel)
    {
        FHitResult Hit{};
        if (ByChannel)
            return !UKismetSystemLibrary::LineTraceSingle(World, From, To, ETraceTypeQuery::TraceTypeQuery1, false, Ignore,
                EDrawDebugTrace::None, &Hit, true, FLinearColor{}, FLinearColor{}, 0);
        return !UKismetSystemLibrary::LineTraceSingleByProfile(World, From, To, FName(L"BlockAll"), false, Ignore,
            EDrawDebugTrace::None, &Hit, true, FLinearColor{}, FLinearColor{}, 0);
    }

    // recast agent climbs these, the player capsule cant
    bool SteepClimb(UNavigationPath* Path)
    {
        if (!Path) return false;
        for (int K = 0; K + 1 < Path->PathPoints.Num(); ++K)
        {
            const FVector A = Path->PathPoints[K], B = Path->PathPoints[K + 1];
            const float Rise = B.Z - A.Z, Run = sqrtf((B.X - A.X) * (B.X - A.X) + (B.Y - A.Y) * (B.Y - A.Y));
            if (Rise > 90.f && Rise > 0.85f * Run) return true;
        }
        return false;
    }

    // navmesh drops clip low parapets, keep the lift under step height
    std::string DropBlocker;

    bool BlockedDrop(UWorld* World, UNavigationPath* Path, const TArray<AActor*>& Ignore)
    {
        if (!Path) return false;
        for (int K = 0; K + 1 < Path->PathPoints.Num(); ++K)
        {
            const FVector A = Path->PathPoints[K], B = Path->PathPoints[K + 1];
            const float Fall = A.Z - B.Z, Run = sqrtf((B.X - A.X) * (B.X - A.X) + (B.Y - A.Y) * (B.Y - A.Y));
            // lower than climb check or mid ramp railing slips through
            if (!(Fall > 90.f && Fall > 0.7f * Run)) continue;
            const float Lift = Bake.HalfHeight + 20.f;
            FHitResult Hit{};
            ++Bake.Traces;
            // start penetrating is just navmesh sitting off the floor
            if (UKismetSystemLibrary::CapsuleTraceSingleByProfile(World, FVector{A.X, A.Y, A.Z + Lift}, FVector{B.X, B.Y, A.Z + Lift},
                    Bake.Radius, Bake.HalfHeight, FName(L"BlockAll"), false, Ignore, EDrawDebugTrace::None, &Hit, true,
                    FLinearColor{}, FLinearColor{}, 0) && !Hit.bStartPenetrating)
            {
                auto Actor = Hit.Actor.Get();
                DropBlocker = Actor ? Actor->GetName() : "?";
                return true;
            }
        }
        return false;
    }

    // navmesh has no jump links at all
    bool JumpArc(UWorld* World, const TacticalGraph::Node& A, const TacticalGraph::Node& B, const TArray<AActor*>& Ignore)
    {
        const float Vz = Bake.JumpZ, G = Bake.Gravity;
        if (Vz <= 0 || G <= 0) return false;
        const float Rise = B.Z - A.Z, Apex = Vz * Vz / (2 * G);
        if (Rise > Apex - 12.f) return false;
        const float T = (Vz + sqrtf(std::max(0.f, Vz * Vz - 2 * G * Rise))) / G;
        const float DX = B.X - A.X, DY = B.Y - A.Y, Run = sqrtf(DX * DX + DY * DY);
        if (Run < 60.f || Run / T > Bake.RunSpeed * 0.9f) return false;
        ++Bake.JumpTests;
        const float Lift = Bake.HalfHeight + 4.f;
        FVector Prev{A.X, A.Y, A.Z + Lift};
        constexpr int Steps = 10;
        for (int I = 1; I <= Steps; ++I)
        {
            const float F = (float)I / Steps, Time = T * F;
            FVector P{A.X + DX * F, A.Y + DY * F, I == Steps ? B.Z + Lift : A.Z + Lift + Vz * Time - 0.5f * G * Time * Time};
            FHitResult Hit{};
            ++Bake.Traces;
            if (UKismetSystemLibrary::CapsuleTraceSingleByProfile(World, Prev, P, Bake.Radius - 4.f, Bake.HalfHeight - 4.f,
                    FName(L"BlockAll"), false, Ignore, EDrawDebugTrace::None, &Hit, true, FLinearColor{}, FLinearColor{}, 0))
                return false;
            Prev = P;
        }
        return true;
    }

    // islands behind long ramps/doors get cut off, hook them back to main
    void Bridge(UWorld* World, AShooterCharacter* Pawn)
    {
        auto& G = Bake.Graph;
        TArray<AActor*> Ignore{};
        Ignore.Add(Pawn);
        for (int Round = 0; Round < 24; ++Round)
        {
            std::vector<int> Sizes;
            auto Comp = G.Components(Sizes);
            const int Main = (int)(std::max_element(Sizes.begin(), Sizes.end()) - Sizes.begin());
            bool Merged = false;
            for (int C = 0; C < (int)Sizes.size() && !Merged; ++C)
            {
                if (C == Main || Sizes[C] < 3) continue;
                std::vector<std::pair<float, std::pair<int, int>>> Pairs;
                for (int A = 0; A < G.Count(); ++A)
                {
                    if (Comp[A] != C) continue;
                    for (int B = 0; B < G.Count(); ++B)
                        if (Comp[B] == Main) { const float D = G.Dist(A, B); if (D < 1500) Pairs.push_back({D, {A, B}}); }
                }
                std::sort(Pairs.begin(), Pairs.end());
                // more than one bridge, market door closes and kills the only one
                int Tries = 0;
                std::vector<TacticalGraph::Node> Made;
                for (auto& [D, AB] : Pairs)
                {
                    if (++Tries > 60 || Made.size() >= 3) break;
                    auto [A, B] = AB;
                    const auto& NA = G.Nodes[A]; const auto& NB = G.Nodes[B];
                    bool Crowded = false;
                    for (auto& M : Made) if (TacticalGraph::Dist(M, NA) < 1000.f) Crowded = true;
                    if (Crowded) continue;
                    auto Forward = UNavigationSystemV1::FindPathToLocationSynchronously(World, FVector{NA.X, NA.Y, NA.Z + 20}, FVector{NB.X, NB.Y, NB.Z + 20}, Pawn, nullptr);
                    auto Back = UNavigationSystemV1::FindPathToLocationSynchronously(World, FVector{NB.X, NB.Y, NB.Z + 20}, FVector{NA.X, NA.Y, NA.Z + 20}, Pawn, nullptr);
                    Bake.PathQueries += 2;
                    const bool F = Forward && Forward->IsValid() && !Forward->IsPartial() && !SteepClimb(Forward) && !BlockedDrop(World, Forward, Ignore);
                    const bool R = Back && Back->IsValid() && !Back->IsPartial() && !SteepClimb(Back) && !BlockedDrop(World, Back, Ignore);
                    if (!F && !R) continue;
                    if (F) G.Links[A].push_back({B, std::max(D, Forward->GetPathLength())});
                    if (R) G.Links[B].push_back({A, std::max(D, Back->GetPathLength())});
                    ++Bake.Bridges;
                    Made.push_back(NA);
                    Ares::Log("tactical: bridged a %d-node component at %.0f %.0f %.0f (%s, %.0f apart)", Sizes[C], NA.X, NA.Y, NA.Z,
                              F && R ? "both ways" : F ? "one way out" : "one way in", D);
                    Merged = true;
                }
                if (!Merged) Ares::Log("tactical: could not bridge a %d-node component at %.0f %.0f %.0f",
                                       Sizes[C], G.Nodes[Pairs.empty() ? 0 : Pairs[0].second.first].X,
                                       G.Nodes[Pairs.empty() ? 0 : Pairs[0].second.first].Y, G.Nodes[Pairs.empty() ? 0 : Pairs[0].second.first].Z);
            }
            if (!Merged) break;
        }
        Ignore.Free();
        G.Index();
    }

    void Abort(const char* Why)
    {
        Ares::Log("tactical: bake aborted: %s", Why);
        Bake.Stage = EStage::Idle;
        Bake.Graph = TacticalGraph{};
        Bake.Pending.clear();
        Bake.Hash.clear();
    }

    void Finish()
    {
        const auto File = LogsDir() / "maps" / ("tactical-" + Bake.Graph.Level + ".txt");
        fs::create_directories(File.parent_path());
        std::ofstream Out(File);
        Out << std::fixed << std::setprecision(1);
        Bake.Graph.Write(Out);
        Out.close();
        size_t Links = 0;
        for (auto& L : Bake.Graph.Links) Links += L.size();
        Ares::Log("tactical: bake %s nodes=%d links=%zu visible-pairs=%lld traces=%lld path-queries=%lld "
                  "clearance-rejected=%lld channel-vs-blockall disagreements=%lld/%lld sampled, %.0fs -> %s",
                  Out ? "complete" : "WRITE FAILED", Bake.Graph.Count(), Links, Bake.Visible, Bake.Traces,
                  Bake.PathQueries, Bake.Rejected, Bake.Disagree, Bake.Sampled, Ares::Seconds() - Bake.Started,
                  File.string().c_str());
        Bake.Stage = EStage::Idle;
        LoadedWorld = nullptr; // force reload
    }

    void Advance()
    {
        auto World = Bake.World.Get();
        auto Pawn = Bake.Pawn.Get();
        if (!World || World != UWorld::GetWorld() || !Pawn || Pawn->bActorIsBeingDestroyed) { Abort("world or pawn changed"); return; }
        TArray<AActor*> Ignore{};
        Ignore.Add(Pawn);
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(6);
        auto& G = Bake.Graph;
        while (Bake.Stage != EStage::Idle && std::chrono::steady_clock::now() < Deadline)
        {
            if (Bake.Stage == EStage::Nodes)
            {
                if (Bake.Index >= Bake.NX * Bake.NY * Bake.NZ)
                {
                    const int N = (int)Bake.Pending.size();
                    const float EyeHeight = G.Eye;
                    const std::string Level = G.Level;
                    G.Reset(N);
                    G.Eye = EyeHeight; G.Level = Level;
                    for (int I = 0; I < N; ++I) G.Nodes[I] = Bake.Pending[I];
                    G.Index(true);
                    Bake.Stage = EStage::Links; Bake.A = 0;
                    Ares::Log("tactical: %d standing nodes, linking", N);
                    continue;
                }
                const int I = Bake.Index++;
                const int X = I % Bake.NX, Y = (I / Bake.NX) % Bake.NY, Z = I / (Bake.NX * Bake.NY);
                FVector Query{Bake.Min.X + X * Spacing, Bake.Min.Y + Y * Spacing, Bake.Min.Z + Z * 100.f}, Floor{};
                if (!UNavigationSystemV1::K2_ProjectPointToNavigation(World, Query, &Floor, nullptr, nullptr, FVector{90, 90, 55})) continue;
                FHitResult Ground{};
                if (!UKismetSystemLibrary::LineTraceSingleByProfile(World, Floor + FVector{0, 0, 40}, Floor - FVector{0, 0, 100},
                    FName(L"BlockAll"), false, Ignore, EDrawDebugTrace::None, &Ground, true, FLinearColor{}, FLinearColor{}, 0)) continue;
                if (Ground.ImpactNormal.Z < Pawn->CharacterMovement->K2_GetWalkableFloorZ()) continue;
                Floor = Ground.ImpactPoint;
                bool Duplicate = false;
                for (int DY = -1; DY <= 1 && !Duplicate; ++DY) for (int DX = -1; DX <= 1 && !Duplicate; ++DX)
                {
                    auto Found = Bake.Hash.find(Cell(Floor.X + DX * Spacing, Floor.Y + DY * Spacing));
                    if (Found == Bake.Hash.end()) continue;
                    for (int J : Found->second)
                    {
                        const auto& P = Bake.Pending[J];
                        if (fabsf(P.Z - Floor.Z) < MergeZ && (P.X - Floor.X) * (P.X - Floor.X) + (P.Y - Floor.Y) * (P.Y - Floor.Y) < MergeXY * MergeXY)
                        { Duplicate = true; break; }
                    }
                }
                if (Duplicate) continue;
                if (!ClearPose(World, Floor, Ignore)) { ++Bake.Rejected; continue; }
                Bake.Hash[Cell(Floor.X, Floor.Y)].push_back((int)Bake.Pending.size());
                Bake.Pending.push_back({Floor.X, Floor.Y, Floor.Z});
            }
            else if (Bake.Stage == EStage::Links)
            {
                if (Bake.A >= G.Count())
                {
                    Ares::Log("tactical: links done after %lld path queries (rejected: %lld no path, %lld partial, %lld detour, %lld steep climb, %lld walled drop)",
                              Bake.PathQueries, Bake.NoPath, Bake.TooShort, Bake.TooLong, Bake.Steep, Bake.Walled);
                    Bridge(World, Pawn);
                    Bake.Stage = EStage::Jumps; Bake.A = 0;
                    Ares::Log("tactical: jump physics: jump speed %.0f, gravity %.0f (apex %.0f), run %.0f",
                              Bake.JumpZ, Bake.Gravity, Bake.Gravity > 0 ? Bake.JumpZ * Bake.JumpZ / (2 * Bake.Gravity) : 0.f, Bake.RunSpeed);
                    continue;
                }
                const int A = Bake.A++;
                std::vector<int> Near;
                const auto& NA = G.Nodes[A];
                G.Within(NA.X, NA.Y, NA.Z, LinkRange, Near);
                for (int B : Near)
                {
                    if (B == A) continue;
                    const auto& NB = G.Nodes[B];
                    if (fabsf(NB.Z - NA.Z) > 250) continue;
                    ++Bake.PathQueries;
                    // GetPathLength lies about partial paths
                    auto Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, FVector{NA.X, NA.Y, NA.Z + 20},
                        FVector{NB.X, NB.Y, NB.Z + 20}, Pawn, nullptr);
                    if (!Path || !Path->IsValid()) { ++Bake.NoPath; continue; }
                    if (Path->IsPartial()) { ++Bake.TooShort; continue; }
                    if (SteepClimb(Path)) { ++Bake.Steep; continue; }
                    if (BlockedDrop(World, Path, Ignore))
                    {
                        if (Bake.Walled++ < 400) Ares::Log("tactical: no link %.0f %.0f %.0f -> %.0f %.0f %.0f: the drop is walled off by %s", NA.X, NA.Y, NA.Z, NB.X, NB.Y, NB.Z, DropBlocker.c_str());
                        continue;
                    }
                    const float Straight = G.Dist(A, B);
                    const float Length = std::max(Straight, Path->GetPathLength());
                    // allow extra for stairs/ramps
                    if (Length > Straight * 1.35f + 60.f + 2.5f * fabsf(NB.Z - NA.Z)) { ++Bake.TooLong; continue; }
                    G.Links[A].push_back({B, Length});
                }
            }
            else if (Bake.Stage == EStage::Jumps)
            {
                if (Bake.A >= G.Count())
                {
                    Ares::Log("tactical: %lld jump links from %lld arc tests", Bake.Jumps, Bake.JumpTests);
                    Bake.Stage = EStage::Visibility; Bake.A = 0; Bake.B = 1;
                    continue;
                }
                const int A = Bake.A++;
                const auto& NA = G.Nodes[A];
                std::vector<float> Walk;
                G.Distances({A}, Walk);
                std::vector<int> Near;
                G.Within(NA.X, NA.Y, NA.Z, 560.f, Near);
                std::vector<std::pair<float, int>> Found;   // saved, node
                for (int B : Near)
                {
                    if (B == A) continue;
                    const auto& NB = G.Nodes[B];
                    if (NB.Z - NA.Z < -450.f) continue;
                    const float Straight = G.Dist(A, B);
                    if (std::isfinite(Walk[B]) && Walk[B] < Straight * 1.8f + 300.f) continue;
                    if (!JumpArc(World, NA, NB, Ignore)) continue;
                    Found.push_back({std::isfinite(Walk[B]) ? Walk[B] - Straight : 1e9f, B});
                }
                std::sort(Found.begin(), Found.end(), [](auto& X, auto& Y) { return X.first > Y.first; });
                if (Found.size() > 4) Found.resize(4);
                for (auto& [Saved, B] : Found)
                {
                    const auto& NB = G.Nodes[B];
                    G.Links[A].push_back({B, G.Dist(A, B) + 150.f, true});
                    if (Bake.Jumps++ < 60)
                        Ares::Log("tactical: jump %.0f %.0f %.0f -> %.0f %.0f %.0f (rise %.0f, saves %s)", NA.X, NA.Y, NA.Z, NB.X, NB.Y, NB.Z,
                                  NB.Z - NA.Z, Saved > 1e8f ? "an unreachable spot" : (std::to_string((int)Saved) + " units").c_str());
                }
            }
            else
            {
                if (Bake.A >= G.Count() - 1) { Finish(); break; }
                const int A = Bake.A, B = Bake.B;
                if (++Bake.B >= G.Count()) { ++Bake.A; Bake.B = Bake.A + 1; }
                if (G.Dist(A, B) > SightRange) continue;
                const FVector From = Eye(G.Nodes[A]), To = Eye(G.Nodes[B]);
                ++Bake.Traces;
                const bool Clear = TraceClear(World, From, To, Ignore, false);
                if (Clear) { G.SetSees(A, B); ++Bake.Visible; }
                // not sure blockall vs visibility channel, sample both
                if ((Bake.Traces & 15) == 0)
                {
                    ++Bake.Sampled;
                    if (TraceClear(World, From, To, Ignore, true) != Clear) ++Bake.Disagree;
                }
            }
        }
        Ignore.Free();
    }

    bool LoadFrom(const fs::path& File, UWorld* World, const std::string& Level)
    {
        std::ifstream In(File);
        if (!In) return false;
        TacticalGraph Graph;
        std::string Error;
        if (!Graph.Read(In, Error)) { Ares::Log("tactical: %s rejected: %s", File.string().c_str(), Error.c_str()); return false; }
        if (Graph.Level != Level) { Ares::Log("tactical: %s is for %s, not %s", File.string().c_str(), Graph.Level.c_str(), Level.c_str()); return false; }
        Loaded = std::move(Graph);
        Ares::Log("tactical: loaded %s: %d nodes", File.string().c_str(), Loaded.Count());
        return true;
    }
}

const TacticalGraph* TacticalMap::Get()
{
    auto World = UWorld::GetWorld();
    if (!World) return nullptr;
    if (LoadedWorld.Get() != World)
    {
        LoadedWorld = World;
        Loaded = TacticalGraph{};
        const std::string Level = UGameplayStatics::GetCurrentLevelName(World, true).ToString();
        std::string Lower = Level;
        std::transform(Lower.begin(), Lower.end(), Lower.begin(), [](unsigned char C) { return (char)tolower(C); });
        if (!LoadFrom(LogsDir().parent_path() / "assets" / "maps" / Lower / "tactical.txt", World, Level) &&
            !LoadFrom(LogsDir() / "maps" / ("tactical-" + Level + ".txt"), World, Level))
            Ares::Log("tactical: no bake for %s; bots fall back to navmesh-only behaviour (run 'tacbake')", Level.c_str());
    }
    return Loaded.Count() ? &Loaded : nullptr;
}

std::string TacticalMap::Explain(const std::string& Arguments)
{
    float V[6];
    if (sscanf(Arguments.c_str(), "%f %f %f %f %f %f", &V[0], &V[1], &V[2], &V[3], &V[4], &V[5]) != 6) return "tacpath: x y z x y z";
    auto World = UWorld::GetWorld();
    auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    auto Pawn = PC && PC->Pawn ? PC->Pawn->Cast<AShooterCharacter>() : nullptr;
    if (!Pawn || !Pawn->CapsuleComponent) return "tacpath: no player";
    Bake.Radius = Pawn->CapsuleComponent->GetScaledCapsuleRadius();
    Bake.HalfHeight = Pawn->CapsuleComponent->GetScaledCapsuleHalfHeight();
    auto Path = UNavigationSystemV1::FindPathToLocationSynchronously(World, FVector{V[0], V[1], V[2] + 20}, FVector{V[3], V[4], V[5] + 20}, Pawn, nullptr);
    if (!Path || !Path->IsValid()) return "tacpath: no path";
    TArray<AActor*> Ignore{};
    Ignore.Add(Pawn);
    std::ostringstream Out;
    Out << std::fixed << std::setprecision(0) << "tacpath: length " << Path->GetPathLength() << (Path->IsPartial() ? " partial" : "")
        << " steep-climb " << SteepClimb(Path) << " walled-drop " << BlockedDrop(World, Path, Ignore) << '\n';
    for (int K = 0; K < Path->PathPoints.Num(); ++K)
    {
        const FVector P = Path->PathPoints[K];
        Out << "  " << P.X << " " << P.Y << " " << P.Z;
        if (K + 1 < Path->PathPoints.Num())
        {
            const FVector B = Path->PathPoints[K + 1];
            FHitResult Hit{};
            const bool Blocked = UKismetSystemLibrary::CapsuleTraceSingleByProfile(World, P + FVector{0, 0, Bake.HalfHeight + 45},
                B + FVector{0, 0, Bake.HalfHeight + 45}, Bake.Radius, Bake.HalfHeight, FName(L"BlockAll"), false, Ignore,
                EDrawDebugTrace::None, &Hit, true, FLinearColor{}, FLinearColor{}, 0);
            Out << "  -> rise " << (B.Z - P.Z) << " over " << sqrtf((B.X - P.X) * (B.X - P.X) + (B.Y - P.Y) * (B.Y - P.Y));
            if (Blocked)
            {
                auto Actor = Hit.Actor.Get(); auto Comp = Hit.Component.Get();
                Out << "  sweep hits " << (Actor ? Actor->GetName() : "?") << "/" << (Comp ? Comp->GetName() : "?") << " at "
                    << Hit.ImpactPoint.X << " " << Hit.ImpactPoint.Y << " " << Hit.ImpactPoint.Z << (Hit.bStartPenetrating ? " (start penetrating)" : "");
            }
        }
        Out << '\n';
    }
    Ignore.Free();
    return Out.str();
}

std::string TacticalMap::StartBake()
{
    if (Bake.Stage != EStage::Idle) return "tacbake: already running (" + Status() + ")";
    auto World = UWorld::GetWorld();
    auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    auto Pawn = PC && PC->Pawn ? PC->Pawn->Cast<AShooterCharacter>() : nullptr;
    if (!Pawn || !Pawn->CapsuleComponent || !Pawn->CharacterMovement) return "tacbake: no player";
    auto Nav = UNavigationSystemV1::GetNavigationSystem(World);
    if (!Nav) return "tacbake: no navigation system";
    ARecastNavMesh* Mesh = nullptr;
    for (auto Data : Nav->NavDataSet) if (Data && InWorld(Data, World)) if (auto R = Data->Cast<ARecastNavMesh>()) { Mesh = R; break; }
    if (!Mesh) return "tacbake: no registered Recast mesh";
    FVector Center{}, Extent{};
    Mesh->GetActorBounds(false, &Center, &Extent);
    if (Extent.X <= 0 || Extent.Y <= 0 || Extent.Z <= 0) return "tacbake: empty navmesh bounds";
    Bake = FBake{};
    Bake.World = World; Bake.Pawn = Pawn;
    Bake.Min = Center - Extent; Bake.Max = Center + Extent;
    Bake.NX = (int)std::ceil(Extent.X * 2 / Spacing) + 1;
    Bake.NY = (int)std::ceil(Extent.Y * 2 / Spacing) + 1;
    Bake.NZ = (int)std::ceil(Extent.Z * 2 / 100) + 1;
    Bake.Radius = Pawn->CapsuleComponent->GetScaledCapsuleRadius();
    Bake.HalfHeight = Pawn->CapsuleComponent->GetScaledCapsuleHalfHeight();
    Bake.Channel = Pawn->CapsuleComponent->GetCollisionObjectType();
    Bake.JumpZ = Pawn->CharacterMovement->JumpZVelocity;
    Bake.Gravity = -Pawn->CharacterMovement->GetGravityZ();
    // MaxWalkSpeed is 590 but rifle run is slower
    Bake.RunSpeed = std::min(540.f, Pawn->CharacterMovement->MaxWalkSpeed);
    FVector EyePos{}; FRotator EyeRot{};
    Pawn->GetActorEyesViewPoint(&EyePos, &EyeRot);
    Bake.Graph.Eye = EyePos.Z - (Pawn->K2_GetActorLocation().Z - Bake.HalfHeight);
    Bake.Graph.Level = UGameplayStatics::GetCurrentLevelName(World, true).ToString();
    Bake.Started = Ares::Seconds();
    // debug, log why spawns get rejected
    {
        TArray<AActor*> Ignore{}; Ignore.Add(Pawn);
        for (auto Start : GetObjectsOfClass<AShooterTeamStart>())
        {
            if (!Start || Start->IsDefaultObject() || !InWorld(Start, World)) continue;
            const FVector At = Start->K2_GetActorLocation();
            FVector Floor{};
            const bool Projects = UNavigationSystemV1::K2_ProjectPointToNavigation(World, At, &Floor, nullptr, nullptr, FVector{90, 90, 200});
            FHitResult Ground{};
            const bool HasGround = Projects && UKismetSystemLibrary::LineTraceSingleByProfile(World, Floor + FVector{0, 0, 40}, Floor - FVector{0, 0, 100},
                FName(L"BlockAll"), false, Ignore, EDrawDebugTrace::None, &Ground, true, FLinearColor{}, FLinearColor{}, 0);
            std::string Blocker = "none";
            bool Clear = false;
            if (HasGround)
            {
                Clear = ClearPose(World, Ground.ImpactPoint, Ignore);
                if (!Clear)
                {
                    TArray<EObjectTypeQuery> Types{}; Types.Add(EObjectTypeQuery::ObjectTypeQuery1); Types.Add(EObjectTypeQuery::ObjectTypeQuery2);
                    TArray<UPrimitiveComponent*> Hits{};
                    UKismetSystemLibrary::CapsuleOverlapComponents(World, Ground.ImpactPoint + FVector{0, 0, Bake.HalfHeight + 2}, Bake.Radius, Bake.HalfHeight, Types, nullptr, Ignore, &Hits);
                    for (auto C : Hits)
                        if (C && C->K2_IsQueryCollisionEnabled() && C->GetCollisionResponseToChannel(Bake.Channel) == ECollisionResponse::ECR_Block)
                        { Blocker = (C->GetOwner() ? C->GetOwner()->GetName() : std::string("?")) + "/" + C->GetName(); break; }
                    Types.Free(); Hits.Free();
                }
            }
            Ares::Log("tactical: start %s at %.0f %.0f %.0f: projects=%d ground=%d normalZ=%.2f clear=%d blocker=%s",
                      Start->GetName().c_str(), At.X, At.Y, At.Z, Projects, HasGround, HasGround ? Ground.ImpactNormal.Z : 0.f, Clear, Blocker.c_str());
        }
        Ignore.Free();
    }
    Bake.Stage = EStage::Nodes;
    std::ostringstream Reply;
    Reply << "tacbake: started on " << Bake.Graph.Level << ", lattice " << Bake.NX << "x" << Bake.NY << "x" << Bake.NZ
          << ", eye height " << Bake.Graph.Eye << ", capsule " << Bake.Radius << "/" << Bake.HalfHeight;
    return Reply.str();
}

std::string TacticalMap::Status()
{
    std::ostringstream S;
    switch (Bake.Stage)
    {
    case EStage::Idle: S << "idle"; break;
    case EStage::Nodes: S << "sampling " << Bake.Index << "/" << Bake.NX * Bake.NY * Bake.NZ << ", " << Bake.Pending.size() << " nodes"; break;
    case EStage::Links: S << "linking " << Bake.A << "/" << Bake.Graph.Count(); break;
    case EStage::Jumps: S << "jump links " << Bake.A << "/" << Bake.Graph.Count() << ", " << Bake.Jumps << " found"; break;
    case EStage::Visibility: S << "visibility row " << Bake.A << "/" << Bake.Graph.Count() << ", " << Bake.Traces << " traces"; break;
    }
    if (auto G = Loaded.Count() ? &Loaded : nullptr) S << "; loaded " << G->Level << " with " << G->Count() << " nodes";
    return S.str();
}

void TacticalMap::Tick()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone || Bake.Stage == EStage::Idle) return;
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered()) return;
    static double NextSlice = 0;
    if (Ares::Seconds() < NextSlice) return;
    NextSlice = Ares::Seconds() + 0.016;
    Advance();
    static double NextReport = 0;
    if (Bake.Stage != EStage::Idle && Ares::Seconds() >= NextReport)
    {
        NextReport = Ares::Seconds() + 10;
        Ares::Log("tactical: %s", Status().c_str());
    }
}

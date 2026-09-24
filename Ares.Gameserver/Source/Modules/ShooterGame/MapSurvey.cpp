#include <pch.h>
#include <Headers/Modules/ShooterGame/MapSurvey.h>
#include <Headers/Modules/ShooterGame/TacticalMap.h>
#include <SDK/NavigationSystem_classes.hpp>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <chrono>

namespace
{
    bool Busy = false;
    struct Point { FVector Floor; int X, Y; };
    struct Survey
    {
        bool Active = false, Linking = false;
        TWeakObjectPtr<UWorld> World;
        TWeakObjectPtr<AShooterCharacter> Pawn;
        FVector Min{}, Max{};
        int NX = 0, NY = 0, NZ = 0, Index = 0, EdgeIndex = 0, Edges = 0, Rejected = 0;
        float Radius = 42, HalfHeight = 98;
        FName Profile;
        ECollisionChannel Channel = ECollisionChannel::ECC_Pawn;
        std::vector<Point> Points;
        std::unordered_set<std::string> Seen;
        std::unordered_map<int, std::vector<int>> Cells;
        std::ofstream Out;
    } Scan;
    constexpr float Spacing = 100.f;
    bool InWorld(UObject* Object, UWorld* World)
    {
        for (int Depth = 0; Object && Depth < 32; ++Depth, Object = Object->Outer)
        {
            if (Object == World) return true;
            if (auto Level = Object->Cast<ULevel>()) return Level->OwningWorld == World;
        }
        return false;
    }

    void Vector(std::ostream& Out, const FVector& V)
    {
        Out << '[' << V.X << ',' << V.Y << ',' << V.Z << ']';
    }

    bool ClearCapsule(UWorld* World, const FVector& A, const FVector& B, const TArray<AActor*>& Ignore)
    {
        const FVector Lift{0,0,Scan.HalfHeight + 2.f};
        FHitResult Hit{};
        return !UKismetSystemLibrary::CapsuleTraceSingleByProfile(World, A + Lift, B + Lift,
            Scan.Radius, Scan.HalfHeight, Scan.Profile, false, Ignore, EDrawDebugTrace::None,
            &Hit, true, FLinearColor{}, FLinearColor{}, 0);
    }

    bool ClearPose(UWorld* World, const FVector& Floor, const TArray<AActor*>& Ignore)
    {
        TArray<EObjectTypeQuery> Types{};
        Types.Add(EObjectTypeQuery::ObjectTypeQuery1); // WorldStatic
        Types.Add(EObjectTypeQuery::ObjectTypeQuery2); // WorldDynamic
        TArray<UPrimitiveComponent*> Components{};
        UKismetSystemLibrary::CapsuleOverlapComponents(World,Floor+FVector{0,0,Scan.HalfHeight+2},
            Scan.Radius,Scan.HalfHeight,Types,nullptr,Ignore,&Components);
        bool Clear=true;
        for (auto Component:Components)
            if (Component && Component->K2_IsQueryCollisionEnabled() &&
                Component->GetCollisionResponseToChannel(Scan.Channel)==ECollisionResponse::ECR_Block) { Clear=false; break; }
        Types.Free(); Components.Free();
        return Clear;
    }

    void AdvanceScan()
    {
        auto World = Scan.World.Get();
        auto Pawn = Scan.Pawn.Get();
        if (!World || World != UWorld::GetWorld() || !Pawn || Pawn->bActorIsBeingDestroyed)
        {
            Scan.Out << "{\"type\":\"aborted\",\"reason\":\"world or pawn changed\"}\n";
            Scan.Out.close(); Scan.Active = false; return;
        }
        TArray<AActor*> Ignore{};
        Ignore.Add(Pawn);
        const auto Deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(2);
        int Work = 0;
        while (Scan.Active && Work++ < 64 && std::chrono::steady_clock::now() < Deadline)
        {
            if (!Scan.Linking)
            {
                if (Scan.Index >= Scan.NX * Scan.NY * Scan.NZ)
                {
                    Scan.Linking = true;
                    Ares::Log("mapscan: sampled %zu clearance points, linking", Scan.Points.size());
                    continue;
                }
                int I = Scan.Index++, X = I % Scan.NX, Y = (I / Scan.NX) % Scan.NY, Z = I / (Scan.NX * Scan.NY);
                FVector Query{Scan.Min.X + X*Spacing, Scan.Min.Y + Y*Spacing, Scan.Min.Z + Z*100.f}, Floor{};
                if (!UNavigationSystemV1::K2_ProjectPointToNavigation(World, Query, &Floor, nullptr, nullptr, FVector{35,35,60})) continue;
                // navmesh sits a bit above the real floor
                FHitResult Ground{};
                if (!UKismetSystemLibrary::LineTraceSingleByProfile(World, Floor + FVector{0,0,40}, Floor - FVector{0,0,100},
                    Scan.Profile, false, Ignore, EDrawDebugTrace::None, &Ground, true, FLinearColor{}, FLinearColor{}, 0)) continue;
                if (Ground.ImpactNormal.Z < Pawn->CharacterMovement->K2_GetWalkableFloorZ()) continue;
                Floor = Ground.ImpactPoint;
                std::string Key = std::to_string((int)std::lround(Floor.X/10)) + ":" + std::to_string((int)std::lround(Floor.Y/10)) + ":" + std::to_string((int)std::lround(Floor.Z/10));
                if (!Scan.Seen.insert(Key).second) continue;
                if (!ClearPose(World,Floor,Ignore) || !ClearCapsule(World, Floor, Floor + FVector{0,0,0.5f}, Ignore)) { ++Scan.Rejected; continue; }
                int ID = (int)Scan.Points.size();
                Scan.Points.push_back({Floor,X,Y});
                Scan.Cells[Y*Scan.NX+X].push_back(ID);
                Scan.Out << "{\"type\":\"point\",\"id\":" << ID << ",\"floor\":"; Vector(Scan.Out,Floor); Scan.Out << "}\n";
            }
            else
            {
                if (Scan.EdgeIndex >= (int)Scan.Points.size())
                {
                    Scan.Out << "{\"type\":\"complete\",\"points\":" << Scan.Points.size() << ",\"edges\":" << Scan.Edges
                        << ",\"clearance_rejected\":" << Scan.Rejected << "}\n";
                    Scan.Out.close(); Scan.Active = false;
                    Ares::Log("mapscan: %s points=%zu edges=%d clearance-rejected=%d", Scan.Out ? "complete" : "WRITE FAILED", Scan.Points.size(),Scan.Edges,Scan.Rejected);
                    break;
                }
                int AID = Scan.EdgeIndex++;
                const auto& A = Scan.Points[AID];
                // walk links only, no jumps/drops
                for (int DY=-1; DY<=1; ++DY) for (int DX=-1; DX<=1; ++DX)
                {
                    if (!DX && !DY) continue;
                    int X=A.X+DX,Y=A.Y+DY;
                    if (X<0 || X>=Scan.NX || Y<0 || Y>=Scan.NY) continue;
                    auto Cell=Scan.Cells.find(Y*Scan.NX+X);
                    if (Cell==Scan.Cells.end()) continue;
                    for (int BID : Cell->second)
                    {
                        if (BID<=AID) continue;
                        const auto& B=Scan.Points[BID];
                        const auto Delta=B.Floor-A.Floor;
                        if (std::abs(Delta.Z)>80 || Delta.X*Delta.X+Delta.Y*Delta.Y>220.f*220.f) continue;
                        FVector Hit{};
                        if (UNavigationSystemV1::NavigationRaycast(World,A.Floor+FVector{0,0,5},B.Floor+FVector{0,0,5},&Hit,nullptr,Pawn->Controller)) continue;
                        if (!ClearCapsule(World,A.Floor,B.Floor,Ignore) || !ClearCapsule(World,B.Floor,A.Floor,Ignore)) continue;
                        Scan.Out << "{\"type\":\"edge\",\"a\":" << AID << ",\"b\":" << BID << "}\n";
                        ++Scan.Edges;
                    }
                }
            }
        }
        Ignore.Free();
    }
}

std::string MapSurvey::StartScan()
{
    if (Scan.Active) return "mapscan: already running";
    auto World=UWorld::GetWorld();
    auto PC=World ? UGameplayStatics::GetPlayerController(World,0) : nullptr;
    auto Pawn=PC && PC->Pawn ? PC->Pawn->Cast<AShooterCharacter>() : nullptr;
    if (!Pawn || !Pawn->CapsuleComponent || !Pawn->CharacterMovement) return "mapscan: no player";
    auto Nav=UNavigationSystemV1::GetNavigationSystem(World);
    if (!Nav) return "mapscan: no navigation system";
    ARecastNavMesh* Mesh=nullptr;
    for (auto Data:Nav->NavDataSet) if (Data && InWorld(Data,World)) if (auto R=Data->Cast<ARecastNavMesh>()) { Mesh=R; break; }
    if (!Mesh) return "mapscan: no registered Recast mesh";
    FVector Center{},Extent{}; Mesh->GetActorBounds(false,&Center,&Extent);
    if (Extent.X<=0 || Extent.Y<=0 || Extent.Z<=0) return "mapscan: empty bounds";
    Scan=Survey{};
    Scan.Min=Center-Extent; Scan.Max=Center+Extent;
    Scan.NX=(int)std::ceil(Extent.X*2/Spacing)+1; Scan.NY=(int)std::ceil(Extent.Y*2/Spacing)+1; Scan.NZ=(int)std::ceil(Extent.Z*2/100)+1;
    if ((int64_t)Scan.NX*Scan.NY*Scan.NZ>2000000) return "mapscan: bounds exceed two million sample limit";
    Scan.Radius=Pawn->CapsuleComponent->GetScaledCapsuleRadius(); Scan.HalfHeight=Pawn->CapsuleComponent->GetScaledCapsuleHalfHeight();
    // pawn profile is "Custom", not a real profile
    Scan.Profile=FName(L"BlockAll"); Scan.World=World; Scan.Pawn=Pawn;
    Scan.Channel=Pawn->CapsuleComponent->GetCollisionObjectType();
    TArray<AActor*> Ignore{}; Ignore.Add(Pawn);
    const FVector Here=Pawn->K2_GetActorLocation();
    FHitResult Ground{};
    const bool HasGround=UKismetSystemLibrary::LineTraceSingleByProfile(World,Here,Here-FVector{0,0,500},
        Scan.Profile,false,Ignore,EDrawDebugTrace::None,&Ground,true,FLinearColor{},FLinearColor{},0);
    const bool AboveClear=HasGround && ClearPose(World,Ground.ImpactPoint,Ignore);
    const bool BuriedClear=HasGround && ClearPose(World,Ground.ImpactPoint-FVector{0,0,Scan.HalfHeight},Ignore);
    const bool CrossesFloor=HasGround && !ClearCapsule(World,Ground.ImpactPoint+FVector{0,0,250},Ground.ImpactPoint-FVector{0,0,Scan.HalfHeight},Ignore);
    Ignore.Free();
    Ares::Log("mapscan: collision controls ground=%d above-clear=%d buried-clear=%d sweep-hits-floor=%d channel=%d",HasGround,AboveClear,BuriedClear,CrossesFloor,(int)Scan.Channel);
    if (!HasGround || !AboveClear || BuriedClear || !CrossesFloor) return "mapscan: collision controls failed; move to clear flat ground";
    auto Dir=std::filesystem::path(Ares::Config().LogPath).parent_path()/"maps";
    std::filesystem::create_directories(Dir);
    Scan.Out.open(Dir/"walkmap.jsonl");
    if (!Scan.Out) return "mapscan: cannot open output";
    Scan.Out << std::fixed << std::setprecision(3) << "{\"type\":\"metadata\",\"schema\":1,\"level\":"
        << std::quoted(UGameplayStatics::GetCurrentLevelName(World,true).ToString())
        << ",\"spacing\":" << Spacing << ",\"capsule_radius\":" << Scan.Radius << ",\"capsule_half_height\":" << Scan.HalfHeight
        << ",\"collision_profile\":" << std::quoted(Scan.Profile.ToString())
        << ",\"collision_controls_passed\":true,\"initial_overlap_checked\":true,\"min\":";
    Vector(Scan.Out,Scan.Min); Scan.Out << ",\"max\":"; Vector(Scan.Out,Scan.Max); Scan.Out << "}\n";
    Scan.Active=true;
    return "mapscan: started; writes logs/maps/walkmap.jsonl (partial until complete record)";
}

void MapSurvey::Tick()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered()) return;
    static double NextSlice = 0;
    if (Scan.Active && Ares::Seconds() >= NextSlice)
    {
        NextSlice = Ares::Seconds() + 0.01;
        AdvanceScan();
    }
    static double NextPoll = 0;
    if (Ares::Seconds() < NextPoll) return;
    NextPoll = Ares::Seconds() + 1.0;
    auto World = UWorld::GetWorld();
    auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    if (!PC || !PC->Pawn) return;
    const auto Request = std::filesystem::path(Ares::Config().LogPath).parent_path() / "maps" / "request.txt";
    std::ifstream In(Request);
    std::string Command;
    if (!std::getline(In, Command) || Command.empty()) return;
    In.close();
    std::ofstream(Request, std::ios::trunc).close();
    if (Command == "probe") Ares::Log("%s", Probe().c_str());
    else if (Command == "scan") Ares::Log("%s", StartScan().c_str());
    else if (Command == "tactical") Ares::Log("%s", TacticalMap::StartBake().c_str());
    else if (Command == "cancel") { Scan.Active=false; if (Scan.Out.is_open()) { Scan.Out << "{\"type\":\"aborted\",\"reason\":\"cancelled\"}\n"; Scan.Out.close(); } }
    else Ares::Log("map survey: unknown request (expected probe, scan, tactical or cancel)");
}

std::string MapSurvey::Probe()
{
    auto World = UWorld::GetWorld();
    if (!World) return "mapprobe: no world";
    auto PC = UGameplayStatics::GetPlayerController(World, 0);
    auto Pawn = PC && PC->Pawn ? PC->Pawn->Cast<AShooterCharacter>() : nullptr;
    if (!Pawn) return "mapprobe: wait for player spawn";
    try
    {
        auto Dir = std::filesystem::path(Ares::Config().LogPath).parent_path() / "maps";
        std::filesystem::create_directories(Dir);
        auto File = Dir / "survey.json";
        std::ofstream Out(File);
        if (!Out) return "mapprobe: cannot open output";
        Out << std::fixed << std::setprecision(3);
        const auto Level = UGameplayStatics::GetCurrentLevelName(World, true).ToString();
        auto Nav = UNavigationSystemV1::GetNavigationSystem(World);
        Out << "{\"schema\":1,\"level\":" << std::quoted(Level)
            << ",\"navigation_system\":" << (Nav ? "true" : "false")
            << ",\"registered_nav_data\":" << (Nav ? Nav->NavDataSet.Num() : 0)
            << ",\"player\":";
        const FVector Here = Pawn->K2_GetActorLocation();
        Vector(Out, Here);
        if (Pawn->CapsuleComponent)
            Out << ",\"capsule_radius\":" << Pawn->CapsuleComponent->GetScaledCapsuleRadius()
                << ",\"capsule_half_height\":" << Pawn->CapsuleComponent->GetScaledCapsuleHalfHeight();
        FVector Projected{};
        bool ProjectOK = Nav && UNavigationSystemV1::K2_ProjectPointToNavigation(
            World, Here, &Projected, nullptr, nullptr, FVector{100,100,300});
        Out << ",\"player_projects\":" << (ProjectOK ? "true" : "false") << ",\"projected\":";
        Vector(Out, Projected);
        Out << ",\"actors\":[";
        bool First = true;
        int Starts = 0, Paths = 0;
        for (auto Actor : GetObjectsOfClass<AActor>())
        {
            if (!Actor || Actor->IsDefaultObject() || Actor->bActorIsBeingDestroyed || !InWorld(Actor, World)) continue;
            const auto ClassName = Actor->Class->GetName();
            bool IsStart = Actor->IsA(APlayerStart::StaticClass());
            bool IsNav = Actor->IsA(ANavigationData::StaticClass()) || Actor->IsA(ANavMeshBoundsVolume::StaticClass());
            bool IsObjective = ClassName.find("Bomb") != std::string::npos || ClassName.find("Site") != std::string::npos;
            if (!IsStart && !IsNav && !IsObjective) continue;
            if (!First) Out << ',';
            First = false;
            FVector Center{}, Extent{};
            Actor->GetActorBounds(false, &Center, &Extent);
            Out << "{\"name\":" << std::quoted(Actor->GetName()) << ",\"class\":" << std::quoted(ClassName) << ",\"position\":";
            auto Position = Actor->K2_GetActorLocation();
            Vector(Out, Position);
            Out << ",\"bounds_center\":"; Vector(Out, Center);
            Out << ",\"bounds_extent\":"; Vector(Out, Extent);
            if (auto Recast = Actor->Cast<ARecastNavMesh>())
                Out << ",\"agent_radius\":" << Recast->AgentRadius << ",\"agent_height\":" << Recast->AgentHeight;
            if (IsStart)
            {
                ++Starts;
                auto Path = Nav ? UNavigationSystemV1::FindPathToLocationSynchronously(World, Here, Position, Pawn, nullptr) : nullptr;
                const bool Valid = Path && Path->IsValid() && !Path->IsPartial();
                if (Valid) ++Paths;
                Out << ",\"path_complete\":" << (Valid ? "true" : "false") << ",\"path\":[";
                if (Path) for (int I=0; I<Path->PathPoints.Num(); ++I) { if (I) Out << ','; Vector(Out, Path->PathPoints[I]); }
                Out << ']';
            }
            Out << '}';
        }
        Out << "]}";
        Out.close();
        if (!Out) return "mapprobe: output write failed";
        return "mapprobe: " + Level + " nav=" + (Nav ? "yes" : "no") + " projects=" + (ProjectOK ? "yes" : "no")
            + " complete spawn paths=" + std::to_string(Paths) + "/" + std::to_string(Starts) + " -> " + File.string();
    }
    catch (const std::exception& E) { return std::string("mapprobe: ") + E.what(); }
}

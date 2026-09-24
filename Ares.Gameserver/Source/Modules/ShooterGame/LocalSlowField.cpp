#include <pch.h>
#include <cmath>
#include <queue>
#include <unordered_map>
#include <algorithm>
#include <vector>

// patch node grid + overlaps, the native sampler is stripped from the client

namespace LocalSlowField { void Init(); }
INIT_MODULE(LocalSlowField);

namespace
{
    void (*BeginPlayOG)(ACellularPatch*) = nullptr;
    void (*TickOG)(ACellularPatch*, float) = nullptr;

    // sdk stub caches one ufunction for every patch class
    struct FNodeScoreParams
    {
        FIntPoint Location;
        float NodeDistance;
        bool bValidNode;
        uint8 Pad_D[3];
        float ReturnValue;
    };

    constexpr int MaxRetries = 20; // ground isnt always there on the first frame
    std::unordered_map<ACellularPatch*, int> Retries;
    std::unordered_map<ACellularPatch*, int> Dissipating;

    // distance is in cells
    bool NodeAllowed(ACellularPatch* Patch, int X, int Y, float Distance)
    {
        static std::unordered_map<UClass*, UFunction*> Cache;
        auto Found = Cache.find(Patch->Class);
        if (Found == Cache.end())
            Found = Cache.emplace(Patch->Class, Patch->Class->FindFunction("GetNodeScore")).first;
        if (!Found->second) return true;
        FNodeScoreParams Parms{};
        Parms.Location = FIntPoint{X, Y};
        Parms.NodeDistance = Distance;
        Patch->ProcessEvent(Found->second, &Parms);
        return Parms.bValidNode;
    }

    // orb is still lying on the origin cell, dont trace against it
    void CollectIgnored(ACellularPatch* Patch, TArray<AActor*>& Ignore)
    {
        Ignore.Add(Patch);
        if (Patch->Instigator) Ignore.Add((AActor*)Patch->Instigator);
        if (Patch->Owner) Ignore.Add(Patch->Owner);
        for (auto Projectile : GetObjectsOfClass<AProjectile>())
            if (!Projectile->IsDefaultObject() && !Projectile->bActorIsBeingDestroyed) Ignore.Add(Projectile);
        for (auto Character : GetObjectsOfClass<AShooterCharacter>())
            if (!Character->IsDefaultObject() && !Character->bActorIsBeingDestroyed) Ignore.Add(Character);
    }

    bool Trace(ACellularPatch* Patch, const TArray<AActor*>& Ignore, const FVector& Start, const FVector& End,
               FHitResult& Hit)
    {
        // object type queries hit trigger volumes, profile one gets the floor
        return UKismetSystemLibrary::LineTraceSingleByProfile(Patch, Start, End, FName(L"BlockAll"), true, Ignore,
            EDrawDebugTrace(0), &Hit, true, {}, {}, 0);
    }

    struct Cell
    {
        float FloorZ = 0.f;
        float CeilingZ = 0.f;
        float Distance = 0.f;
        FVector Normal{0.f, 0.f, 1.f};
        ENodeStatus Status = ENodeStatus::Outside;
    };

    bool Build(ACellularPatch* Patch, bool Verbose)
    {
        if (Patch->CellSize <= 0.f || Patch->MaxNumberOfCells < 1) return false;
        // MaxNumberOfCells is a cell count not a width
        const int Budget = Patch->MaxNumberOfCells;
        const int Radius = Budget < 24 ? Budget : 24;
        const int Side = Radius * 2 + 1;
        const auto Index = [&](int X, int Y) { return (Y + Radius) * Side + X + Radius; };

        std::vector<FVector> Centre(Side * Side);
        std::vector<char> Known(Side * Side, 0);
        const auto At = [&](int X, int Y, float Z)
        {
            const int Slot = Index(X, Y);
            if (!Known[Slot])
            {
                Centre[Slot] = Patch->GetNodeLocation(FIntPoint{X, Y});
                Known[Slot] = 1;
            }
            return FVector{Centre[Slot].X, Centre[Slot].Y, Z};
        };

        TArray<AActor*> Ignore;
        CollectIgnored(Patch, Ignore);
        struct FreeOnExit { TArray<AActor*>& Array; ~FreeOnExit() { Array.Free(); } } Guard{Ignore};

        std::vector<Cell> Cells(Side * Side);
        struct Visit { float Distance; int X, Y; float Height; };
        struct Later { bool operator()(const Visit& A, const Visit& B) const { return A.Distance > B.Distance; } };
        std::priority_queue<Visit, std::vector<Visit>, Later> Pending;
        Pending.push({0.f, 0, 0, Patch->K2_GetActorLocation().Z});

        int Count = 0;
        while (!Pending.empty() && Count < Budget)
        {
            const Visit Current = Pending.top();
            Pending.pop();
            auto& Node = Cells[Index(Current.X, Current.Y)];
            if (Node.Status == ENodeStatus::Inside) continue;
            const bool Origin = !Current.X && !Current.Y;
            if (!NodeAllowed(Patch, Current.X, Current.Y, Current.Distance))
            {
                if (Verbose && Origin)
                    Ares::Log("slow-field: %s origin refused by GetNodeScore", Patch->GetName().c_str());
                continue;
            }

            FHitResult Floor{};
            const bool Hit = Trace(Patch, Ignore, At(Current.X, Current.Y, Current.Height + Patch->StepUp),
                                   At(Current.X, Current.Y, Current.Height - Patch->StepDown), Floor);
            if (!Hit || Floor.bStartPenetrating || Floor.ImpactNormal.Z < 0.5f)
            {
                if (Verbose && Origin)
                    Ares::Log("slow-field: %s origin has no floor (hit=%d penetrating=%d normal-z=%.2f, "
                              "traced %.0f down from %.0f)", Patch->GetName().c_str(), (int)Hit,
                              (int)Floor.bStartPenetrating, Floor.ImpactNormal.Z, Patch->StepUp + Patch->StepDown,
                              Current.Height + Patch->StepUp);
                Node.Status = ENodeStatus::Blocked;
                continue;
            }
            const float FloorZ = Floor.ImpactPoint.Z;
            float CeilingZ = FloorZ + 500.f;
            FHitResult Ceiling{};
            if (Trace(Patch, Ignore, At(Current.X, Current.Y, FloorZ + 2.f),
                      At(Current.X, Current.Y, FloorZ + 10000.f), Ceiling))
                CeilingZ = Ceiling.ImpactPoint.Z;
            if (CeilingZ - FloorZ < (float)Patch->MinimumValidVolume)
            {
                Node.Status = ENodeStatus::Blocked;
                continue;
            }

            Node.FloorZ = FloorZ;
            Node.CeilingZ = CeilingZ;
            Node.Distance = Current.Distance;
            Node.Normal = Floor.ImpactNormal;
            Node.Status = ENodeStatus::Inside;
            ++Count;

            for (int DY = -1; DY <= 1; ++DY)
                for (int DX = -1; DX <= 1; ++DX)
                {
                    if (!DX && !DY) continue;
                    const int NX = Current.X + DX, NY = Current.Y + DY;
                    if (std::abs(NX) > Radius || std::abs(NY) > Radius) continue;
                    if (Cells[Index(NX, NY)].Status == ENodeStatus::Inside) continue;
                    // dont spread through walls
                    FHitResult Wall{};
                    if (Trace(Patch, Ignore, At(Current.X, Current.Y, FloorZ + Patch->StepUp),
                              At(NX, NY, FloorZ + Patch->StepUp), Wall))
                        continue;
                    Pending.push({Current.Distance + (DX && DY ? 1.41421356f : 1.f), NX, NY, FloorZ});
                }
        }

        if (!Count)
        {
            if (Verbose)
                Ares::Log("slow-field: %s sampled no ground (cell=%.0f budget=%d)",
                          Patch->GetName().c_str(), Patch->CellSize, Budget);
            return false;
        }

        // overlap box is sized off these bounds
        int MinX = Radius, MaxX = -Radius, MinY = Radius, MaxY = -Radius;
        for (int Y = -Radius; Y <= Radius; ++Y)
            for (int X = -Radius; X <= Radius; ++X)
            {
                if (Cells[Index(X, Y)].Status != ENodeStatus::Inside) continue;
                if (X < MinX) MinX = X;
                if (X > MaxX) MaxX = X;
                if (Y < MinY) MinY = Y;
                if (Y > MaxY) MaxY = Y;
            }
        const int LineSize = MaxX - MinX + 1, Lines = MaxY - MinY + 1;

        FNodeGrid Grid{};
        Grid.Min = FIntPoint{MinX, MinY};
        Grid.Max = FIntPoint{MaxX, MaxY};
        Grid.LineSize = LineSize;
        Grid.LowestNode = 1.e20f;
        Grid.HeighestNode = -1.e20f;
        Grid.MaxDistance = 0.f;
        Grid.NodeArray.Reserve(LineSize * Lines);
        for (int Y = MinY; Y <= MaxY; ++Y)
            for (int X = MinX; X <= MaxX; ++X)
            {
                const auto& Cell = Cells[Index(X, Y)];
                FPatchNode Node{};
                Node.Status = Cell.Status;
                Node.ListIndex = -1;
                if (Cell.Status == ENodeStatus::Inside)
                {
                    Node.FloorZ = Cell.FloorZ;
                    Node.CeilingZ = Cell.CeilingZ;
                    Node.Distance = Cell.Distance;
                    Node.Normal = *(FVector_NetQuantizeNormal*)&Cell.Normal;
                    for (int DY = -1; DY <= 1 && !Node.bEdgePoint; ++DY)
                        for (int DX = -1; DX <= 1; ++DX)
                        {
                            if (!DX && !DY) continue;
                            const int NX = X + DX, NY = Y + DY;
                            if (NX < MinX || NX > MaxX || NY < MinY || NY > MaxY ||
                                Cells[Index(NX, NY)].Status != ENodeStatus::Inside)
                            {
                                Node.bEdgePoint = 1;
                                break;
                            }
                        }
                    if (Cell.FloorZ < Grid.LowestNode) Grid.LowestNode = Cell.FloorZ;
                    if (Cell.CeilingZ > Grid.HeighestNode) Grid.HeighestNode = Cell.CeilingZ;
                    if (Cell.Distance > Grid.MaxDistance) Grid.MaxDistance = Cell.Distance;
                }
                else
                {
                    Node.Distance = 1.e20f;
                }
                Grid.NodeArray.Add(Node);
            }
        Grid.bPatchIsValid = true;

        Patch->NodeGrid = Grid;
        Patch->NetMulticastUpdateNodeGrid(Patch->NodeGrid);
        Ares::Log("slow-field: %s covered %d/%d cells in %dx%d (built=%d overlap=%.0f)",
                  Patch->GetName().c_str(), Count, Budget, LineSize, Lines,
                  (int)Patch->PatchBuilt(), Patch->PatchOverlapExtent);
        return true;
    }

    bool NeedsGrid(ACellularPatch* Patch)
    {
        return Patch && !Patch->bActorIsBeingDestroyed && Patch->Role == ENetRole::ROLE_Authority &&
               !Patch->NodeGrid.bPatchIsValid && !Patch->NodeGrid.NodeArray.Num() && !Patch->PatchBuilt();
    }

    void BeginPlay(ACellularPatch* Patch)
    {
        BeginPlayOG(Patch);
        if (!NeedsGrid(Patch)) return;
        // old patch at the same address can leave stale entries
        Retries.erase(Patch);
        Dissipating.erase(Patch);
        if (!Build(Patch, true)) Retries[Patch] = MaxRetries;
    }

    // nothing fills OverlappingActors in the client, box alone is too coarse
    bool IsCovered(ACellularPatch* Patch, AActor* Actor)
    {
        if (!Actor || Actor->bActorIsBeingDestroyed || !Actor->IsA<AShooterCharacter>()) return false;
        const FVector Where = Actor->K2_GetActorLocation();
        // GetCellHeight drops the outer ring, GetNodeAt reads a NodeMap nobody fills
        const auto& Grid = Patch->NodeGrid;
        FVector Offset{};
        const FIntPoint Coords = Patch->GetClosestGridPoint(Where, &Offset);
        if (Grid.bPatchIsValid && Grid.LineSize > 0 &&
            Coords.X >= Grid.Min.X && Coords.X <= Grid.Max.X &&
            Coords.Y >= Grid.Min.Y && Coords.Y <= Grid.Max.Y)
        {
            const int Slot = (Coords.Y - Grid.Min.Y) * Grid.LineSize + (Coords.X - Grid.Min.X);
            if (Slot >= 0 && Slot < Grid.NodeArray.Num())
            {
                const auto& Node = Grid.NodeArray[Slot];
                return Node.Status == ENodeStatus::Inside &&
                       Where.Z > Node.FloorZ - 100.f && Where.Z < Node.FloorZ + 250.f;
            }
        }
        return false;
    }

    float AuthoredFloat(UObject* Object, const char* Name)
    {
        for (auto Class = (UStruct*)Object->Class; Class; Class = Class->Super)
            for (auto Field = Class->Children; Field; Field = Field->Next)
            {
                auto Property = Field->Cast<UProperty>();
                if (!Property || Property->GetName() != Name) continue;
                if (Property->Class->GetName() != "FloatProperty") return 0.f;
                return *(float*)((uint8*)Object + Property->Offset);
            }
        return 0.f;
    }

    // nothing calls BeginDissipate so the field just pops out
    void TickDissipate(ACellularPatch* Patch)
    {
        if (Patch->bActorIsBeingDestroyed) { Dissipating.erase(Patch); return; }
        if (!Patch->PatchBuilt() || Dissipating.count(Patch)) return;
        const float Remaining = Patch->GetLifeSpan();
        const float Duration = AuthoredFloat(Patch, "DissipateDuration");
        if (Remaining <= 0.f || Duration <= 0.f || Remaining > Duration) return;
        auto Function = Patch->Class->FindFunction("BeginDissipate");
        if (!Function) return;
        Dissipating[Patch] = 1;
        std::vector<uint8> Parms(Function->Size > 0 ? Function->Size : 1, 0);
        Patch->ProcessEvent(Function, Parms.data());
        Ares::Log("slow-field: %s dissipates with %.1fs left", Patch->GetName().c_str(), Remaining);
    }

    void TrackOverlaps(ACellularPatch* Patch)
    {
        if (!Patch->bTrackPatchOverlaps || !Patch->PatchBuilt() || !Patch->BoxOverlap ||
            Patch->bActorIsBeingDestroyed)
            return;
        TArray<AActor*> InBox;
        Patch->BoxOverlap->GetOverlappingActors(&InBox, nullptr);
        std::vector<AActor*> Covered;
        for (auto Actor : InBox)
            if (IsCovered(Patch, Actor)) Covered.push_back(Actor);
        InBox.Free();

        const auto WasTracked = [&](AActor* Actor)
        {
            for (auto Tracked : Patch->OverlappingActors)
                if (Tracked == Actor) return true;
            return false;
        };
        for (auto Actor : Covered)
        {
            if (WasTracked(Actor)) continue;
            Patch->OverlappingActors.Add(Actor);
            Patch->OnActorBeginOverlapPatch(Actor);
            Ares::Log("slow-field: %s covers %s", Patch->GetName().c_str(), Actor->GetName().c_str());
        }
        for (int Slot = Patch->OverlappingActors.Num() - 1; Slot >= 0; --Slot)
        {
            auto Actor = Patch->OverlappingActors[Slot];
            if (std::find(Covered.begin(), Covered.end(), Actor) != Covered.end()) continue;
            Patch->OverlappingActors.Remove(Slot);
            if (!Actor) continue;
            Patch->OnActorEndOverlapPatch(Actor);
            Ares::Log("slow-field: %s released %s", Patch->GetName().c_str(), Actor->GetName().c_str());
        }
    }

    void Tick(ACellularPatch* Patch, float DeltaSeconds)
    {
        TickOG(Patch, DeltaSeconds);
        TrackOverlaps(Patch);
        TickDissipate(Patch);
        auto Pending = Retries.find(Patch);
        if (Pending == Retries.end()) return;
        const bool Last = --Pending->second <= 0;
        if (!NeedsGrid(Patch) || Build(Patch, Last) || Last) Retries.erase(Pending);
    }
}

void LocalSlowField::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Hooking::Hook(ImageBase + 0x1926EE0, BeginPlay, BeginPlayOG);
    Hooking::Hook(ImageBase + 0x194AA10, Tick, TickOG);
}

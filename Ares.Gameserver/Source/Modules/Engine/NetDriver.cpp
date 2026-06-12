#include <pch.h>
#include <Headers/Modules/Engine/NetDriver.hpp>
INIT_MODULE(NetDriver);

void (*TickFlushOG)(UNetDriver* _this, float DeltaTime);
void NetDriver::TickFlush(UNetDriver* _this, float DeltaTime)
{
    ServerReplicateActors(_this, DeltaTime);

    return TickFlushOG(_this, DeltaTime);
}

int32 NetDriver::ServerReplicateActors(UNetDriver* _this, float DeltaSeconds)
{
    if (_this->ClientConnections.Num() == 0 || !_this->World)
    {
        return 0;
    }

    ++*(int32*)(__int64(_this) + 0x30C);
    //ReplicationFrame++;

    const int32 NumClientsToTick = ServerReplicateActors_PrepConnections(_this, DeltaSeconds);
    
	if (NumClientsToTick == 0)
    {
        // No connections are ready this frame
        return 0;
    }

  //  static float (*GetMaxTickRate)(UGameEngine*, float DeltaSeconds) = decltype(GetMaxTickRate)(ImageBase + 0xF8C334);
   // static void (*SendClientAdjustment)(APlayerController*) = decltype(SendClientAdjustment)(ImageBase + 0x9FA03E0);

    float ServerTickTime =120.f;// GetMaxTickRate(UGameEngine::GetEngine(), DeltaSeconds);
    ServerTickTime = 1.f / ServerTickTime;

    std::vector<AActor*> ConsiderList;
    ServerReplicateActors_BuildConsiderList(_this, ConsiderList, ServerTickTime);
    for (int32 i = 0; i < _this->ClientConnections.Num(); i++)
    {
        UNetConnection* Connection = _this->ClientConnections[i];
        if (Connection)
        {
            if (Connection->ViewTarget)
            {
                if (Connection->PlayerController)
                {
                 //   SendClientAdjustment(Connection->PlayerController);
                    int Count = ServerReplicateActors_ProcessActors(_this, Connection, ConsiderList);
                    printf("[Runtime] replicated %d actors to client %d\n", Count, i);
                }
            }
        }
    }

    return int32();
}

int32 NetDriver::ServerReplicateActors_ProcessActors(UNetDriver* _this, UNetConnection* Connection, std::vector<AActor*>& Actors)
{
    static void (*CallPreReplication)(AActor* Actor, UNetDriver* Driver) = decltype(CallPreReplication)(ImageBase + 0x3230F70);
    static bool (*DemoReplicateActor)(UNetDriver*, AActor*, UNetConnection*, bool) = decltype(DemoReplicateActor)(ImageBase + 0x3489090);

    int FinalActors = 0;

    if (Connection)
    {
        for (auto& Actor : Actors)
        {
            if (Actor && Connection)
            {
                if (!Actor)
                    continue;

                if (Actor->IsA(APlayerController::StaticClass()) && Connection->PlayerController && Actor != Connection->PlayerController)
                    continue;

                CallPreReplication(Actor, _this);
                DemoReplicateActor(_this, Actor, Connection, false);
                FinalActors++;
            }
        }
    }

    return FinalActors;
}

int32 NetDriver::ServerReplicateActors_PrepConnections(UNetDriver* _this, float DeltaSeconds)
{
    int32 NumClientsToTick = _this->ClientConnections.Num();

    bool bFoundReadyConnection = false;

    for (int32 ConnIdx = 0; ConnIdx < _this->ClientConnections.Num(); ConnIdx++)
    {
        UNetConnection* Connection = _this->ClientConnections[ConnIdx];
        if (!Connection)
            continue;
        // Handle not ready channels.
        //@note: we cannot add Connection->IsNetReady(0) here to check for saturation, as if that's the case we still want to figure out the list of relevant actors
        //			to reset their NetUpdateTime so that they will get sent as soon as the connection is no longer saturated
        AActor* OwningActor = Connection->OwningActor;
        if (OwningActor != NULL)
        {
            bFoundReadyConnection = true;

            // the view target is what the player controller is looking at OR the owning actor itself when using beacons
            AActor* DesiredViewTarget = OwningActor;
            if (Connection->PlayerController)
            {
                if (AActor* ViewTarget = Connection->PlayerController->GetViewTarget())
                {
                    DesiredViewTarget = ViewTarget;
                }
            }
            Connection->ViewTarget = DesiredViewTarget;

            for (int32 ChildIdx = 0; ChildIdx < Connection->Children.Num(); ChildIdx++)
            {
                UNetConnection* Child = Connection->Children[ChildIdx];
                APlayerController* ChildPlayerController = Child->PlayerController;
                AActor* DesiredChildViewTarget = Child->OwningActor;

                if (ChildPlayerController)
                {
                    AActor* ChildViewTarget = ChildPlayerController->GetViewTarget();

                    if (ChildViewTarget)
                    {
                        DesiredChildViewTarget = ChildViewTarget;
                    }
                }

                Child->ViewTarget = DesiredChildViewTarget;
            }
        }
        else
        {
            Connection->ViewTarget = NULL;
            for (int32 ChildIdx = 0; ChildIdx < Connection->Children.Num(); ChildIdx++)
            {
                Connection->Children[ChildIdx]->ViewTarget = NULL;
            }
        }
    }

    return bFoundReadyConnection ? NumClientsToTick : 0;
}

ULevel* GetLevel(AActor* Actor)
{
    auto Outer = Actor->Outer;

    while (Outer)
    {
        if (Outer->Class == ULevel::StaticClass())
            return (ULevel*)Outer;
        else
            Outer = Outer->Outer;
    }
    return nullptr;
}

void NetDriver::ServerReplicateActors_BuildConsiderList(UNetDriver* _this, std::vector<AActor*>& OutConsiderList, const float ServerTickTime)
{
    static void (*CallPreReplication)(AActor* Actor, UNetDriver* Driver) = decltype(CallPreReplication)(ImageBase + 0x3230F70);
    TArray<AActor*> Actors;
    UGameplayStatics::GetAllActorsOfClass(UWorld::GetWorld(), AActor::StaticClass(), &Actors);

    for (auto& Actor : Actors)
    {
        if (!Actor || Actor->NetDriverName != _this->NetDriverName)
            continue;

        auto Outer = Actor->Outer;
        if (Actor->RemoteRole == ENetRole::ROLE_None || (Actor->NetDormancy == ENetDormancy::DORM_Initial))
            continue;

        ULevel* Level = GetLevel(Actor);
        if (!Level || Level == Level->OwningWorld->CurrentLevelPendingVisibility || Level == Level->OwningWorld->CurrentLevelPendingInvisibility)
            continue;

        OutConsiderList.push_back(Actor);
        CallPreReplication(Actor, _this);
    }
}

void NetDriver::Init()
{
    Hooking::Hook(ImageBase + 0x3704C10, TickFlush, TickFlushOG); 
}


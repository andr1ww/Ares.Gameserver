#pragma once
#include "pch.h"

class NetDriver
{
public:
    static void Init();

private:
    static void TickFlush(UNetDriver* _this, float DeltaTime);
    static int32 ServerReplicateActors(UNetDriver* _this, float DeltaSeconds);
    static int32 ServerReplicateActors_PrepConnections(UNetDriver*, float);
    static void ServerReplicateActors_BuildConsiderList(UNetDriver*, std::vector<AActor*>&, const float);
    static int32 ServerReplicateActors_ProcessActors(UNetDriver*, UNetConnection*, std::vector<AActor*>&);
};
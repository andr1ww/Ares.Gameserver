#include <pch.h>
#include <Headers/Modules/Engine/World.hpp>
INIT_MODULE(World);

bool World::Listen(UWorld* _this)
{
    auto CreateNetDriver = (UNetDriver * (*)(UEngine*, UWorld*, FName))(ImageBase + 0x28B5300);
    auto InitListen = (bool (*)(UNetDriver*, UWorld*, FURL*, bool, FString&))(ImageBase + 0x423D20);
    auto SetWorld = (void (*)(UNetDriver*, UWorld*))(ImageBase + 0x264A500);

    auto NetDriverName = FName(L"GameNetDriver");

    auto NetDriver = _this->NetDriver = CreateNetDriver(UEngine::GetEngine(), _this, NetDriverName);

    NetDriver->NetDriverName = NetDriverName;
    NetDriver->World = _this;

    for (auto& Collection : _this->LevelCollections)
        Collection.NetDriver = NetDriver;

    FURL URL;
    URL.Port = 7777;

    FString Err;

    auto ret = InitListen(NetDriver, _this, &URL, false, Err);

    SetWorld(NetDriver, _this);

    return ret;
}

void World::Init()
{
    Hooking::RetTrue(ImageBase + 0x3A71D20); // GetNetMode
    Hooking::Rel32Hook(ImageBase + 0x3A04E7E, Listen);
}
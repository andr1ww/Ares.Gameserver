#include <pch.h>
#include <Headers/Modules/Engine/World.h>
#include <intrin.h>
INIT_MODULE(World);

bool World::Listen(UWorld* _this)
{
    UEngine::GetEngine()->NetDriverDefinitions.Search([](FNetDriverDefinition& Def)
    {
        if (Def.DefName == FName(L"GameNetDriver"))
        {
            Def.DriverClassName = FString(L"/Script/OnlineSubsystemUtils.IpNetDriver");
            return true;
        }
        return false;
    });

    auto CreateNetDriver = (UNetDriver * (*)(UEngine*, UWorld*, FName))(ImageBase + 0x39F3C90);
    auto InitListen = (bool (*)(UNetDriver*, UWorld*, FURL*, bool, FString&))(ImageBase + 0x1451A00);
    auto SetWorld = (void (*)(UNetDriver*, UWorld*))(ImageBase + 0x37038B0);

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

    printf("[Runtime] Listening!\n");
    return ret;
}

int (*GetNetModeOG)(UWorld* _this);
int GetNetMode(UWorld* _this)
{
    if (_this->NetDriver)
        return 1; // NM_DedicatedServer

    return GetNetModeOG(_this);
}

void World::Init()
{
    Hooking::Hook(ImageBase + 0x3A71D20, GetNetMode, GetNetModeOG); // GetNetMode
    Hooking::Rel32Hook(ImageBase + 0x3A04E7E, Listen);
}
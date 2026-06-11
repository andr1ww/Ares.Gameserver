// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"

void MainThread()
{
    SetConsoleTitleA("Ares | Initializing");

    for (auto& Initter : Initters)
        Initter();

    *(bool*)(ImageBase + 0x6C67BE9) = false; // GIsClient
    *(bool*)(ImageBase + 0x6C67BEA) = true;  // GIsServer

    UWorld::GetWorld()->OwningGameInstance->LocalPlayers[0]->PlayerController->SwitchLevel(L"/Game/Maps/Ascent/Ascent");
    UWorld::GetWorld()->OwningGameInstance->LocalPlayers.Remove(0);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        std::thread(MainThread).detach();
        break;
    }

    return TRUE;
}

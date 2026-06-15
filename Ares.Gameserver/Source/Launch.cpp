// dllmain.cpp : Defines the entry point for the DLL application.
#include "pch.h"
#include <fstream>

static void* (*ProcessEventOG)(UObject*, UFunction*, void*);
static std::vector<std::string> LoggedFunctions;
void* ProcessEvent(UObject* Obj, UFunction* Function, void* Params)
{
    static bool firstCall = true;
    static std::ofstream logFile("Ares_PE.log", firstCall ? std::ios::trunc : std::ios::app);
    if (firstCall)
        firstCall = false;

    if (Function)
    {
        std::string FunctionName = Function->GetFullName();

        if (std::find(LoggedFunctions.begin(), LoggedFunctions.end(), FunctionName) == LoggedFunctions.end())
        {
            printf("[ProcessEvent] %s\n", FunctionName.c_str());
            LoggedFunctions.push_back(FunctionName);
        }

        if (FunctionName.find("oordinatedHUDModel.HandlePlayerStateListUpdated") != std::string::npos)
        {
            return nullptr;
        }

        logFile << FunctionName << std::endl;
    }

    return ProcessEventOG(Obj, Function, Params);
}

void InternalServerTryActivateAbility(UAbilitySystemComponent* _this, FGameplayAbilitySpecHandle Handle, bool, FPredictionKey* PredictionKey, void* TriggerEventData)
{
    printf("[Runtime] InternalServerTryActivateAbility Handle: %d\n", Handle.Handle);
}

void MainThread()
{
    SetConsoleTitleA("Ares | Initializing");

    for (auto& Initter : Initters)
        Initter();

    *(bool*)(ImageBase + 0x6C67BE9) = false; // GIsClient
    *(bool*)(ImageBase + 0x6C67BEA) = true;  // GIsServer

    Hooking::Hook(ImageBase + Offsets::ProcessEvent, ProcessEvent, ProcessEventOG);
    Hooking::HookEvery<UAbilitySystemComponent>(0x7F8 / 8, &InternalServerTryActivateAbility);

    printf("[Runtime] Opening ascent!\n");
   // UGameplayStatics::OpenLevel(UWorld::GetWorld(), FName(TEXT("/Game/Maps/Duality/Duality")), false, TEXT(""));

        UGameplayStatics::OpenLevel(UWorld::GetWorld(), FName(TEXT("/Game/Maps/Ascent/Ascent")), false, TEXT(""));
    //UGameplayStatics::OpenLevel(UWorld::GetWorld(), FName(TEXT("/Game/Maps/Poveglia/Range?game=/Game/GameModes/ShootingRange/ShootingRangeGameMode.ShootingRangeGameMode_C")), false, TEXT(""));
   // UGameplayStatics::OpenLevel(UWorld::GetWorld(), FName(TEXT("/Game/Maps/PregameV2/CharacterSelectPersistentLevel")), true, TEXT(""));
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

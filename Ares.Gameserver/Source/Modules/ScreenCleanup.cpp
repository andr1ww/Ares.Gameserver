#include <pch.h>
#include <Headers/Modules/ScreenCleanup.h>

namespace
{
    // Update() resets bMapComplete from platform state, force it back after
    void (*MapLoadUpdateOG)(UObject*, FFrame&, void*) = nullptr;

    void MapLoadUpdateDetour(UObject* Context, FFrame& Stack, void* Result)
    {
        if (MapLoadUpdateOG)
            MapLoadUpdateOG(Context, Stack, Result);

        if (UMapLoadModel* Model = Context ? Context->Cast<UMapLoadModel>() : nullptr)
        {
            Model->bMapReady = true;
            Model->bMapComplete = true;
        }
    }

    void HoldMapComplete()
    {
        static bool bInstalled = false;
        if (bInstalled)
            return;
        bInstalled = true;

        UFunction* Update = UMapLoadModel::StaticClass()->FindFunction("Update");
        if (!Update)
        {
            Ares::Log("screen: MapLoadModel::Update not found");
            return;
        }

        Ares::Log("screen: MapLoadModel::Update exec at client+0x%llX",
                  (unsigned long long)((uintptr_t)Update->ExecFunction - ImageBase));
        Hooking::ExecHook(Update, MapLoadUpdateDetour, MapLoadUpdateOG);
    }

    // agent select leaves cinematic mode on, kills all input
    void ReleaseInput(APlayerController* PC)
    {
        if (AAresPlayerController* Ares = PC->Cast<AAresPlayerController>())
        {
            const bool bWas = Ares->InCinematicMode();
            Ares::Log("input: cinematic mode was %d", (int)bWas);
        }

        PC->SetCinematicMode(false, false, false, true, true);
        PC->SetIgnoreMoveInput(false);
        PC->SetIgnoreLookInput(false);
        PC->EnableInput(PC);
        PC->bShowMouseCursor = false;

        if (APawn* Pawn = PC->Pawn)
            Pawn->EnableInput(PC);

        Ares::Log("input: handed control back to the player");
    }

    UMenuStackManager* FindMenuStack()
    {
        for (UMenuStackManager* Manager : GetObjectsOfClass<UMenuStackManager>())
        {
            if (Manager && !Manager->IsDefaultObject())
                return Manager;
        }
        return nullptr;
    }
} // namespace

void ScreenCleanup::Tick()
{
    static bool bDone = false;
    if (bDone)
        return;

    static bool bBusy = false;
    FReentrancyGuard Guard(bBusy);
    if (!Guard.Entered())
        return;

    static double LastCheck = -1.0;
    const double Now = Ares::Seconds();
    if (Now - LastCheck < 0.25)
        return;
    LastCheck = Now;

    UWorld* World = UWorld::GetWorld();
    if (!World)
        return;

    APlayerController* Local = UGameplayStatics::GetPlayerController(World, 0);
    if (!Local || !Local->Pawn)
        return;

    bDone = true;
    HoldMapComplete();

    // loading screen waits on this, never completes offline
    for (UMapLoadModel* Model : GetObjectsOfClass<UMapLoadModel>())
    {
        if (!Model || Model->IsDefaultObject())
            continue;

        Ares::Log("screen: map load model '%s' ready=%d complete=%d",
                  Model->MapName.ToString().c_str(), (int)Model->bMapReady, (int)Model->bMapComplete);

        Model->HandleGamePhaseChanged(EAresGamePhase::InRound, 0);
        Model->bMapReady = true;
        Model->bMapComplete = true;
        Model->Update();

        Ares::Log("screen: after update ready=%d complete=%d",
                  (int)Model->bMapReady, (int)Model->bMapComplete);

        // Update() resets these
        Model->bMapReady = true;
        Model->bMapComplete = true;
    }

    // VAL 29 screen is the flow stuck in PlatformFaulted
    for (UGameFlowStateManagerV2* Flow : GetObjectsOfClass<UGameFlowStateManagerV2>())
    {
        if (!Flow || Flow->IsDefaultObject())
            continue;

        Ares::Log("screen: game flow state = %d (PlatformFaulted=8, InGame=6)",
                  (int)Flow->GetCurrentStateType());

        UGameFlowState* InGame = Flow->GetState(EGameFlowStateType::InGame);
        if (!InGame)
        {
            Ares::Log("screen: no InGame flow state registered");
            continue;
        }

        Flow->CurrentState = InGame;
        Flow->OnStateChanged.Process(EGameFlowStateType::InGame);
        Ares::Log("screen: pushed game flow to InGame, now %d", (int)Flow->GetCurrentStateType());
    }

    // the actual fullscreen cover, lives outside the world
    for (UTransitionManagerWidget* Transition : GetObjectsOfClass<UTransitionManagerWidget>())
    {
        if (!Transition || Transition->IsDefaultObject())
            continue;

        Ares::Log("screen: transition widget %s active=%d ending=%d content=%s",
                  Transition->Class->GetName().c_str(),
                  (int)Transition->bTransitionIsActive,
                  (int)Transition->bTransitionIsEnding,
                  Transition->CurrentTransitionWidget
                      ? Transition->CurrentTransitionWidget->Class->GetName().c_str()
                      : "<none>");

        Transition->bTransitionIsEnding = true;
        Transition->HandleEndTransitionCompleted();

        if (USimpleLifecycleWidget* Content = Transition->CurrentTransitionWidget)
        {
            Content->RemoveFromParent();
            Content->SetVisibility(ESlateVisibility::Collapsed);
        }
        if (UPanelWidget* Container = Transition->ContainerWidget)
            Container->SetVisibility(ESlateVisibility::Collapsed);

        Transition->bTransitionIsActive = false;
        Transition->SetVisibility(ESlateVisibility::Collapsed);

        Ares::Log("screen: transition torn down (active=%d)", (int)Transition->bTransitionIsActive);
    }

    UMenuStackManager* MenuStack = FindMenuStack();
    if (!MenuStack)
    {
        Ares::Log("screen: no menu stack manager found");
        ReleaseInput(Local);
        return;
    }

    Ares::Log("screen: menu stack has %d entry/entries", MenuStack->OpenedMenus.Num());
    for (int32 i = 0; i < MenuStack->OpenedMenus.Num(); i++)
    {
        UMenuStackEntry* Entry = MenuStack->OpenedMenus[i];
        if (!Entry)
            continue;
        UUserWidget* Content = Entry->Content;
        Ares::Log("  entry %d: %s", i,
                  Content && Content->Class ? Content->Class->GetName().c_str() : "<no content>");
    }

    MenuStack->CloseAll();
    Ares::Log("screen: closed the menu stack; %d entry/entries left",
              MenuStack->OpenedMenus.Num());

    ReleaseInput(Local);

    // given guns spawn with empty mags
    int Filled = 0;
    for (UAmmoComponent* Ammo : GetObjectsOfClass<UAmmoComponent>())
    {
        if (!Ammo || Ammo->IsDefaultObject() || Ammo->MaxAmmo <= 0)
            continue;
        if (Ammo->Ammo >= Ammo->MaxAmmo)
            continue;

        Ammo->Ammo = Ammo->MaxAmmo;
        Filled++;
    }
    Ares::Log("ammo: topped up %d magazine(s)", Filled);
}

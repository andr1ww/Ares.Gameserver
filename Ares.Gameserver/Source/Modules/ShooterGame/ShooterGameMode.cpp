#include <pch.h>
#include <Headers/Modules/ShooterGame/ShooterGameMode.h>
INIT_MODULE(ShooterGameMode);

// any guid works, nothing checks it
static const wchar_t* kMatchId = L"88a1b12a-52ac-42b6-b443-a59bee67977e";

bool ShooterGameMode::ReadyToStartMatch(AShooterGameMode* _this)
{
    static bool bSublevelsRequested = false;
    static bool bStarted = false;
    static double SublevelRequestTime = 0.0;

    if (!bSublevelsRequested)
    {
        bSublevelsRequested = true;
        SublevelRequestTime = Ares::Seconds();

        if (AShooterGameState* GameState = _this->GameState->Cast<AShooterGameState>())
            GameState->MatchInfo.MatchID = kMatchId;

        int32 Requested = 0;
        if (AAresWorldSettings* WorldSettings = UWorld::GetWorld()->K2_GetWorldSettings()->Cast<AAresWorldSettings>())
        {
            for (auto Level : WorldSettings->GetSublevelsToStreamForGameMode(_this->Class, _this->GameModeSublevelKeys))
            {
                bool Success = false;
                FName LevelName = Level.ObjectID.AssetPathName;
                ULevelStreamingDynamic::LoadLevelInstance(UWorld::GetWorld(), UKismetStringLibrary::Conv_NameToString(LevelName), FVector(), FRotator(), &Success);
                Requested++;
            }
        }

        Ares::Log("game mode %s: requested %d sublevel(s)", _this->Class->GetName().c_str(), Requested);
        return false;
    }

    if (bStarted)
        return true;

    UNetDriver* NetDriver = UWorld::GetWorld()->NetDriver;
    const int32 Clients = NetDriver ? NetDriver->ClientConnections.Num() : 0;

    if (Clients > 0)
    {
        bStarted = true;
        Ares::Log("starting match: %d remote client(s) connected", Clients);
        return true;
    }

    if (Ares::Config().Mode == Ares::EMode::Standalone)
    {
        const double Waited = Ares::Seconds() - SublevelRequestTime;
        if (Waited >= Ares::Config().StartDelay)
        {
            bStarted = true;
            Ares::Log("starting match: standalone, sublevels had %.1fs to stream", Waited);
            return true;
        }
        return false;
    }

    return false;
}

APawn* ShooterGameMode::SpawnDefaultPawnFor(AShooterGameMode* _this, AShooterPlayerController* NewPlayer, AActor* StartSpot)
{
    AShooterPlayerState* PlayerState = NewPlayer->PlayerState->Cast<AShooterPlayerState>();
    AShooterGameState* GameState = _this->GameState->Cast<AShooterGameState>();

    if (Ares::Config().Mode == Ares::EMode::Standalone && !PlayerState->PlayerInfo)
    {
        // stripped in client, abilities find the controller through this
        auto Info = SpawnActorUnfinished<AOwnerExclusivePlayerInfo>(
            AOwnerExclusivePlayerInfo::StaticClass(), {}, {}, NewPlayer);
        if (Info)
        {
            Info->AresController = NewPlayer;
            PlayerState->PlayerInfo = Info;
            FinishSpawnActor(Info, {}, {});
            Ares::Log("player: initialized local owner-exclusive player info");
        }
    }

    UClass* PawnClass = _this->GetDefaultPawnClassForController(NewPlayer);
    APawn* Pawn = (APawn*)SpawnActor(PawnClass, StartSpot->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);

    for (int Attempt = 0; !Pawn && Attempt < 8; Attempt++)
    {
        AActor* PlayerStart = _this->ChoosePlayerStart(NewPlayer);
        if (!PlayerStart)
            break;
        Pawn = (APawn*)SpawnActor(PawnClass, PlayerStart->GetTransform(), NewPlayer, ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
    }

    if (!Pawn)
    {
        Ares::Log("SpawnDefaultPawnFor: could not spawn a pawn for %s", NewPlayer->GetName().c_str());
        return nullptr;
    }

    GameState->MulticastSetPhase(EAresGamePhase::InRound);
    GameState->Phase = EAresGamePhase::InRound;
    _this->OnPhaseChange.Process(EAresGamePhase::InRound);
    _this->OnPlayerSpawned.Process(Pawn);

    NewPlayer->CachedShooterCharacter = Pawn->Cast<AShooterCharacter>();
    PlayerState->PossessedCharacter = NewPlayer->CachedShooterCharacter;
    PlayerState->SpawnedCharacterState.SpawnedCharacter = NewPlayer->CachedShooterCharacter;
    PlayerState->SpawnedCharacterState.bIsAlive = true;
    PlayerState->OnRep_PossessedCharacter();
    PlayerState->OnRep_SpawnedCharacterState({});

    NewPlayer->PlayerViewTarget = Pawn;
    NewPlayer->OnRep_ViewTarget(nullptr);

    auto Equippables = NewPlayer->CachedShooterCharacter->StartingEquippableClasses;

    for (int i = 0; i < Equippables.Num(); i++)
    {
        TSubclassOf<AAresEquippable> EquippableClass = Equippables[i];
        if (!EquippableClass)
            continue;

        NewPlayer->CachedShooterCharacter->Inventory->AuthCreateAndAddEquippable(EquippableClass);
    }

    AAresEquippable* Preferred = nullptr;

    // starting equippables is just the knife, no store offline
    for (const std::wstring& Path : Ares::Config().Loadout)
    {
        UClass* WeaponClass = FindObject<UClass>(Path);
        if (!WeaponClass)
        {
            Ares::LogW(L"loadout: could not find %s", Path.c_str());
            continue;
        }

        if (AAresEquippable* Granted = NewPlayer->CachedShooterCharacter->Inventory->AuthCreateAndAddEquippable(WeaponClass))
        {
            Ares::Log("loadout: granted %s", WeaponClass->GetName().c_str());
            Preferred = Granted;
        }
        else
        {
            Ares::Log("loadout: failed to grant %s", WeaponClass->GetName().c_str());
        }
    }

    AAresEquippable* Melee = NewPlayer->CachedShooterCharacter->Inventory->ItemSlots[(uint8)EAresItemSlot::Melee]->Contents->Cast<AAresEquippable>();
    if (!Preferred)
        Preferred = Melee;
    Ares::Log("loadout: equipping %s", Preferred ? Preferred->GetName().c_str() : "<nothing>");

    NewPlayer->CachedShooterCharacter->Inventory->OnRep_ItemSlots();
    NewPlayer->CachedShooterCharacter->OnInventoryItemsChanged();

    // setting CurrentEquippable by hand skips the draw and the 1p model
    if (Preferred)
        NewPlayer->CachedShooterCharacter->Inventory->SetDesiredEquippable(
            Preferred, EEquipSpeed::Normal, EEquipSource::GrantEquippable);
    NewPlayer->CachedShooterCharacter->Inventory->LatestDesiredEquippableSlot = Preferred == Melee ? EAresItemSlot::Melee : EAresItemSlot::Secondary;

    FPendingEquippableChange PendingChange;
    PendingChange.NewCurrentEquippable = Preferred;
    PendingChange.Timestamp.Character = NewPlayer->CachedShooterCharacter;
    PendingChange.Timestamp.RespawnNumber = 0;
    PendingChange.Timestamp.NetTimestamp = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());

    NewPlayer->CachedShooterCharacter->Inventory->PendingEquippableChanges.Add(PendingChange);
    NewPlayer->CachedShooterCharacter->Inventory->UpdatePendingEquippableChange(UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()));
    NewPlayer->CachedShooterCharacter->Inventory->OnRep_EquippableChange();

    NewPlayer->CachedShooterCharacter->Inventory->AuthCorrectionState = EServerCorrectionState::CorrectionIssued;
    NewPlayer->CachedShooterCharacter->Inventory->OwningClientCurrentCorrectionIndex = 0;
    NewPlayer->CachedShooterCharacter->Inventory->AuthServerCorrectRepVariables.CorrectionIndex = 0;
    NewPlayer->CachedShooterCharacter->Inventory->AuthServerCorrectRepVariables.CurrentEquippable = Preferred;
    NewPlayer->CachedShooterCharacter->Inventory->ClientHandleEquippableDisagreement(Preferred, 0);

    NewPlayer->CachedShooterCharacter->Inventory->bInitialServerCorrectionSent = true;
    NewPlayer->CachedShooterCharacter->Inventory->bInitialServerCorrectionProcessed = true;
    NewPlayer->CachedShooterCharacter->Inventory->AuthCorrectionState = EServerCorrectionState::Agreement;
    NewPlayer->CachedShooterCharacter->Inventory->ServerRequestCorrection(0);
    NewPlayer->CachedShooterCharacter->Inventory->ClientOnEquippableAdded(Preferred);
    NewPlayer->CachedShooterCharacter->InputStateComponent->OnEquippableChanged(Preferred);
    NewPlayer->CachedShooterCharacter->InputStateComponent->CurrentEquippableTarget = Preferred;

    const FVector Where = StartSpot->K2_GetActorLocation();
    Ares::Log("spawned pawn for %s at %.0f %.0f %.0f", NewPlayer->GetName().c_str(), Where.X, Where.Y, Where.Z);

    return Pawn;
}

void (*HandleStartingNewPlayerOG)(AShooterGameMode* _this, AAresPlayerController* NewPlayer);
void ShooterGameMode::HandleStartingNewPlayer(AShooterGameMode* _this, AShooterPlayerController* NewPlayer)
{
    _this->bStartPlayersAsSpectators = false;
    NewPlayer->MatchID = kMatchId;
    NewPlayer->PlayerState->Cast<AShooterPlayerState>()->DesiredClass = FGuid(0xadd6443a, 0x41bde414, 0xf6ade58d, 0x267f4e95);
    Ares::Log("HandleStartingNewPlayer: %s", NewPlayer->GetName().c_str());
    HandleStartingNewPlayerOG(_this, NewPlayer);
}

UClass* GetDefaultPawnClassForController(AShooterGameMode* _this, AController* InController)
{
    static UClass* PawnClass = nullptr;
    if (!PawnClass)
    {
        PawnClass = FindObject<UClass>(Ares::Config().PawnClass);
        if (!PawnClass)
            Ares::LogW(L"pawn class not found: %s", Ares::Config().PawnClass.c_str());
    }
    return PawnClass;
}

// stock one asks the backend
float AuthGetPhaseRemainingTime(AShooterGameMode* _this)
{
    const float Elapsed = UGameplayStatics::GetTimeSeconds(UWorld::GetWorld());
    const float Remaining = 20.0f - Elapsed;
    return Remaining > 0.05f ? Remaining : 0.05f;
}

// nobody reports ready offline, load-in just times out
static void AllPlayersReady(UObject* Context, FFrame& Stack, bool* Result)
{
    Stack.IncrementCode();
    if (Result)
        *Result = true;
}

void ShooterGameMode::Init()
{
    AresBootLogLine("init: ShooterGameMode");
    Hooking::Hook<AShooterGameMode>(0x838 / 8, ReadyToStartMatch);
    Hooking::Hook<AShooterGameMode>(0x668 / 8, SpawnDefaultPawnFor);
    Hooking::Hook<AShooterGameMode>(0x698 / 8, HandleStartingNewPlayer, HandleStartingNewPlayerOG);
    Hooking::Hook(ImageBase + 0x19FFC00, AuthGetPhaseRemainingTime);
    Hooking::Hook<AShooterGameMode>(0x778 / 8, AGameMode::GetDefaultObj()->VTable[0x778 / 8]); // postlogin, fuck you riot..
    Hooking::Hook<AShooterGameMode>(0x7A8 / 8, AGameMode::GetDefaultObj()->VTable[0x7A8 / 8]); // restartplayer
    Hooking::Hook<AShooterGameMode>(0x6A8 / 8, GetDefaultPawnClassForController);

    UClass* ModeClass = AShooterGameMode::StaticClass();
    Hooking::ExecHook(ModeClass->FindFunction("AreAllPlayersReadyToStart"), AllPlayersReady);
    Hooking::ExecHook(ModeClass->FindFunction("AreAllPlayersResponsive"), AllPlayersReady);
}

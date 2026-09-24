#include <pch.h>
#include <Headers/Modules/Freecam.h>
#include <Headers/Modules/ShooterGame/LocalBots.h>
#include <chrono>
#include <atomic>

namespace
{
    TWeakObjectPtr<ACameraActor> Camera;
    TWeakObjectPtr<APawn> Body;
    TWeakObjectPtr<APlayerController> Owner;
    float Speed = 1200.f;
    float Sensitivity = 0.035f;  // deg per count, 0.07 * 0.5
    double LastLookFrame = -1;
    long long LookFrames = 0, LookNonZero = 0, LookUnfocused = 0; double LookSumX = 0, LookSumY = 0;
    FRotator BodyAim{};
    std::vector<TWeakObjectPtr<USceneComponent>> HiddenFirstPerson;   // 1p arms and gun

    void HideFirstPerson(APawn* Pawn, bool Hide)
    {
        if (Hide)
        {
            HiddenFirstPerson.clear();
            for (auto Component : Pawn->GetComponentsByClass(UPrimitiveComponent::StaticClass()))
            {
                auto Scene = Component ? Component->Cast<USceneComponent>() : nullptr;
                if (!Scene || Scene->GetName().find("1P") == std::string::npos || !Scene->IsVisible()) continue;
                Scene->SetHiddenInGame(true, true); Scene->SetVisibility(false, true);
                HiddenFirstPerson.push_back(Scene);
            }
            // gun is its own attached actor
            TArray<AActor*> Attached{};
            Pawn->GetAttachedActors(&Attached);
            for (auto Actor : Attached)
                if (Actor) for (auto Component : Actor->GetComponentsByClass(UPrimitiveComponent::StaticClass()))
                {
                    auto Scene = Component ? Component->Cast<USceneComponent>() : nullptr;
                    if (!Scene || Scene->GetName().find("1P") == std::string::npos || !Scene->IsVisible()) continue;
                    Scene->SetHiddenInGame(true, true); Scene->SetVisibility(false, true);
                    HiddenFirstPerson.push_back(Scene);
                }
            std::string Names;
            for (auto& Weak : HiddenFirstPerson) if (auto Scene = Weak.Get()) Names += " " + Scene->GetName();
            Ares::Log("freecam: hid %zu first-person part(s):%s (%d attached actors)", HiddenFirstPerson.size(), Names.c_str(), Attached.Num());
            Attached.Free();
        }
        else
        {
            for (auto& Weak : HiddenFirstPerson) if (auto Scene = Weak.Get()) { Scene->SetHiddenInGame(false, true); Scene->SetVisibility(true, true); }
            HiddenFirstPerson.clear();
        }
    }
    bool BodyUsedYaw = true;
    std::chrono::steady_clock::time_point Last;
    bool KeyWasDown = false;

    bool Down(int Key) { return (GetAsyncKeyState(Key) & 0x8000) != 0; }

    std::atomic<long long> RawX{0}, RawY{0}, RawEvents{0};
    long long UsedX = 0, UsedY = 0;
    UINT (WINAPI* GetRawInputDataOG)(HRAWINPUT, UINT, LPVOID, PUINT, UINT) = nullptr;
    UINT (WINAPI* GetRawInputBufferOG)(PRAWINPUT, PUINT, UINT) = nullptr;

    void CountMouse(const RAWINPUT* Input)
    {
        if (!Input || Input->header.dwType != RIM_TYPEMOUSE) return;
        if (Input->data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE) return;
        RawX += Input->data.mouse.lLastX; RawY += Input->data.mouse.lLastY; ++RawEvents;
    }

    UINT WINAPI GetRawInputDataDetour(HRAWINPUT Handle, UINT Command, LPVOID Data, PUINT Size, UINT HeaderSize)
    {
        const UINT Result = GetRawInputDataOG(Handle, Command, Data, Size, HeaderSize);
        if (Command == RID_INPUT && Data && Result != (UINT)-1 && Result >= sizeof(RAWINPUTHEADER)) CountMouse((const RAWINPUT*)Data);
        return Result;
    }

    UINT WINAPI GetRawInputBufferDetour(PRAWINPUT Data, PUINT Size, UINT HeaderSize)
    {
        const UINT Count = GetRawInputBufferOG(Data, Size, HeaderSize);
        if (Data && Count != (UINT)-1)
        {
            const RAWINPUT* Input = Data;
            for (UINT I = 0; I < Count && Input; ++I)
            {
                CountMouse(Input);
                // NEXTRAWINPUTBLOCK
                Input = (const RAWINPUT*)(((uintptr_t)Input + Input->header.dwSize + 7) & ~(uintptr_t)7);
            }
        }
        return Count;
    }

    bool GameFocused()
    {
        HWND Window = GetForegroundWindow();
        DWORD Pid = 0;
        GetWindowThreadProcessId(Window, &Pid);
        char Class[64] = {};
        GetClassNameA(Window, Class, sizeof Class);
        return Pid == GetCurrentProcessId() && strcmp(Class, "UnrealWindow") == 0;
    }

    std::string Enter()
    {
        auto World = UWorld::GetWorld();
        auto PC = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
        if (!PC || !PC->Pawn) return "freecam: no player";
        FVector Eye{}; FRotator Rot{};
        PC->Pawn->GetActorEyesViewPoint(&Eye, &Rot);
        auto Spawned = SpawnActor(ACameraActor::StaticClass(), Eye, PC->GetControlRotation(), nullptr,
                                  ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
        auto Cam = Spawned ? Spawned->Cast<ACameraActor>() : nullptr;
        if (!Cam) return "freecam: could not spawn a camera";
        Camera = Cam; Body = PC->Pawn; Owner = PC;
        PC->SetIgnoreMoveInput(true);
        PC->Pawn->DisableInput(PC);
        BodyAim = PC->GetControlRotation();
        BodyUsedYaw = PC->Pawn->bUseControllerRotationYaw;
        PC->Pawn->bUseControllerRotationYaw = 0;
        HideFirstPerson(PC->Pawn, true);
        PC->SetViewTargetWithBlend(Cam, 0.15f, EViewTargetBlendFunction::VTBlend_Linear, 0.f, false);
        Last = std::chrono::steady_clock::now();
        return "freecam: on - WASD to fly, Space/Ctrl up/down, Shift faster, mouse or arrow keys to look; your body stays put, takes no input and is still a target. "
               "'freecam' or F6 to return.";
    }

    std::string Leave()
    {
        auto PC = Owner.Get();
        if (PC)
        {
            PC->SetIgnoreMoveInput(false);
            if (auto Pawn = Body.Get())
            {
                Pawn->EnableInput(PC);
                HideFirstPerson(Pawn, false);
                Pawn->bUseControllerRotationYaw = BodyUsedYaw;
                PC->SetControlRotation(BodyAim);
            }
            if (auto Pawn = Body.Get()) PC->SetViewTargetWithBlend(Pawn, 0.15f, EViewTargetBlendFunction::VTBlend_Linear, 0.f, false);
            else if (PC->Pawn) PC->SetViewTargetWithBlend(PC->Pawn, 0.f, EViewTargetBlendFunction::VTBlend_Linear, 0.f, false);
        }
        HiddenFirstPerson.clear();
        if (auto Cam = Camera.Get()) Cam->K2_DestroyActor();
        Camera = nullptr; Body = nullptr; Owner = nullptr;
        return "freecam: off - back in the body";
    }
}

bool Freecam::Active() { return Camera.Get() != nullptr; }

void Freecam::Install()
{
    HMODULE User = GetModuleHandleA("user32.dll");
    if (!User) return;
    if (auto Data = GetProcAddress(User, "GetRawInputData")) Hooking::Hook(Data, &GetRawInputDataDetour, GetRawInputDataOG);
    if (auto Buffer = GetProcAddress(User, "GetRawInputBuffer")) Hooking::Hook(Buffer, &GetRawInputBufferDetour, GetRawInputBufferOG);
    Ares::Log("freecam: watching raw mouse input (GetRawInputData %s, GetRawInputBuffer %s)",
              GetRawInputDataOG ? "hooked" : "missing", GetRawInputBufferOG ? "hooked" : "missing");
}

std::string Freecam::Command(const std::string& Argument)
{
    if (Argument.rfind("sens", 0) == 0)
    {
        const float Value = (float)atof(Argument.c_str() + 4);
        if (Value > 0) Sensitivity = std::clamp(Value, 0.005f, 2.f);
        return "freecam: mouse sensitivity " + std::to_string(Sensitivity) + " degrees per count ('freecam sens <n>')";
    }
    if (Argument == "mouse")
    {
        char Buffer[256];
        snprintf(Buffer, sizeof Buffer, "freecam: mouse - %lld frames, %lld with a delta (sum %.1f, %.1f), %lld unfocused, %lld raw mouse events, sensitivity %.3f",
                 LookFrames, LookNonZero, LookSumX, LookSumY, LookUnfocused, RawEvents.load(), Sensitivity);
        return Buffer;
    }
    if (Argument == "speed") return "freecam: speed " + std::to_string((int)Speed) + " units/s (use 'freecam <number>')";
    if (!Argument.empty() && isdigit((unsigned char)Argument[0]))
    {
        Speed = std::clamp((float)atof(Argument.c_str()), 100.f, 10000.f);
        return "freecam: speed " + std::to_string((int)Speed);
    }
    if (Argument.rfind("at ", 0) == 0)
    {
        float X = 0, Y = 0, Z = 0, Yaw = 0, Pitch = -20;
        const int Read = sscanf(Argument.c_str() + 3, "%f %f %f %f %f", &X, &Y, &Z, &Yaw, &Pitch);
        if (Read < 3) return "freecam: at x y z [yaw pitch]";
        std::string Entered = Active() ? "" : Enter();
        auto Cam = Camera.Get(); auto PC = Owner.Get();
        if (!Cam || !PC) return Entered.empty() ? "freecam: not active" : Entered;
        if (Read < 4) Yaw = PC->GetControlRotation().Yaw;
        const FRotator Rot{Pitch, Yaw, 0};
        PC->SetControlRotation(Rot);
        FHitResult Hit{};
        Cam->K2_SetActorLocationAndRotation(FVector{X, Y, Z}, Rot, false, &Hit, true);
        char Buffer[128];
        snprintf(Buffer, sizeof Buffer, "freecam: at %.0f %.0f %.0f yaw %.0f pitch %.0f", X, Y, Z, Yaw, Pitch);
        return Buffer;
    }
    if (Argument == "off") return Active() ? Leave() : "freecam: already off";
    if (Argument == "on") return Active() ? "freecam: already on" : Enter();
    return Active() ? Leave() : Enter();
}

void Freecam::Tick()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    {
        static auto LastRun = std::chrono::steady_clock::time_point{};
        const auto Wall = std::chrono::steady_clock::now();
        if (Wall - LastRun < std::chrono::milliseconds(4)) return;
        LastRun = Wall;
    }
    const bool Key = GameFocused() && Down(VK_F6);
    if (Key && !KeyWasDown) Ares::Log("%s", Command("").c_str());
    KeyWasDown = Key;

    auto Cam = Camera.Get();
    if (!Cam) return;
    auto PC = Owner.Get();
    // died or map changed
    if (!PC || UWorld::GetWorld() == nullptr || PC->Pawn != Body.Get()) { Ares::Log("%s", Leave().c_str()); return; }

    const auto Now = std::chrono::steady_clock::now();
    const float Dt = std::min(0.05f, std::chrono::duration<float>(Now - Last).count());
    if (Dt < 0.004f) return;
    Last = Now;

    // once per game frame, tick runs many times per frame
    if (auto World = UWorld::GetWorld())
    {
        const double Frame = UGameplayStatics::GetTimeSeconds(World);
        if (Frame != LastLookFrame)
        {
            LastLookFrame = Frame;
            float DX = 0, DY = 0;
            PC->GetInputMouseDelta(&DX, &DY);
            // raw input wins if there is any, y flipped
            const long long RX = RawX.load(), RY = RawY.load();
            if (RX != UsedX || RY != UsedY) { DX = (float)(RX - UsedX); DY = (float)-(RY - UsedY); }
            UsedX = RX; UsedY = RY;
            ++LookFrames;
            if (DX != 0 || DY != 0) { ++LookNonZero; LookSumX += DX; LookSumY += DY; }
            if (!GameFocused()) ++LookUnfocused;
            if (GameFocused() && (DX != 0 || DY != 0))
            {
                FRotator Turn = PC->GetControlRotation();
                Turn.Yaw += DX * Sensitivity;
                float Pitch = Turn.Pitch > 180.f ? Turn.Pitch - 360.f : Turn.Pitch;
                Turn.Pitch = std::clamp(Pitch + DY * Sensitivity, -89.f, 89.f);
                PC->SetControlRotation(Turn);
            }
        }
    }
    if (GameFocused() && (Down(VK_LEFT) || Down(VK_RIGHT) || Down(VK_UP) || Down(VK_DOWN)))
    {
        FRotator Turn = PC->GetControlRotation();
        const float Rate = 90.f * Dt;
        if (Down(VK_LEFT)) Turn.Yaw -= Rate;
        if (Down(VK_RIGHT)) Turn.Yaw += Rate;
        if (Down(VK_UP)) Turn.Pitch += Rate;
        if (Down(VK_DOWN)) Turn.Pitch -= Rate;
        PC->SetControlRotation(Turn);
    }
    const FRotator Rot = PC->GetControlRotation();
    const float Yaw = Rot.Yaw * 3.14159265f / 180.f, Pitch = Rot.Pitch * 3.14159265f / 180.f;
    const FVector Forward{cosf(Pitch) * cosf(Yaw), cosf(Pitch) * sinf(Yaw), sinf(Pitch)};
    const FVector Right{-sinf(Yaw), cosf(Yaw), 0.f};
    FVector Move{};
    if (GameFocused())
    {
        if (Down('W')) Move = Move + Forward;
        if (Down('S')) Move = Move - Forward;
        if (Down('D')) Move = Move + Right;
        if (Down('A')) Move = Move - Right;
        if (Down(VK_SPACE)) Move.Z += 1.f;
        if (Down(VK_CONTROL)) Move.Z -= 1.f;
    }
    const float Fast = Down(VK_SHIFT) ? 3.f : 1.f;
    FVector At = Cam->K2_GetActorLocation();
    At = At + Move * (Speed * Fast * Dt);
    FHitResult Hit{};
    Cam->K2_SetActorLocationAndRotation(At, Rot, false, &Hit, true);
}

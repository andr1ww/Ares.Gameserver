#include <pch.h>

namespace GameplayDiagnostics { void Init(); }
INIT_MODULE(GameplayDiagnostics);

namespace
{
    using HiddenFn = void (*)(AActor*, bool, const FName*);
    HiddenFn HiddenOG = nullptr;
    void Hidden(AActor* Actor, bool Hide, const FName* Tag)
    {
        static int Count = 0;
        if (Actor && Count < 100 && Actor->Class->GetName().find("PossessableCamera") != std::string::npos)
        {
            ++Count;
            auto Pawn = Actor->Cast<APawn>();
            Ares::Log("gameplay-trace: camera hidden=%d tag=%s controller=%s local=%d",
                (int)Hide, Tag->ToString().c_str(),
                Pawn && Pawn->Controller ? Pawn->Controller->GetName().c_str() : "none",
                Pawn ? (int)Pawn->IsLocallyControlled() : -1);
        }
        HiddenOG(Actor, Hide, Tag);
    }
}

void GameplayDiagnostics::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone || !GetEnvironmentVariableA("ARES_TRACE_GAMEPLAY", nullptr, 0)) return;
    Hooking::Hook(ImageBase + 0x3254280, Hidden, HiddenOG);
    Ares::Log("gameplay-trace: bounded damage and camera visibility tracing enabled");
}

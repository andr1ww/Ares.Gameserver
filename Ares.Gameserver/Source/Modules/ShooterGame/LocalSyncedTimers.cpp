#include <pch.h>

// AuthStartTimer is stripped (just a ret), curveball hung in the air forever

namespace LocalSyncedTimers { void Init(); }
INIT_MODULE(LocalSyncedTimers);

namespace
{
    void (*AuthStartTimerOG)(UObject*, FFrame&, void*) = nullptr;

    // same clock GetTimeRemaining uses
    float Now(USyncedTimerComponent* Timer)
    {
        static auto Clock = (float (*)(USyncedTimerComponent*))(ImageBase + 0x18F6080);
        return Clock(Timer);
    }

    void AuthStartTimer(UObject* Context, FFrame& Stack, void* Result)
    {
        // still needed for P_FINISH
        AuthStartTimerOG(Context, Stack, Result);
        auto Timer = Context ? Context->Cast<USyncedTimerComponent>() : nullptr;
        if (!Timer) return;
        Timer->MulticastSetNetExpireTime(Now(Timer) + Timer->TimerDuration);
    }
}

void LocalSyncedTimers::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Hooking::Hook(ImageBase + 0x16AC1F0, AuthStartTimer, AuthStartTimerOG);
}

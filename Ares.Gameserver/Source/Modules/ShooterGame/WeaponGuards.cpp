#include <pch.h>
#include <Headers/Modules/ShooterGame/WeaponGuards.h>
INIT_MODULE(WeaponGuards);

namespace
{
    constexpr uintptr_t kEquippableNotify = 0x1E0F5F0; // derefs this first thing, callers never null check

    void (*NotifyOG)(void*) = nullptr;

    void NotifyGuard(void* This)
    {
        if (!This)
        {
            static bool bReported = false;
            if (!bReported)
            {
                bReported = true;
                Ares::Log("weapons: skipped an equippable notify with no object "
                          "(this is what used to crash rifles)");
            }
            return;
        }
        NotifyOG(This);
    }
} // namespace

void WeaponGuards::Init()
{
    Hooking::Hook(ImageBase + kEquippableNotify, &NotifyGuard, NotifyOG);
    Ares::Log("weapons: null guard installed at client+0x%llX",
              (unsigned long long)kEquippableNotify);
}

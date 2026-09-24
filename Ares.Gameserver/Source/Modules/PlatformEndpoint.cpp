#include <pch.h>
#include <Headers/Modules/PlatformEndpoint.h>

namespace
{
    // only these call sites, the string helpers are used everywhere
    constexpr uintptr_t kValidationCallSites[] = {
        0x1AAA9B8, // Contains(".a.pvp.net")
        0x1AAAA74, // StartsWith("https://shared.")
        0x1AAAB56, // same two for -config-endpoint=
        0x1AAAC15,
    };

    bool AlwaysValid(void* Self, void* Needle, int Flags)
    {
        return true;
    }
} // namespace

void PlatformEndpoint::Install()
{
    // operand addr, not the E8
    for (uintptr_t Site : kValidationCallSites)
        Hooking::Rel32Hook(ImageBase + Site + 1, AlwaysValid);

    Ares::Log("platform endpoint: accepted a local config endpoint (%d call site(s) patched)",
              (int)(sizeof(kValidationCallSites) / sizeof(kValidationCallSites[0])));
}

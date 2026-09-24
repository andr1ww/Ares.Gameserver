#include <pch.h>
#include <Headers/Modules/VanguardBypass.h>

namespace
{
    constexpr uintptr_t kBlockingWaitCall = 0x188EF30; // call [rip+x] WaitForSingleObject
    constexpr size_t kCallLength = 6;

    // xor eax, eax + nops, WAIT_OBJECT_0
    constexpr uint8_t kReturnZero[kCallLength] = { 0x31, 0xC0, 0x90, 0x90, 0x90, 0x90 };

    bool WriteBytes(uintptr_t Address, const uint8_t* Bytes, size_t Length)
    {
        DWORD Old = 0;
        if (!VirtualProtect((void*)Address, Length, PAGE_EXECUTE_READWRITE, &Old))
            return false;
        memcpy((void*)Address, Bytes, Length);
        VirtualProtect((void*)Address, Length, Old, &Old);
        FlushInstructionCache(GetCurrentProcess(), (void*)Address, Length);
        return true;
    }
} // namespace

void VanguardBypass::Install()
{
    uint8_t* const Site = (uint8_t*)(ImageBase + kBlockingWaitCall);

    // 1.08 only
    if (Site[0] != 0xFF || Site[1] != 0x15)
    {
        Ares::Log("vanguard: unexpected bytes at client+0x%llX (%02X %02X), not patching",
                  (unsigned long long)kBlockingWaitCall, Site[0], Site[1]);
        return;
    }

    if (!WriteBytes((uintptr_t)Site, kReturnZero, kCallLength))
    {
        Ares::Log("vanguard: failed to patch client+0x%llX", (unsigned long long)kBlockingWaitCall);
        return;
    }

    Ares::Log("vanguard: neutralised the blocking wait at client+0x%llX",
              (unsigned long long)kBlockingWaitCall);
}

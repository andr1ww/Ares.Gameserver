#ifndef PCH_H
#define PCH_H

#include "framework.h"

#if !defined(__int64) && !defined(_MSC_VER)
#define __int64 long long
#endif

#include <thread>
#include <algorithm>
#include <numeric>
#include <bit>

inline uint64_t ImageBase = *(uint64_t*)(__readgsqword(0x60) + 0x10);

#include "SDK/Basic.hpp"
#include "SDK/CoreUObject_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/ShooterGame_classes.hpp"
#include "SDK/GameplayAbilities_classes.hpp"
#include "SDK/UMG_classes.hpp"
#include "SDK/AssetRegistry_structs.hpp"
#include "SDK/AssetRegistry_classes.hpp"

using namespace UC;
using namespace SDK;

#include "Headers/Shared.hpp"
#include "Headers/AresRuntime.hpp"
#include "Headers/Reentrancy.hpp"

void AresBootLogLine(const char* Line);

#endif

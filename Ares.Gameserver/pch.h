#ifndef PCH_H
#define PCH_H

#include "framework.h"
#include <thread>
#include <algorithm>
#include <numeric>

inline uint64_t ImageBase = *(uint64_t*)(__readgsqword(0x60) + 0x10);

#include "SDK/Basic.hpp"
#include "SDK/CoreUObject_classes.hpp"
#include "SDK/Engine_classes.hpp"
#include "SDK/ShooterGame_classes.hpp"
#include "SDK/GameplayAbilities_classes.hpp"

using namespace UC;
using namespace SDK;

#include "Headers/Shared.hpp"

#endif

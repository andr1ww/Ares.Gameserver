#pragma once
#include <string>

namespace Freecam
{
    std::string Command(const std::string& Argument);
    bool Active();
    void Tick();
    void Install();
}

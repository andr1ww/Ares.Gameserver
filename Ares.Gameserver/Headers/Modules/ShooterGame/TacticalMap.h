#pragma once
#include <string>
#include <Headers/Modules/ShooterGame/TacticalGraph.h>

namespace TacticalMap
{
    const TacticalGraph* Get();
    std::string StartBake();
    std::string Status();
    // x y z x y z
    std::string Explain(const std::string& Arguments);
    void Tick();
}

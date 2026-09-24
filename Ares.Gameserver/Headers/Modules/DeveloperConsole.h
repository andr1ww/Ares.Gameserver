#pragma once
#include <string>

class DeveloperConsole
{
public:
    static void Start();
    static void Tick();
    static void Run(const std::string& Line); // game thread only

    static bool SpyArmed();
    static void SpyRecord(class UObject* Object, class UFunction* Function);
};

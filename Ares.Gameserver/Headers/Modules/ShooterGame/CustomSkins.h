#pragma once
#include <string>
#include <vector>
namespace SDK { class AAresEquippable; class UObject; class UFunction; }
namespace CustomSkins
{
    struct FDefinition { const char* Name; const char* Weapon; const char* Label; const char* Note; };
    const std::vector<FDefinition>& All();
    const FDefinition* Find(const std::string& Name, const std::string& Weapon, std::string& Why);
    bool Apply(SDK::AAresEquippable*& Weapon, const FDefinition& Skin, std::string& Report);
    void Clear(SDK::AAresEquippable* Weapon);
    void Tick();
    bool IsActive(SDK::AAresEquippable* Weapon);
    bool BeforeDispatch(SDK::UObject* Object, SDK::UFunction* Function, void* Parameters);
    void AfterDispatch(SDK::UObject* Object, SDK::UFunction* Function, void* Parameters);
    void OnKill(SDK::AAresEquippable* Weapon);
    std::vector<std::string> Describe();
    void PreviewMaterial(const std::string& Mode);
}

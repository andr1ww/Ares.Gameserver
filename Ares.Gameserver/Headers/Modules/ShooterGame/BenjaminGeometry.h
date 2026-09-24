#pragma once
#include <filesystem>
#include <string>
#include <vector>
#include <SDK/ProceduralMeshComponent_classes.hpp>

namespace BenjaminGeometry
{
    struct FPart
    {
        std::string Name;
        int Material, Motion;
        SDK::FVector Pivot;
        UC::TArray<SDK::FVector> Vertices, Normals;
        UC::TArray<SDK::FVector2D> UV;
        UC::TArray<int32> Triangles;
    };
    bool Load(const std::filesystem::path& Folder,std::string& Error);
    const std::vector<FPart>& Parts();
    SDK::UProceduralMeshComponent* Create(SDK::AAresEquippable* Weapon,const FPart& Part);
}

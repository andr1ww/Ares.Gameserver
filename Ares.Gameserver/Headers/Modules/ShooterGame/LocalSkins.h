#pragma once
#include <string>
#include <vector>

namespace LocalSkins
{
    struct FEntry
    {
        std::string Name;
        std::string Folder;
        std::wstring Path;
    };

    const std::vector<FEntry>& Catalogue();

    std::vector<const FEntry*> For(const std::wstring& WeaponFolder);

    // level 0 = max
    bool Apply(class AAresEquippable*& Weapon, const std::wstring& WeaponFolder,
               const std::string& Wanted, int Level, std::string& Report);

    // fresh gun, ClearWeaponComponents leaves the skin mesh on
    class AAresEquippable* Replace(class AAresEquippable* Weapon);

    std::string Worn(class AAresEquippable* Weapon);

    void ListFolder(const std::string& PackagePath, bool bRecursive, int Limit);

    std::wstring FolderOf(const wchar_t* AssetPath);
}

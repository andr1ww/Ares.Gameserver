#include <pch.h>
#include <Headers/Modules/LocalSettings.h>
#include <filesystem>
#include <sstream>
#include <iomanip>
#include <cmath>

namespace
{
    std::wstring Path;
    UAresSettingsManager* LoadedManager = nullptr;
    std::string LastSaved;

    std::wstring Key(wchar_t Type, int Id) { return std::wstring(1, Type) + L"." + std::to_wstring(Id); }

    bool Read(wchar_t Type, int Id, std::wstring& Value)
    {
        wchar_t Buffer[16384];
        GetPrivateProfileStringW(L"Settings", Key(Type, Id).c_str(), L"\x1", Buffer, 16384, Path.c_str());
        if (Buffer[0] == 1 && Buffer[1] == 0) return false;
        Value = Buffer;
        return true;
    }

    std::string Encode(const FString& Value)
    {
        static const char Hex[] = "0123456789ABCDEF";
        std::string Result;
        for (wchar_t Ch : Value.ToWString())
            for (int Shift : {12, 8, 4, 0}) Result += Hex[(Ch >> Shift) & 15];
        return Result;
    }

    bool Decode(const std::wstring& Text, std::wstring& Value)
    {
        if (Text.size() % 4) return false;
        Value.clear();
        for (size_t I = 0; I < Text.size(); I += 4)
        {
            unsigned Ch = 0;
            for (size_t J = I; J < I + 4; ++J)
            {
                wchar_t C = Text[J];
                unsigned N = C >= L'0' && C <= L'9' ? C - L'0' : C >= L'A' && C <= L'F' ? C - L'A' + 10 : 16;
                if (N > 15) return false;
                Ch = (Ch << 4) | N;
            }
            if (!Ch) return false;
            Value += (wchar_t)Ch;
        }
        return true;
    }

    void Restore(UAresSettingsManager* Manager)
    {
        int Count = 0;
        std::wstring Text;
        wchar_t* End = nullptr;
        for (auto& Pair : Manager->FloatSettings)
        {
            auto& Setting = Pair.Value();
            if (!Read(L'F', (int)Setting.SettingEnum, Text) || Text.empty()) continue;
            float Value = wcstof(Text.c_str(), &End);
            if (*End || !std::isfinite(Value) || Value < Setting.MinValue || Value > Setting.MaxValue) continue;
            Manager->SetFloatSetting(Setting.SettingEnum, Value, false);
            ++Count;
        }
        for (auto& Pair : Manager->IntSettings)
        {
            auto& Setting = Pair.Value();
            if (!Read(L'I', (int)Setting.SettingEnum, Text) || Text.empty()) continue;
            long Value = wcstol(Text.c_str(), &End, 10);
            if (*End || Value < Setting.MinValue || Value > Setting.MaxValue) continue;
            Manager->SetIntSetting(Setting.SettingEnum, (int32)Value, false);
            ++Count;
        }
        for (auto& Pair : Manager->BoolSettings)
        {
            auto& Setting = Pair.Value();
            if (!Read(L'B', (int)Setting.SettingEnum, Text) || (Text != L"0" && Text != L"1")) continue;
            Manager->SetBoolSetting(Setting.SettingEnum, Text == L"1", false);
            ++Count;
        }
        for (auto& Pair : Manager->StringSettings)
        {
            auto& Setting = Pair.Value();
            std::wstring Value;
            if (!Read(L'S', (int)Setting.SettingEnum, Text) || !Decode(Text, Value)) continue;
            Manager->SetStringSetting(Setting.SettingEnum, FString(Value.c_str()), false);
            ++Count;
        }
        Ares::Log("settings: restored %d offline preference(s)", Count);
    }

    std::string Snapshot(UAresSettingsManager* Manager)
    {
        std::ostringstream Out;
        Out.imbue(std::locale::classic());
        Out << "; VALORANT 1.08 offline preferences. String values are UTF-16 hex.\n[Settings]\n";
        Out << std::setprecision(9);
        for (auto& Pair : Manager->FloatSettings) Out << "F." << (int)Pair.Value().SettingEnum << '=' << Pair.Value().Value << '\n';
        for (auto& Pair : Manager->IntSettings) Out << "I." << (int)Pair.Value().SettingEnum << '=' << Pair.Value().Value << '\n';
        for (auto& Pair : Manager->BoolSettings) Out << "B." << (int)Pair.Value().SettingEnum << '=' << (int)Pair.Value().Value << '\n';
        for (auto& Pair : Manager->StringSettings) Out << "S." << (int)Pair.Value().SettingEnum << '=' << Encode(Pair.Value().Value) << '\n';
        return Out.str();
    }

    bool Save(const std::string& Data)
    {
        const std::wstring Temp = Path + L".tmp";
        HANDLE File = CreateFileW(Temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (File == INVALID_HANDLE_VALUE) return false;
        DWORD Written = 0;
        bool Ok = WriteFile(File, Data.data(), (DWORD)Data.size(), &Written, nullptr) && Written == Data.size();
        if (Ok) Ok = FlushFileBuffers(File);
        CloseHandle(File);
        return Ok && MoveFileExW(Temp.c_str(), Path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
}

void LocalSettings::Tick()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    static bool Busy = false;
    FReentrancyGuard Guard(Busy);
    if (!Guard.Entered()) return;
    static double LastCheck = -1;
    double Now = Ares::Seconds();
    if (Now - LastCheck < 0.25) return;
    LastCheck = Now;
    auto Engine = UEngine::GetEngine();
    auto UserSettings = Engine && Engine->GameUserSettings ? Engine->GameUserSettings->Cast<UShooterGameUserSettings>() : nullptr;
    auto Manager = UserSettings ? UserSettings->AresSettingsManager : nullptr;
    if (!Manager || !Manager->FloatSettings.Num() || !Manager->StringSettings.Num()) return;
    if (Path.empty())
    {
        auto Saved = UKismetSystemLibrary::GetProjectSavedDirectory().ToWString();
        if (Saved.empty()) return;
        auto Destination = std::filesystem::absolute(std::filesystem::path(Saved) / L"Config" / L"Windows" / L"AresOfflineSettings.ini");
        std::error_code Error;
        std::filesystem::create_directories(Destination.parent_path(), Error);
        if (Error) return;
        Path = Destination.wstring();
        Ares::LogW(L"settings: offline preferences %s", Path.c_str());
    }
    if (LoadedManager != Manager)
    {
        Restore(Manager);
        LoadedManager = Manager;
        LastSaved.clear();
    }
    auto Current = Snapshot(Manager);
    if (Current == LastSaved) return;
    if (Save(Current))
    {
        LastSaved = std::move(Current);
        Ares::Log("settings: saved offline preferences");
    }
}

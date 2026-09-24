#include <pch.h>
#include <Headers/Modules/ShooterGame/LocalTeams.h>
#include <unordered_map>

// team setters are stripped, answer GetTeamComponent ourselves

namespace LocalTeams { void Init(); }
INIT_MODULE(LocalTeams);

namespace
{
    UBaseTeamComponent* (*GetTeamComponentOG)(const UObject*) = nullptr;
    std::unordered_map<const UObject*, UBaseTeamComponent*> Assignments;

    UBaseTeamComponent* Lookup(const UObject* Source)
    {
        if (!Source || Assignments.empty()) return nullptr;
        auto Found = Assignments.find(Source);
        return Found == Assignments.end() ? nullptr : Found->second;
    }

    UBaseTeamComponent* GetTeamComponent(const UObject* Source)
    {
        if (auto Direct = Lookup(Source)) return Direct;

        auto Answer = GetTeamComponentOG(Source);
        if (Answer && Answer->HasValidTeam()) return Answer;

        // projectiles/pings get their team from whoever spawned them
        auto Object = const_cast<UObject*>(Source);
        AActor* Actor = Object ? Object->Cast<AActor>() : nullptr;
        if (!Actor)
            if (auto Component = Object ? Object->Cast<UActorComponent>() : nullptr)
                Actor = Component->GetOwner();
        for (int Depth = 0; Actor && Depth < 8; ++Depth)
        {
            AActor* Next = Actor->Owner ? Actor->Owner : (AActor*)Actor->Instigator;
            if (!Next || Next == Actor) break;
            if (auto Inherited = Lookup(Next)) return Inherited;
            Actor = Next;
        }
        return Answer;
    }
}

namespace
{
    // fog/outline cache the team at spawn, enemies kept the ally tint
    void NotifyTeamChanged(UObject* Object)
    {
        if (!Object) return;
        auto Call = [](UObject* Target)
        {
            auto Function = Target->Class->FindFunction("OnTeamChanged");
            if (!Function || Function->Size > 0) return;
            uint8 Nothing = 0;
            Target->ProcessEvent(Function, &Nothing);
        };
        Call(Object);
        auto Actor = Object->Cast<AActor>();
        if (!Actor) return;
        auto Components = Actor->GetComponentsByClass(UActorComponent::StaticClass());
        for (auto Component : Components)
            if (Component) Call(Component);
        Components.Free();
    }
}

void LocalTeams::Assign(UObject* Actor, UBaseTeamComponent* Team)
{
    if (!Actor) return;
    if (!Team) { Assignments.erase(Actor); return; }
    Assignments[Actor] = Team;
    // pawn, controller and player state all get asked
    if (auto Pawn = Actor->Cast<APawn>())
    {
        if (Pawn->Controller) Assignments[Pawn->Controller] = Team;
        if (Pawn->PlayerState) Assignments[Pawn->PlayerState] = Team;
    }
    NotifyTeamChanged(Actor);
}

UBaseTeamComponent* LocalTeams::Assigned(UObject* Actor)
{
    if (!Actor) return nullptr;
    auto Found = Assignments.find(Actor);
    return Found == Assignments.end() ? nullptr : Found->second;
}

void LocalTeams::Init()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone) return;
    Hooking::Hook(ImageBase + 0x1CCC550, GetTeamComponent, GetTeamComponentOG);
}

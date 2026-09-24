#pragma once
#include <algorithm>
#include <cmath>
#include <cstring>
namespace BotPolicy
{
    // csknow SecondOrderController port (MIT), caller unwraps angles
    struct AimAxis
    {
        float Velocity=0;
        float Update(float dt,float error,float maxRate=240.f)
        {
            if(dt<=0)return 0;
            constexpr float k1=0.9f/(3.14159265f*4.f);
            constexpr float k2=1.f/(8.f*3.14159265f*8.f*3.14159265f);
            float stable=std::max(k2,1.1f*(dt*dt/4+dt*k1/2));
            float step=Velocity*dt;
            Velocity=std::clamp(Velocity+(error-step-Velocity*k1)*dt/stable,-maxRate,maxRate);
            return std::clamp(step,-maxRate*dt,maxRate*dt);
        }
    };

    struct Skill
    {
        const char* Name = "normal";
        float Reaction = 0.27f;      // secs
        float TurnRate = 300.f;      // deg/s
        float Error = 1.6f;          // deg
        float Settle = 0.28f;        // secs
        float HeadRatio = 0.45f;
        float Discipline = 0.7f;
    };

    inline Skill SkillFor(const char* Name)
    {
        if (!std::strcmp(Name, "easy"))   return {"easy",   0.42f, 170.f, 3.2f, 0.45f, 0.20f, 0.35f};
        if (!std::strcmp(Name, "hard"))   return {"hard",   0.22f, 380.f, 1.0f, 0.20f, 0.65f, 0.90f};
        if (!std::strcmp(Name, "pro"))    return {"pro",    0.17f, 480.f, 0.6f, 0.15f, 0.80f, 1.00f};
        return {};
    }

    enum class Gun { Rifle, Burst, Smg, Shotgun, Sniper, Marksman, Pistol, Heavy, Other };

    struct GunProfile
    {
        Gun Kind = Gun::Rifle;
        float Interval = 0.103f;     // vandal
        bool Automatic = true;
        float Recovery = 0.32f;
    };

    inline GunProfile ProfileFor(const char* ClassName)
    {
        auto Has = [&](const char* S) { return ClassName && std::strstr(ClassName, S); };
        if (Has("AssaultRifle_AK")) return {Gun::Rifle, 0.1026f, true, 0.33f};
        if (Has("AssaultRifle_ACR")) return {Gun::Rifle, 0.0909f, true, 0.30f};
        if (Has("AssaultRifle_Burst")) return {Gun::Burst, 0.25f, false, 0.35f};
        if (Has("SubMachineGun") || Has("Vector")) return {Gun::Smg, 0.075f, true, 0.25f};
        if (Has("Shotgun")) return {Gun::Shotgun, 0.9f, false, 0.4f};
        if (Has("BoltSniper") || Has("LeverSniper")) return {Gun::Sniper, 1.5f, false, 0.2f};
        if (Has("DMR")) return {Gun::Marksman, 0.21f, false, 0.25f};
        if (Has("MachineGun")) return {Gun::Heavy, 0.083f, true, 0.4f};
        if (Has("Pistol")) return {Gun::Pistol, 0.2f, false, 0.25f};
        return {Gun::Other, 0.15f, false, 0.3f};
    }

    inline int BurstShots(const GunProfile& G, float Distance)
    {
        switch (G.Kind)
        {
        case Gun::Rifle:  return Distance > 2500 ? 1 : Distance > 1500 ? 2 : Distance > 900 ? 4 : 7;
        case Gun::Smg:    return Distance > 1800 ? 2 : Distance > 900 ? 4 : 8;
        case Gun::Heavy:  return Distance > 2000 ? 4 : 10;
        default:          return 1;
        }
    }
    inline double BurstSeconds(float distance) { return distance>2000 ? 0.06:0.23; }

    inline float FireTolerance(float Distance, bool Head)
    {
        const float Radius = Head ? 11.f : 22.f;
        return std::max(0.35f, std::atan2(Radius, std::max(50.f, Distance)) * 57.2958f * 1.3f);
    }

    inline bool StillEnough(float Speed) { return Speed < 70.f; }

    enum class Order { Auto, Hold, Push };
    enum class Action { Wait, Hold, Advance, Engage, Recover, Reload };
    struct Facts
    {
        bool Active=false, Enemy=false, Visible=false, AmmoKnown=false;
        int Ammo=0, Reserve=0;
        float Health=100;
        Order Intent=Order::Auto;
    };
    inline Action Decide(const Facts& f)
    {
        if (!f.Active || !f.Enemy) return Action::Wait;
        if (f.AmmoKnown && f.Ammo==0)
            return f.Visible ? Action::Recover : (f.Reserve>0 ? Action::Reload : Action::Hold);
        if (f.Visible && f.Health<30) return Action::Recover;
        if (f.Visible) return Action::Engage;
        if (f.AmmoKnown && f.Ammo<6 && f.Reserve>0) return Action::Reload;
        if (f.Intent==Order::Hold || (f.Intent==Order::Auto && f.Health<50)) return Action::Hold;
        return Action::Advance;
    }
    inline const char* Name(Action a)
    {
        switch(a) {
        case Action::Wait:return "wait"; case Action::Hold:return "hold";
        case Action::Advance:return "advance"; case Action::Engage:return "engage";
        case Action::Recover:return "recover"; case Action::Reload:return "reload";
        }
        return "unknown";
    }
    inline bool KeepEngagement(bool target,bool visible,double sinceSeen)
    { return target && (visible || sinceSeen<0.45); }
    inline bool CanFire(bool visible, float speed, float error, double exposed, int ammo)
    { return visible && speed<35 && error<1.5f && exposed>=0.22 && ammo>0; }

    enum class Goal { None, Hunt, Hold, Take, Plant, Defuse, Guard, Regroup };
    inline const char* Name(Goal g)
    {
        switch (g) {
        case Goal::None: return "none"; case Goal::Hunt: return "hunt"; case Goal::Hold: return "hold";
        case Goal::Take: return "take"; case Goal::Plant: return "plant"; case Goal::Defuse: return "defuse";
        case Goal::Guard: return "guard"; case Goal::Regroup: return "regroup";
        }
        return "unknown";
    }

    enum class Task { Wait, Engage, Recheck, Retreat, Reload, Hold, Clear, Move, Plant, Defuse, Peek };
    inline const char* Name(Task t)
    {
        switch (t) {
        case Task::Wait: return "wait"; case Task::Engage: return "engage"; case Task::Recheck: return "hold-last-seen";
        case Task::Retreat: return "retreat"; case Task::Reload: return "reload"; case Task::Hold: return "hold";
        case Task::Clear: return "clear"; case Task::Move: return "move"; case Task::Plant: return "plant";
        case Task::Defuse: return "defuse"; case Task::Peek: return "peek";
        }
        return "unknown";
    }

    enum class PeekKind { None, Jiggle, Shoulder, Wide };
    inline const char* Name(PeekKind k)
    {
        switch (k) {
        case PeekKind::None: return "none"; case PeekKind::Jiggle: return "jiggle";
        case PeekKind::Shoulder: return "shoulder"; case PeekKind::Wide: return "wide";
        }
        return "unknown";
    }

    struct PeekFacts
    {
        bool Contact = false;
        double SinceInfo = 1e9;
        float Health = 100;
        bool Defensive = false;
        bool EnemyReloading = false;
        bool EnemyBusy = false;
        bool HurtHere = false;         // got hit peeking this corner
        int LocalAllies = 0, LocalEnemies = 1;
        int Jiggles = 0;
        float Aggression = 0.5f;
    };

    inline PeekKind ChoosePeek(const PeekFacts& F)
    {
        if (!F.Contact) return PeekKind::None;
        if (F.HurtHere) return PeekKind::None;
        const bool Punish = F.EnemyReloading || F.EnemyBusy;
        if (F.Defensive)
            return Punish ? PeekKind::Wide
                 : (F.Aggression > 0.75f && F.LocalAllies >= F.LocalEnemies && F.Jiggles < 1 ? PeekKind::Jiggle : PeekKind::None);
        if (Punish) return PeekKind::Wide;
        if (F.LocalEnemies > F.LocalAllies + 1 && F.Aggression < 0.85f) return PeekKind::None;   // wait for team
        if (F.Health < 50) return F.Jiggles < 2 ? PeekKind::Jiggle : PeekKind::None;
        if (F.SinceInfo > 3.0 && F.Jiggles < 2) return PeekKind::Jiggle;
        return F.Aggression > 0.65f ? PeekKind::Wide : PeekKind::Shoulder;
    }

    struct Situation
    {
        bool Active = false;
        bool EnemyVisible = false;
        double SinceSeen = 1e9;
        double SinceHurt = 1e9;
        float Health = 100;
        int Ammo = -1, Magazine = 25, Reserve = 0;
        bool Exposed = false;
        int Allies = 0, Enemies = 1;  // excl self
        Goal Want = Goal::Hunt;
        bool AtGoal = false;
        bool Committed = false;
        float Aggression = 0.5f;
        int VisibleEnemies = 0;
        int NearAllies = 0;
        PeekKind Peek = PeekKind::None;
    };

    inline Task Choose(const Situation& S)
    {
        if (!S.Active) return Task::Wait;
        const bool Empty = S.Ammo == 0;
        if (S.EnemyVisible)
        {
            if (S.Committed) return S.Want == Goal::Defuse ? Task::Defuse : Task::Plant;
            if (Empty) return Task::Retreat;
            if (S.Health < 35 && S.SinceHurt < 1.0 && S.Allies > 0) return Task::Retreat;
            if (S.VisibleEnemies >= 2 && S.NearAllies == 0 && S.Aggression < 0.85f) return Task::Retreat;
            return Task::Engage;
        }
        if (Empty) return S.Reserve > 0 ? (S.Exposed ? Task::Retreat : Task::Reload) : Task::Retreat;
        // they usually repeek
        const double Linger = 1.2 + 1.8 * (1.f - S.Aggression);
        if (S.SinceSeen < Linger && S.Want != Goal::Plant && S.Want != Goal::Defuse) return Task::Recheck;
        if (S.Magazine > 0 && S.Ammo >= 0 && S.Ammo < S.Magazine * 0.35f && S.Reserve > 0 && !S.Exposed) return Task::Reload;
        if (S.Peek != PeekKind::None && S.Want != Goal::Plant && S.Want != Goal::Defuse) return Task::Peek;
        switch (S.Want)
        {
        case Goal::None: return Task::Wait;
        case Goal::Hold: case Goal::Guard: return S.AtGoal ? Task::Hold : Task::Move;
        case Goal::Regroup: return S.AtGoal ? Task::Hold : Task::Move;
        case Goal::Plant: return S.AtGoal ? Task::Plant : Task::Clear;
        case Goal::Defuse: return S.AtGoal ? Task::Defuse : Task::Clear;
        case Goal::Take: case Goal::Hunt: return Task::Clear;
        }
        return Task::Wait;
    }
}

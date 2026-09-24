#include <pch.h>
#include <Headers/Modules/ShooterGame/LocalBots.h>
#include <unordered_map>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <chrono>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <SDK/NavigationSystem_classes.hpp>
#include <Headers/Modules/ShooterGame/BotPolicy.h>
#include <Headers/Modules/ShooterGame/BotTactics.h>
#include <Headers/Modules/ShooterGame/TacticalMap.h>
#include <Headers/Modules/ShooterGame/RoundReferee.h>
#include <Headers/Modules/ShooterGame/LocalTeams.h>
#include <Headers/Modules/ShooterGame/LocalDrop.h>

namespace
{
    using BotPolicy::Goal;
    using BotPolicy::Task;
    namespace fs = std::filesystem;
    constexpr float Pi = 3.14159265358979f;

    struct FSettings
    {
        bool Exact = false;                       // wallhack
        BotPolicy::Skill Skill = BotPolicy::SkillFor("normal");
        BotPolicy::Order Intent = BotPolicy::Order::Auto;
    } Settings;

    uint32_t RandState = 0x9E3779B9u;
    float Rand()
    {
        RandState ^= RandState << 13; RandState ^= RandState >> 17; RandState ^= RandState << 5;
        return (RandState & 0xFFFFFF) / 16777216.f;
    }

    // listen server makes every bot locally controlled so footsteps play at any range
    // IsLocallyControlled 0x6E0, only lie to audio/fx callers
    using IsLocalFn = bool (*)(APawn*);
    constexpr int VtSlots = 48;
    std::atomic<void**> VtKeys[VtSlots]; IsLocalFn VtOrig[VtSlots];
    std::atomic<APawn*> BotSlots[64];
    // dont add 0x1CEF759, guns stop firing
    std::atomic<uintptr_t> RemoteCallers[32] = {0x1988CBE, 0x1D62DC7, 0x190B5C2, 0x1964F9E, 0x19DCBBD};
    std::mutex CallerLock; std::unordered_map<uintptr_t,long> Callers;
    bool IsBotPawn(APawn* p){ for(auto& s:BotSlots) if(s.load(std::memory_order_relaxed)==p) return true; return false; }
    bool IsLocallyControlledHook(APawn* self)
    {
        void** vt=*(void***)self; IsLocalFn orig=nullptr;
        for(int i=0;i<VtSlots;++i) if(VtKeys[i].load(std::memory_order_acquire)==vt){orig=VtOrig[i];break;}
        if(!orig) return false;
        const bool local=orig(self);
        if(!local || !IsBotPawn(self)) return local;
#ifdef _MSC_VER
        const uintptr_t caller=(uintptr_t)_ReturnAddress()-ImageBase;
#else
        const uintptr_t caller=(uintptr_t)__builtin_return_address(0)-ImageBase;
#endif
        { std::lock_guard<std::mutex> l(CallerLock); if(Callers.size()<256) ++Callers[caller]; }
        for(auto& r:RemoteCallers) if(r.load(std::memory_order_relaxed)==caller) return false;
        return true;
    }
    void HookIsLocallyControlled(APawn* pawn)
    {
        void** vt=*(void***)pawn; const int slot=0x6E0/8;
        for(int i=0;i<VtSlots;++i) if(VtKeys[i].load()==vt) return;
        for(int i=0;i<VtSlots;++i) {
            if(VtKeys[i].load()!=nullptr) continue;
            if((void*)vt[slot]==(void*)&IsLocallyControlledHook) return;
            VtOrig[i]=(IsLocalFn)vt[slot];
            VtKeys[i].store(vt,std::memory_order_release);
            DWORD old=0; VirtualProtect(vt+slot,8,PAGE_EXECUTE_READWRITE,&old);
            vt[slot]=(void*)&IsLocallyControlledHook;
            VirtualProtect(vt+slot,8,old,&old);
            Ares::Log("botaudio: hooked IsLocallyControlled for %s",pawn->Class->GetName().c_str());
            return;
        }
    }

    struct FPerf { double Scan=0, Team=0, Control=0, Total=0, Sense=0, Decide=0, Act=0, Aim=0, Move=0; long Scans=0; int Objects=0; double Next=0; } Perf;
    struct FTimer { double& Into; std::chrono::steady_clock::time_point T0=std::chrono::steady_clock::now();
        ~FTimer(){Into+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-T0).count();} };
    template<class T> std::vector<T*> ScanObjects(){FTimer t{Perf.Scan};++Perf.Scans;Perf.Objects=UObject::GObjects->Num();return GetObjectsOfClass<T>();}

    // cached, full GObjects scans every frame were the lag
    struct FRoster { std::vector<TWeakObjectPtr<AShooterCharacter>> List; double At=-100; UWorld* World=nullptr; } RosterCache;
    std::vector<AShooterCharacter*> Roster(UWorld* world)
    {
        if(world!=RosterCache.World || Ares::Seconds()-RosterCache.At>0.5 || Ares::Seconds()<RosterCache.At) {
            FTimer t{Perf.Scan}; ++Perf.Scans;
            TArray<AActor*> found{};
            UGameplayStatics::GetAllActorsOfClass(world,TSubclassOf<AActor>(AShooterCharacter::StaticClass()),&found);
            RosterCache.List.clear();
            for(auto a:found)if(a)if(auto c=a->Cast<AShooterCharacter>())RosterCache.List.push_back(c);
            found.Free();
            RosterCache.At=Ares::Seconds(); RosterCache.World=world;
        }
        std::vector<AShooterCharacter*> out;
        for(auto& w:RosterCache.List)if(auto c=w.Get())if(!c->IsDefaultObject()&&!c->bActorIsBeingDestroyed)out.push_back(c);
        return out;
    }

    bool Usable(AShooterCharacter* Bot) { return Bot && !Bot->IsDefaultObject() && !Bot->bActorIsBeingDestroyed; }
    bool Alive(AShooterCharacter* Bot) { return Bot->DamageHandler && Bot->DamageHandler->bAlive; }
    float Length2(const FVector& v) { return sqrtf(v.X*v.X+v.Y*v.Y); }
    float Length3(const FVector& v) { return sqrtf(v.X*v.X+v.Y*v.Y+v.Z*v.Z); }
    FVector Unit(const FVector& v) { float n=Length2(v); return n>0.01f ? FVector{v.X/n,v.Y/n,0}:FVector{}; }
    float Wrap(float a) { while(a>180) a-=360; while(a< -180) a+=360; return a; }
    float YawTo(const FVector& d) { return atan2f(d.Y,d.X)*180.f/Pi; }
    float PitchTo(const FVector& d) { return atan2f(d.Z,Length2(d))*180.f/Pi; }
    bool InWorld(UObject* object,UWorld* world)
    {
        for(int i=0;object && i<32;++i,object=object->Outer) {
            if(object==world)return true;
            if(auto level=object->Cast<ULevel>())return level->OwningWorld==world;
        }
        return false;
    }
    FVector Eyes(AActor* actor) { FVector p{}; FRotator r{}; actor->GetActorEyesViewPoint(&p,&r); return p; }
    float HalfHeight(AShooterCharacter* c) { return c->CapsuleComponent ? c->CapsuleComponent->GetScaledCapsuleHalfHeight() : 98.f; }
    FVector Floor(AShooterCharacter* c) { auto p=c->K2_GetActorLocation(); p.Z-=HalfHeight(c); return p; }
    bool Sight(UWorld* world,AActor* from,AActor* to,const FVector& a,const FVector& b)
    {
        TArray<AActor*> ignore{}; ignore.Add(from); if(to)ignore.Add(to);
        FHitResult hit{};
        bool blocked=UKismetSystemLibrary::LineTraceSingleByProfile(world,a,b,FName(L"BlockAll"),false,ignore,
            EDrawDebugTrace::None,&hit,true,FLinearColor{},FLinearColor{},0);
        ignore.Free(); return !blocked;
    }
    std::string LastBlock;
    // drop = route goes down a ledge here, dont treat it as a hole
    bool StepClear(UWorld* world,AShooterCharacter* bot,const FVector& from,const FVector& to,float drop=0)
    {
        if(!bot->CapsuleComponent)return false;
        TArray<AActor*> ignore{}; ignore.Add(bot); FHitResult hit{};
        bool blocked=UKismetSystemLibrary::CapsuleTraceSingleByProfile(world,from+FVector{0,0,3},to+FVector{0,0,3},
            bot->CapsuleComponent->GetScaledCapsuleRadius(),HalfHeight(bot),
            FName(L"BlockAll"),false,ignore,EDrawDebugTrace::None,&hit,true,FLinearColor{},FLinearColor{},0);
        FVector floor{};
        const float below=std::clamp(drop,0.f,260.f);
        bool supported=UNavigationSystemV1::K2_ProjectPointToNavigation(world,to-FVector{0,0,below*0.5f},&floor,nullptr,nullptr,FVector{35,35,120+below*0.5f});
        const float step=floor.Z-(to.Z-HalfHeight(bot));
        const bool level=supported && step<85 && step>-85-below;
        if(blocked) {
            auto actor=hit.Actor.Get(); auto comp=hit.Component.Get();
            LastBlock=std::string("hit ")+(actor?actor->GetName():"?")+"/"+(comp?comp->GetName():"?")+
                      (hit.bStartPenetrating?" (start penetrating)":"");
        } else if(!level) LastBlock=supported?"floor step >85":"no navmesh under step";
        ignore.Free(); return !blocked && level;
    }
    FVector NodeEye(const TacticalGraph& g,int n) { auto& N=g.Nodes[n]; return FVector{N.X,N.Y,N.Z+g.Eye}; }
    FVector NodeFloor(const TacticalGraph& g,int n) { auto& N=g.Nodes[n]; return FVector{N.X,N.Y,N.Z}; }
    int NodeOf(const TacticalGraph* g,AShooterCharacter* c)
    { if(!g)return -1; auto f=Floor(c); return g->Nearest(f.X,f.Y,f.Z,500.f); }
    float ViewYaw(AShooterCharacter* c) { return c->Controller ? c->Controller->GetControlRotation().Yaw : c->K2_GetActorRotation().Yaw; }
    bool Facing(AShooterCharacter* c,const FVector& eye,const FVector& at,float halfAngle=55.f)
    {
        FVector d=at-eye; if(Length2(d)<600)return true;
        return fabsf(Wrap(YawTo(d)-ViewYaw(c)))<halfAngle;
    }
    std::string Short(AShooterCharacter* c)
    {
        std::string n=c->GetName(); auto p=n.find("_PC_C_"); return p==std::string::npos?n:n.substr(0,p)+"#"+n.substr(n.size()-4);
    }

    struct FSite { char Name='?'; FVector Min{},Max{},Center{}; int CenterNode=-1; std::vector<int> Nodes; std::vector<float> To; };
    struct FSpawn { std::string Team; std::vector<int> Nodes; std::vector<FVector> Starts; std::vector<float> From; };
    struct FMap
    {
        UWorld* World=nullptr; const TacticalGraph* G=nullptr;
        std::vector<FSite> Sites; std::vector<FSpawn> Spawns; int AttackSpawn=-1;
    } Map;

    void BuildMap(UWorld* world)
    {
        const TacticalGraph* g=TacticalMap::Get();
        if(Map.World==world && Map.G==g)return;
        Map=FMap{}; Map.World=world; Map.G=g;
        if(!g)return;
        FVector bombSpawn{}; bool haveBomb=false;
        std::vector<FVector> starts;
        for(auto a:GetObjectsOfClass<AActor>()) {
            if(!a||a->IsDefaultObject()||a->bActorIsBeingDestroyed||!InWorld(a,world))continue;
            const std::string cls=a->Class->GetName(), name=a->GetName();
            if(cls=="BP_BombDestinationVolume_C" && !name.empty()) {
                FSite s; s.Name=name.back(); FVector c{},e{}; a->GetActorBounds(false,&c,&e);
                s.Min=c-e; s.Max=c+e; s.Center=c; Map.Sites.push_back(s);
            } else if(cls=="BombSpawnLocation_C") { bombSpawn=a->K2_GetActorLocation(); haveBomb=true; }
            else if(a->Cast<AShooterTeamStart>()) starts.push_back(a->K2_GetActorLocation());
        }
        // team starts have no team here, split by distance to spike spawn
        if(haveBomb && starts.size()>=2) {
            auto d=[&](const FVector& p){return Length3(p-bombSpawn);};
            FVector atk=*std::min_element(starts.begin(),starts.end(),[&](auto& a,auto& b){return d(a)<d(b);});
            FVector def=*std::max_element(starts.begin(),starts.end(),[&](auto& a,auto& b){return d(a)<d(b);});
            Map.Spawns.resize(2); Map.Spawns[0].Team="attack"; Map.Spawns[1].Team="defend";
            for(auto& p:starts)Map.Spawns[Length3(p-atk)<Length3(p-def)?0:1].Starts.push_back(p);
        }
        std::sort(Map.Sites.begin(),Map.Sites.end(),[](auto& a,auto& b){return a.Name<b.Name;});
        for(auto a:GetObjectsOfClass<AActor>()) {
            if(!a||a->IsDefaultObject()||!InWorld(a,world)||a->Class->GetName()!="BombDestination_C")continue;
            std::string name=a->GetName();
            for(auto& s:Map.Sites)if(!name.empty()&&name.back()==s.Name)s.Center=a->K2_GetActorLocation();
        }
        for(auto& s:Map.Sites) {
            for(int i=0;i<g->Count();++i) {
                auto& n=g->Nodes[i];
                if(n.X>=s.Min.X&&n.X<=s.Max.X&&n.Y>=s.Min.Y&&n.Y<=s.Max.Y&&n.Z>=s.Min.Z-60&&n.Z<=s.Max.Z)s.Nodes.push_back(i);
            }
            s.CenterNode=g->Nearest(s.Center.X,s.Center.Y,s.Center.Z,800);
            if(s.CenterNode<0&&!s.Nodes.empty())s.CenterNode=s.Nodes[0];
            if(s.Nodes.empty()&&s.CenterNode>=0)s.Nodes.push_back(s.CenterNode);
            g->Distances(s.Nodes,s.To);
        }
        float best=1e30f;
        for(int k=0;k<(int)Map.Spawns.size();++k) {
            auto& sp=Map.Spawns[k];
            for(auto& p:sp.Starts) {
                int n=g->Nearest(p.X,p.Y,p.Z-92,700);
                if(n<0)n=g->Nearest(p.X,p.Y,p.Z-92,1600);
                if(n>=0&&std::find(sp.Nodes.begin(),sp.Nodes.end(),n)==sp.Nodes.end())sp.Nodes.push_back(n);
                if(haveBomb){float d=Length3(p-bombSpawn);if(d<best){best=d;Map.AttackSpawn=k;}}
            }
            g->Distances(sp.Nodes,sp.From);
        }
        std::string sites,spawns;
        for(auto& s:Map.Sites)sites+=std::string(1,s.Name)+"("+std::to_string(s.Nodes.size())+" nodes) ";
        for(auto& s:Map.Spawns)spawns+=s.Team+"("+std::to_string(s.Nodes.size())+" nodes) ";
        Ares::Log("botmap: sites %s spawns %s attack-spawn=%s",sites.c_str(),spawns.c_str(),
                  Map.AttackSpawn>=0?Map.Spawns[Map.AttackSpawn].Team.c_str():"?");
    }
    const FSpawn* SpawnFor(UBaseTeamComponent* team)
    {
        if(!team)return nullptr;
        const std::string name=team->GetTeamName().ToString();
        for(auto& s:Map.Spawns)if(s.Team==name)return &s;
        return nullptr;
    }
    const FSpawn* StartSpawn(UBaseTeamComponent* team)
    {
        auto& r=RoundReferee::View();
        if(RoundReferee::Active() && Map.AttackSpawn>=0 && Map.Spawns.size()==2) {
            if(team==r.Attackers)return &Map.Spawns[Map.AttackSpawn];
            if(team==r.Defenders)return &Map.Spawns[1-Map.AttackSpawn];
        }
        return SpawnFor(team);
    }

    struct FEntry
    {
        LocalBots::FOptions Options;
        TWeakObjectPtr<AShooterCharacter> Ref;
        TWeakObjectPtr<UWorld> World;
        bool Enabled=true;
        Goal Want=Goal::Hunt; int GoalNode=-1, WatchNode=-1, Site=-1; double GoAt=0; float Aggression=0.5f;
        Task Now=Task::Wait; double TaskSince=0, NextThink=0, NextSense=0;
        TWeakObjectPtr<AShooterCharacter> Target; bool Visible=false; double VisibleSince=-1, LastSeen=-100;
        FVector LastSeenPos{}; float Health=100, LastHealth=100; double HurtAt=-100;
        int Ammo=-1, Reserve=0, Magazine=25; BotPolicy::GunProfile Gun; std::string GunName;
        FVector MoveGoal{}; bool HasMove=false; std::vector<FVector> Path; FVector PathGoal{}; size_t Waypoint=0;
        double Repath=0; std::vector<int> Route; int RouteGoal=-1; double Reroute=0;
        FVector ProgressAt{}; double ProgressTime=0; int Stuck=0; unsigned BlockedFrames=0;
        std::vector<int> Slice; int SliceAim=-1, SliceCandidates=-1, SliceRevealed=-1; double SlicePause=0;
        std::unordered_map<int,double> Checked, Avoid; int LastNext=-1;
        unsigned SteepPaths=0; double NextSteer=0; float SteerAngle=0, BodyYaw=1e9f;
        double ManualUntil=0; int ManualNode=-1;   // botorder goto
        FVector HeldMove{}; double HeldWallUntil=0;
        bool OnJump=false; double NextJump=0, JumpedAt=-100, JumpBackoffUntil=-10; int JumpFrom=-1, JumpTo=-1; std::unordered_map<long long,int> JumpTries;
        FVector WindowAt{}; double WindowStart=0, NextOscLog=0;
        FRotator Rot{}; bool RotInit=false; BotPolicy::AimAxis Yaw, Pitch; FVector Aim{}; float ErrYaw=0, ErrPitch=0;
        bool Head=true; double ReactAt=0; float TrackError=180;
        const char* Pace=""; double DiagAt=0, DiagTime=0, DiagDist=0, DiagTurn=0, DiagInput=0; int DiagGoals=0, DiagCalls=0, DiagIdle=0; FVector DiagMove{}, DiagGoal{}; int Phase=0; double PhaseUntil=0; float StrafeSide=0; int ShotsLeft=0; double NextShot=0, PressUntil=0; bool Trigger=false; float Side=1.f;
        unsigned ShotsRequested=0; bool Crouched=false; double ReloadAt=0, LastFireAt=-100;
        double LastFrame=-1, Report=0;
        TWeakObjectPtr<AShooterCharacter> LastTarget;
        int VisibleEnemies=0, NearAllies=0; FVector HurtPos{};
        BotPolicy::PeekKind PeekKind=BotPolicy::PeekKind::None;
        int PeekStage=0, Jiggles=0; FVector PeekOrigin{}, PeekOut{}, PeekAim{}; double PeekUntil=0, PeekFailUntil=0, ContactSince=0;
        TWeakObjectPtr<AShooterCharacter> PeekEnemy; bool FromPeek=false;
        float Temper=0.55f;   // per bot aggression
        bool TradeInPlace=false;
        std::string Role="solo"; double TradeUntil=0, RelocateAt=0; int TradeNode=-1, TradeWatch=-1, Kills=0;
        TWeakObjectPtr<AShooterCharacter> Fighting; std::vector<int> AvoidRoute;
        double Next=0.0; float Sideways=1.f;
    };
    std::unordered_map<AShooterCharacter*, FEntry> Bots;
    bool Updating=false;

    // navmesh goes through closed doors and tight gaps, shared by all bots
    struct FBadSpot { FVector At; double Until; };
    std::vector<FBadSpot> BadSpots;

    struct FTrack
    {
        TWeakObjectPtr<AShooterCharacter> Who; Belief B; double LastSeen=-100, LastHeard=-100, FiredAt=-100;
        FVector LastPos{}; int LastAmmo=-1; bool Seen=false;
    };
    struct FTeam
    {
        UBaseTeamComponent* Id=nullptr;
        std::vector<FTrack> Tracks;
        std::vector<float> Threat, Penalty; std::vector<int> Hot;
        double NextPenalty=0, NextPlan=0, LastUpdate=-1;
        int HuntTarget=-1; double NextHunt=0;
        int PlanRound=-1, Site=-1; int Style=0; bool Executing=false, Retaking=false; double ExecuteAt=0;
        std::vector<std::vector<int>> Routes; std::vector<int> Staging;
        int ForceSite=-1, ForceStyle=-1;   // brain
        int Alive=0, Enemies=0;
        std::unordered_map<AShooterCharacter*,FVector> LastAlive; int LastRound=-1;
    };
    std::vector<FTeam> Teams;

    FTeam& TeamFor(UBaseTeamComponent* id)
    {
        for(auto& t:Teams)if(t.Id==id)return t;
        Teams.push_back(FTeam{}); Teams.back().Id=id; return Teams.back();
    }
    UBaseTeamComponent* TeamOf(AShooterCharacter* c)
    {
        auto t=UBaseTeamComponent::GetTeamComponent(c);
        return t&&t->HasValidTeam()?t:nullptr;
    }
    TWeakObjectPtr<APawn> Ignored;   // freecam body
    std::vector<AShooterCharacter*> Fighters(UWorld* world)
    {
        std::vector<AShooterCharacter*> out;
        for(auto c:Roster(world))
            if(Usable(c)&&InWorld(c,world)&&Alive(c)&&TeamOf(c)&&c!=Ignored.Get())out.push_back(c);
        return out;
    }
    int AmmoOf(UWorld* world,AShooterCharacter* c,int* reserve=nullptr)
    {
        if(!c->Inventory)return -1;
        auto gun=c->Inventory->GetCurrentEquippable(); if(!gun)return -1;
        bool valid=false; FAmmoHudInfo ammo{};
        UShooterBlueprintLibrary::GetAmmoHudInfoForEquippable(world,gun,&valid,&ammo);
        if(!valid)return -1;
        if(reserve)*reserve=ammo.Reserve.Ammo;
        return ammo.Magazine.Ammo;
    }

    void SeedTrack(FTrack& t,const TacticalGraph& g,AShooterCharacter* enemy,UBaseTeamComponent* enemyTeam)
    {
        auto sp=StartSpawn(enemyTeam);
        if(RoundReferee::Active() && sp && !sp->Nodes.empty()) t.B.Uniform(g,sp->Nodes);
        else { int n=NodeOf(&g,enemy); if(n>=0)t.B.Observe(g,n); else t.B.Uniform(g,{}); }
    }

    void UpdateBeliefs(UWorld* world,FTeam& team,const std::vector<AShooterCharacter*>& all,double now)
    {
        std::vector<AShooterCharacter*> mates, enemies;
        for(auto c:all)(TeamOf(c)==team.Id?mates:enemies).push_back(c);
        team.Alive=(int)mates.size(); team.Enemies=(int)enemies.size();
        auto g=Map.G;
        // new round respawns everyone, not a death
        if(team.LastRound!=RoundReferee::View().Round){team.LastRound=RoundReferee::View().Round;team.LastAlive.clear();}
        // mate gone since last update = died, nearby bots trade
        for(auto& [who,where]:team.LastAlive) {
            if(std::find(mates.begin(),mates.end(),who)!=mates.end())continue;
            const FTrack* killer=nullptr; float best=1e30f;
            for(auto& t:team.Tracks){if(!t.Who.Get()||now-std::max(t.LastSeen,t.LastHeard)>4)continue;
                float d=Length3(t.LastPos-where);if(d<best&&d<4000){best=d;killer=&t;}}
            const FVector focus=killer?killer->LastPos:where;
            for(auto& [bot,s]:Bots) {
                if(!Usable(bot)||!Alive(bot)||TeamOf(bot)!=team.Id||s.Options.Behaviour!=LocalBots::EBehaviour::Combat)continue;
                if(s.Visible||s.Now==Task::Plant||s.Now==Task::Defuse)continue;
                if(Length3(bot->K2_GetActorLocation()-where)>2500)continue;
                // holders stay put, running out one by one just feeds
                s.TradeInPlace=s.Want==Goal::Hold||s.Want==Goal::Guard||s.Want==Goal::Regroup;
                s.TradeUntil=now+(s.TradeInPlace?3:5); s.Aggression=std::max(s.Aggression,0.85f);
                s.TradeNode=g?g->Nearest(where.X,where.Y,where.Z-98,600):-1;
                s.TradeWatch=g?g->Nearest(focus.X,focus.Y,focus.Z-98,600):-1;
                s.LastSeenPos=focus; s.Jiggles=0;
                Ares::Log("bottrade: %s trades a teammate killed at %.0f %.0f (killer %s)",Short(bot).c_str(),where.X,where.Y,
                          killer&&killer->Who.Get()?Short(killer->Who.Get()).c_str():"unknown");
            }
        }
        team.LastAlive.clear();
        for(auto m:mates)team.LastAlive[m]=m->K2_GetActorLocation();
        if(!g)return;
        const float dt=team.LastUpdate<0?0.2f:(float)std::clamp(now-team.LastUpdate,0.0,1.0);
        team.LastUpdate=now;
        team.Tracks.erase(std::remove_if(team.Tracks.begin(),team.Tracks.end(),[&](FTrack& t){
            auto w=t.Who.Get(); return !w||std::find(enemies.begin(),enemies.end(),w)==enemies.end();}),team.Tracks.end());
        std::vector<int> mateNodes; std::vector<FVector> mateEyes;
        for(auto m:mates){mateNodes.push_back(NodeOf(g,m));mateEyes.push_back(Eyes(m));}
        std::vector<int> fallback;
        for(auto e:enemies) {
            FTrack* t=nullptr; for(auto& x:team.Tracks)if(x.Who.Get()==e)t=&x;
            if(!t){team.Tracks.push_back(FTrack{});t=&team.Tracks.back();t->Who=e;SeedTrack(*t,*g,e,TeamOf(e));}
            const FVector pos=e->K2_GetActorLocation(), eye=Eyes(e);
            const int node=NodeOf(g,e);
            const int ammo=AmmoOf(world,e);
            const bool fired=ammo>=0&&t->LastAmmo>=0&&ammo<t->LastAmmo;
            if(fired)t->FiredAt=now;
            t->LastAmmo=ammo;
            bool seen=Settings.Exact;
            for(size_t i=0;i<mates.size()&&!seen;++i)
                if(Length3(eye-mateEyes[i])<7000 && Facing(mates[i],mateEyes[i],eye) && Sight(world,mates[i],e,mateEyes[i],eye))seen=true;
            t->Seen=seen;
            if(seen && node>=0){t->B.Observe(*g,node);t->LastSeen=now;t->LastPos=pos;continue;}
            t->B.Spread(*g,520.f*dt);
            for(size_t i=0;i<mates.size();++i) {
                if(mateNodes[i]<0)continue;
                std::vector<int> cleared;
                for(int j=0;j<g->Count();++j)
                    if(t->B.P[j]>1e-5f && g->Sees(mateNodes[i],j) && g->Dist(mateNodes[i],j)<4500 &&
                       Facing(mates[i],mateEyes[i],NodeEye(*g,j),60.f))cleared.push_back(j);
                t->B.Clear(cleared,0.03f);
            }
            float nearest=1e30f; for(auto& m:mateEyes)nearest=std::min(nearest,Length3(m-eye));
            const float speed=Length2(e->GetVelocity());
            if((fired&&nearest<5000)||(speed>420&&nearest<2000)) {
                t->B.Hear(*g,pos.X,pos.Y,pos.Z-HalfHeight(e),fired?300.f:450.f);
                t->LastHeard=now; t->LastPos=pos;
            }
            if(fallback.empty())for(int j=0;j<g->Count();++j)fallback.push_back(j);
            t->B.Normalize(*g,fallback);
        }
        std::vector<const Belief*> beliefs; for(auto& t:team.Tracks)beliefs.push_back(&t.B);
        BotTactics::Threat(beliefs,g->Count(),team.Threat);
        BotTactics::HotList(team.Threat,team.Hot);
        if(now>=team.NextPenalty || team.Penalty.size()!=(size_t)g->Count()) {
            team.NextPenalty=now+1.0;
            BotTactics::ExposurePenalty(*g,team.Threat,team.Hot,2500.f,team.Penalty);
        }
    }

    std::vector<FEntry*> Members(FTeam& team,UWorld* world)
    {
        std::vector<FEntry*> out;
        for(auto& [bot,s]:Bots)
            if(s.Ref.Get()==bot && s.World.Get()==world && s.Options.Behaviour==LocalBots::EBehaviour::Combat &&
               Usable(bot) && Alive(bot) && TeamOf(bot)==team.Id)out.push_back(&s);
        std::sort(out.begin(),out.end(),[](FEntry* a,FEntry* b){return a->Ref.Get()->Index<b->Ref.Get()->Index;});
        return out;
    }

    void AssignHolds(const TacticalGraph& g,const std::vector<FEntry*>& who,const std::vector<int>& area,const std::vector<float>& fromEnemy,
                     Goal goal,uint32_t seed,float radius=1400.f,const std::vector<int>& avoid={})
    {
        if(who.empty()||area.empty())return;
        std::vector<float> toArea; g.Distances(area,toArea);
        std::vector<char> approach; BotTactics::Approaches(fromEnemy,toArea,1.3f,approach);
        std::vector<int> candidates;
        for(int i=0;i<g.Count();++i) {
            if(!(toArea[i]<radius && std::isfinite(fromEnemy[i])))continue;
            bool excluded=false; for(int a:avoid)if(a>=0&&g.Dist(a,i)<350)excluded=true;
            if(!excluded)candidates.push_back(i);
        }
        std::vector<int> watch,taken=avoid;
        for(auto s:who) {
            auto h=BotTactics::ChooseHold(g,candidates,approach,fromEnemy,watch,taken,seed+(uint32_t)s->Ref.Get()->Index);
            s->Want=goal; s->GoalNode=h.Node>=0?h.Node:area[0]; s->WatchNode=h.Watch;
            watch.push_back(h.Watch); taken.push_back(h.Node);
        }
    }

    int NearestMember(const TacticalGraph& g,const std::vector<FEntry*>& who,int node,FEntry** out)
    {
        float best=1e30f; *out=nullptr;
        for(auto s:who){int h=NodeOf(&g,s->Ref.Get());float d=h<0?1e9f:g.Dist(h,node);if(d<best){best=d;*out=s;}}
        return (int)best;
    }

    void PlanSkirmish(FTeam& team,UWorld* world,double now)
    {
        auto g=Map.G; auto who=Members(team,world);
        if(team.Tracks.empty()) {
            for(auto s:who){s->Want=Goal::None;s->GoalNode=-1;s->Role="idle";}
            return;
        }
        // entry, trade, flank
        std::vector<FEntry*> hunters;
        int target=-1;
        if(g && (now>=team.NextHunt || team.HuntTarget<0)) {
            // sticky target or hunters ping pong
            team.NextHunt=now+1.0;
            std::vector<int> starts; for(auto s:who){int n=NodeOf(g,s->Ref.Get());if(n>=0)starts.push_back(n);}
            std::vector<float> d; g->Distances(starts,d);
            float score=1e30f; int best=-1; float bestMass=0, keptMass=0;
            std::vector<float> mass;
            for(auto& t:team.Tracks) {
                int p=t.B.Hotspot(*g,1200.f,mass);
                if(p<0)continue;
                if(team.HuntTarget>=0&&team.HuntTarget<(int)mass.size())keptMass=std::max(keptMass,mass[team.HuntTarget]);
                if(std::isfinite(d[p])&&d[p]<score){score=d[p];best=p;bestMass=mass[p];}
            }
            if(best>=0 && !(team.HuntTarget>=0 && keptMass*1.5f>=bestMass))team.HuntTarget=best;
            if(best<0)team.HuntTarget=-1;
        }
        target=team.HuntTarget;
        for(auto s:who) {
            auto bot=s->Ref.Get(); int here=NodeOf(g,bot);
            bool hold=Settings.Intent==BotPolicy::Order::Hold ||
                      (Settings.Intent==BotPolicy::Order::Auto && (s->Health<45 || team.Enemies>team.Alive+1));
            s->Aggression=Settings.Intent==BotPolicy::Order::Push?0.9f:0.5f;
            if(!g||here<0){s->Want=hold?Goal::Hold:Goal::Hunt;s->GoalNode=-1;continue;}
            int best=target;
            if(!hold){hunters.push_back(s);continue;}
            {
                if(s->Want!=Goal::Hold||s->GoalNode<0||now>=s->GoAt) {
                    std::vector<float> fromEnemy; std::vector<int> src;
                    for(int h:team.Hot)if(team.Threat[h]>0.02f)src.push_back(h);
                    if(src.empty()&&best>=0)src.push_back(best);
                    g->Distances(src,fromEnemy);
                    AssignHolds(*g,{s},{here},fromEnemy,Goal::Hold,(uint32_t)now,900.f);
                    s->GoAt=now+12;
                    s->Role="anchor";
                }
            }
        }
        if(!g||hunters.empty()||target<0)return;
        std::vector<float> toTarget; g->Distances({target},toTarget);
        // sticky entry or roles swap every update
        auto cost=[&](FEntry* e){int n=NodeOf(g,e->Ref.Get());float c=n<0?1e30f:toTarget[n];return e->Role=="entry"?c-600.f:c;};
        std::sort(hunters.begin(),hunters.end(),[&](FEntry* a,FEntry* b){return cost(a)<cost(b);});
        FEntry* entry=hunters[0];
        for(size_t i=0;i<hunters.size();++i) {
            auto s=hunters[i];
            s->Want=Goal::Hunt; s->WatchNode=-1; s->AvoidRoute.clear();
            if(i==0){s->Role="entry";s->GoalNode=target;s->Aggression=std::max(s->Aggression,0.6f);continue;}
            if(i==2){
                s->Role="flank"; s->GoalNode=target; s->AvoidRoute=entry->Route; s->Aggression=std::max(s->Aggression,0.55f);
                continue;
            }
            FEntry* lead=i==1?entry:hunters[i-1];
            s->Role="trade";
            int follow=target;
            const int leadNode=NodeOf(g,lead->Ref.Get());
            auto at=std::find(lead->Route.begin(),lead->Route.end(),leadNode);
            if(at!=lead->Route.end()) {
                const size_t k=at-lead->Route.begin();
                follow=lead->Route[k>=3?k-3:0];
            } else if(leadNode>=0) follow=leadNode;
            if(lead->Visible||now-lead->LastSeen<2)follow=leadNode>=0?leadNode:target;
            s->GoalNode=follow;
            s->Aggression=std::max(s->Aggression,0.55f);
        }
    }

    void ApplyOverrides(FTeam& team,UWorld* world,double now)
    {
        auto g=Map.G;
        for(auto s:Members(team,world)) {
            if(now<s->ManualUntil && s->ManualNode>=0){s->Want=Goal::Take;s->GoalNode=s->ManualNode;s->WatchNode=-1;s->Role="goto";continue;}
            if(now<s->TradeUntil && (s->TradeWatch>=0||s->TradeNode>=0)) {
                if(s->TradeInPlace){ if(s->TradeWatch>=0)s->WatchNode=s->TradeWatch; continue; }
                s->Want=Goal::Take; s->GoalNode=s->TradeWatch>=0?s->TradeWatch:s->TradeNode; s->WatchNode=s->TradeWatch;
                s->GoAt=0; s->Role="trade";
                continue;
            }
            if(s->RelocateAt>0 && now>=s->RelocateAt) {
                s->RelocateAt=0;
                if(!g||s->GoalNode<0||(s->Want!=Goal::Hold&&s->Want!=Goal::Guard))continue;
                std::vector<float> fromEnemy; std::vector<int> src;
                for(int h:team.Hot)if(team.Threat[h]>0.02f)src.push_back(h);
                if(src.empty())continue;
                g->Distances(src,fromEnemy);
                const int old=s->GoalNode;
                AssignHolds(*g,{s},{old},fromEnemy,s->Want,(uint32_t)(now*10),900.f,{old});
                s->GoAt=now+12;
                Ares::Log("botrelocate: %s leaves node %d for node %d after a kill",Short(s->Ref.Get()).c_str(),old,s->GoalNode);
            }
        }
    }

    const char* StyleName(int s){return s==0?"default":s==1?"execute":"rush";}

    void PlanRound(FTeam& team,UWorld* world,double now)
    {
        auto g=Map.G; auto& r=RoundReferee::View();
        auto who=Members(team,world);
        if(!g||who.empty()||Map.Sites.size()<2)return;
        const bool attack=team.Id==r.Attackers;
        auto mine=StartSpawn(team.Id);
        auto theirs=StartSpawn(attack?r.Defenders:r.Attackers);
        if(!mine||!theirs||theirs->From.size()!=(size_t)g->Count())return;
        const double left=RoundReferee::TimeLeft();
        const uint32_t seed=(uint32_t)r.Round*7919u;
        if(team.PlanRound!=r.Round) {
            team.PlanRound=r.Round; team.Executing=false; team.Retaking=false; team.NextPlan=0;
            team.Site=team.ForceSite>=0?team.ForceSite:(Rand()<0.5f?0:1);
            team.Style=team.ForceStyle>=0?team.ForceStyle:(Rand()<0.45f?0:(Rand()<0.7f?1:2));
            team.Routes.clear(); team.Staging.clear();
            if(attack && !mine->Nodes.empty()) {
                auto& site=Map.Sites[team.Site];
                team.Routes=BotTactics::Entrances(*g,mine->Nodes[0],site.Nodes,3);
                for(auto& route:team.Routes)team.Staging.push_back(BotTactics::Staging(*g,route,site.Nodes));
                const double live=r.PhaseEnds; // buy end
                team.ExecuteAt=team.Style==2?live:(team.Style==1?live+22:live+48);
            }
            Ares::Log("botplan: round %d team %s %s %c %s, %zu entrances",r.Round,team.Id->GetTeamName().ToString().c_str(),
                      attack?"attacks":"defends",Map.Sites[team.Site].Name,attack?StyleName(team.Style):"",team.Routes.size());
            for(auto s:who){s->GoAt=0;s->GoalNode=-1;s->WatchNode=-1;s->Site=-1;}
        }
        if(r.Phase==RoundReferee::EPhase::Buy){for(auto s:who){s->Want=Goal::None;}return;}
        const bool planted=r.Phase==RoundReferee::EPhase::Planted;
        const int spikeNode=r.SpikeDown?g->Nearest(r.SpikeX,r.SpikeY,r.SpikeZ,600):-1;

        if(attack) {
            auto& site=Map.Sites[planted&&r.PlantSite>=0?r.PlantSite:team.Site];
            if(planted) {
                std::vector<FEntry*> free;
                for(auto s:who)if(s->Want!=Goal::Guard||s->GoalNode<0)free.push_back(s);
                if(!free.empty()&&spikeNode>=0) AssignHolds(*g,free,{spikeNode},theirs->From,Goal::Guard,seed,1600.f);
                return;
            }
            FEntry* fetch=nullptr;
            if(!r.Carrier && r.SpikeDown && spikeNode>=0) {
                NearestMember(*g,who,spikeNode,&fetch);
                if(fetch){fetch->Want=Goal::Take;fetch->GoalNode=spikeNode;fetch->GoAt=0;}
            }
            const bool go=team.Executing || now>=team.ExecuteAt || left<40;
            if(go&&!team.Executing){team.Executing=true;Ares::Log("botplan: team %s executes onto %c (%.0fs left)",
                team.Id->GetTeamName().ToString().c_str(),site.Name,left);}
            float siteThreat=0; for(int n:site.Nodes)siteThreat+=team.Threat[n];
            for(size_t i=0;i<who.size();++i) {
                auto s=who[i]; auto bot=s->Ref.Get();
                if(s==fetch)continue;
                const int route=team.Routes.empty()?-1:(int)(i%team.Routes.size());
                if(bot==r.Carrier && team.Executing && siteThreat<0.35f) {
                    int spot=site.CenterNode; float best=1e30f;
                    for(int n:site.Nodes){float e=BotTactics::Exposure(*g,team.Threat,team.Hot,n)*3000+g->Dist(n,site.CenterNode)*0.3f; if(e<best){best=e;spot=n;}}
                    s->Want=Goal::Plant; s->GoalNode=spot; s->GoAt=0; continue;
                }
                if(!team.Executing && route>=0 && team.Staging[route]>=0) {
                    s->Want=Goal::Regroup; s->GoalNode=team.Staging[route]; s->WatchNode=site.CenterNode; continue;
                }
                if(s->Want!=Goal::Take||s->GoalNode<0) {
                    s->Want=Goal::Take;
                    s->GoalNode=site.Nodes[(i*7+r.Round)%site.Nodes.size()];
                    s->Aggression=bot==r.Carrier?0.35f:0.75f;
                    s->GoAt=std::max(now,team.ExecuteAt)+(bot==r.Carrier?2.5:0.8*i);
                }
            }
            return;
        }
        if(planted && r.PlantSite>=0 && spikeNode>=0) {
            auto& site=Map.Sites[r.PlantSite];
            // retake, group up first
            int ready=0;
            if(!team.Retaking) for(auto s:who){
                if(s->Want!=Goal::Regroup||s->Site!=r.PlantSite){
                    int here=NodeOf(g,s->Ref.Get());
                    auto route=g->Route(here<0?spikeNode:here,spikeNode,team.Penalty.size()==(size_t)g->Count()?team.Penalty.data():nullptr);
                    int stage=BotTactics::Staging(*g,route,{spikeNode});
                    s->Want=Goal::Regroup; s->Site=r.PlantSite; s->GoalNode=stage>=0?stage:spikeNode; s->WatchNode=spikeNode;
                }
                int here=NodeOf(g,s->Ref.Get());
                if(here>=0&&s->GoalNode>=0&&g->Dist(here,s->GoalNode)<350)++ready;
            }
            const double spikeLeft=RoundReferee::TimeLeft();
            const bool waited=RoundReferee::SpikeSeconds-spikeLeft>10;
            if(!team.Retaking && (ready>=(int)who.size() || waited || spikeLeft<25)){team.Retaking=true;
                Ares::Log("botplan: team %s retakes %c (%d ready, %.0fs on spike)",team.Id->GetTeamName().ToString().c_str(),site.Name,ready,spikeLeft);}
            if(team.Retaking) {
                float threat=0; for(int n:site.Nodes)threat+=team.Threat[n];
                FEntry* defuser=nullptr; NearestMember(*g,who,spikeNode,&defuser);
                for(auto s:who) {
                    if(s==defuser && (threat<0.3f || spikeLeft<RoundReferee::DefuseSeconds+1.5)) {s->Want=Goal::Defuse;s->GoalNode=spikeNode;s->GoAt=0;continue;}
                    s->Want=Goal::Take; s->GoalNode=spikeNode; s->Aggression=0.8f; s->GoAt=0;
                }
            }
            return;
        }
        bool unassigned=false; for(auto s:who)if(s->GoalNode<0||s->Want!=Goal::Hold)unassigned=true;
        if(now>=team.NextPlan || unassigned) {
            team.NextPlan=now+15;
            int n=(int)who.size(); int onA=(n+1)/2;
            for(int k=0;k<2;++k){float mass=0; for(int m:Map.Sites[k].Nodes)mass+=team.Threat[m]; if(mass>1.2f){onA=k==0?n:0;
                Ares::Log("botplan: team %s rotates to %c (%.1f enemies believed on site)",team.Id->GetTeamName().ToString().c_str(),Map.Sites[k].Name,mass);}}
            std::vector<FEntry*> a(who.begin(),who.begin()+std::min(n,onA)), b(who.begin()+std::min(n,onA),who.end());
            AssignHolds(*g,a,Map.Sites[0].Nodes,theirs->From,Goal::Hold,seed,1500.f);
            AssignHolds(*g,b,Map.Sites[1].Nodes,theirs->From,Goal::Hold,seed+1,1500.f);
        }
    }

    struct FBrain { long long Seq=-1; int Applied=0; std::string Last, Orders; } Brain;
    void ReadBrain(double now)
    {
        static double Next=0; if(now<Next&&now>Next-5)return; Next=now+0.5;
        auto file=fs::path(Ares::Config().LogPath).parent_path()/"bots"/"brain"/"orders.txt";
        std::ifstream in(file); if(!in)return;
        std::string line; long long seq=-1;
        std::vector<std::string> lines;
        while(std::getline(in,line)){if(line.rfind("seq ",0)==0)seq=atoll(line.c_str()+4);else if(!line.empty())lines.push_back(line);}
        if(seq<0||seq==Brain.Seq)return;
        Brain.Applied=0;
        for(auto& l:lines) {
            std::istringstream s(l); std::string kind,who,what,arg;
            s>>kind>>who>>what>>arg;
            if(kind=="team") {
                for(auto& t:Teams)if(t.Id&&t.Id->GetTeamName().ToString()==who) {
                    if(what=="site"){t.ForceSite=arg=="A"||arg=="a"?0:arg=="B"||arg=="b"?1:-1;++Brain.Applied;}
                    if(what=="style"){t.ForceStyle=arg=="default"?0:arg=="execute"?1:arg=="rush"?2:-1;++Brain.Applied;}
                    if(what=="go"){t.ExecuteAt=0;++Brain.Applied;}
                    if(what=="replan"){t.PlanRound=-1;++Brain.Applied;}
                }
            } else if(kind=="bot") {
                for(auto& [bot,e]:Bots)if(Usable(bot)&&std::to_string(bot->Index)==who) {
                    if(what=="aggression"){e.Aggression=std::clamp((float)atof(arg.c_str()),0.f,1.f);++Brain.Applied;}
                }
            }
        }
        // retry if the team doesnt exist yet
        if(Brain.Applied>0||lines.empty())Brain.Seq=seq;
        Brain.Last=std::to_string(Brain.Applied)+" of "+std::to_string(lines.size())+" orders applied (seq "+std::to_string(seq)+")";
        std::string joined; for(auto& l:lines)joined+=l+"; ";
        if(joined!=Brain.Orders){Brain.Orders=joined;Ares::Log("brain: %s: %s",Brain.Last.c_str(),joined.c_str());}
    }

    void WriteBrainState(UWorld* world,double now)
    {
        static double Next=0; if(now<Next&&now>Next-5)return; Next=now+0.5;
        auto dir=fs::path(Ares::Config().LogPath).parent_path()/"bots"/"brain";
        std::error_code ec; fs::create_directories(dir,ec);
        auto& r=RoundReferee::View();
        std::ostringstream o;
        o<<"{\"time\":"<<now<<",\"round\":{\"active\":"<<RoundReferee::Active()<<",\"number\":"<<r.Round<<",\"phase\":"<<(int)r.Phase
         <<",\"time_left\":"<<RoundReferee::TimeLeft()<<",\"attack_score\":"<<r.AttackScore<<",\"defense_score\":"<<r.DefenseScore
         <<",\"attackers\":\""<<(r.Attackers?r.Attackers->GetTeamName().ToString():"")<<"\",\"spike_down\":"<<r.SpikeDown
         <<",\"plant_site\":"<<r.PlantSite<<"},\"sites\":[";
        for(size_t i=0;i<Map.Sites.size();++i)o<<(i?",":"")<<"{\"name\":\""<<Map.Sites[i].Name<<"\",\"center\":["<<Map.Sites[i].Center.X<<","<<Map.Sites[i].Center.Y<<"]}";
        o<<"],\"teams\":[";
        bool first=true;
        for(auto& t:Teams) {
            if(!t.Id)continue;
            o<<(first?"":",")<<"{\"name\":\""<<t.Id->GetTeamName().ToString()<<"\",\"alive\":"<<t.Alive<<",\"enemies\":"<<t.Enemies
             <<",\"site\":"<<t.Site<<",\"style\":\""<<StyleName(t.Style)<<"\",\"executing\":"<<t.Executing<<",\"bots\":[";
            first=false; bool fb=true;
            for(auto s:Members(t,world)) {
                auto b=s->Ref.Get(); auto p=b->K2_GetActorLocation();
                o<<(fb?"":",")<<"{\"id\":"<<b->Index<<",\"name\":\""<<Short(b)<<"\",\"pos\":["<<(int)p.X<<","<<(int)p.Y<<","<<(int)p.Z
                 <<"],\"health\":"<<s->Health<<",\"ammo\":"<<s->Ammo<<",\"task\":\""<<BotPolicy::Name(s->Now)<<"\",\"goal\":\""
                 <<BotPolicy::Name(s->Want)<<"\",\"aggression\":"<<s->Aggression<<"}";
                fb=false;
            }
            o<<"],\"known_enemies\":[";
            bool fe=true;
            for(auto& k:t.Tracks) {
                auto w=k.Who.Get(); if(!w)continue; int peak=k.B.Peak();
                o<<(fe?"":",")<<"{\"name\":\""<<Short(w)<<"\",\"seen_ago\":"<<(now-k.LastSeen)<<",\"last\":["<<(int)k.LastPos.X<<","<<(int)k.LastPos.Y
                 <<"],\"likely\":["<<(peak>=0&&Map.G?(int)Map.G->Nodes[peak].X:0)<<","<<(peak>=0&&Map.G?(int)Map.G->Nodes[peak].Y:0)
                 <<"],\"confidence\":"<<(peak>=0?k.B.P[peak]:0)<<"}";
                fe=false;
            }
            o<<"]}";
        }
        o<<"]}";
        { std::ofstream(dir/"state.tmp")<<o.str(); }
        fs::rename(dir/"state.tmp",dir/"state.json",ec);
    }

    void Input(AShooterCharacter* bot,EAresEquippableInput input,EAresInputState state) { bot->ForceInputState(input,state); }
    void Fire(AShooterCharacter* bot,FEntry& s,bool on)
    {
        if(on)s.LastFireAt=s.LastFrame;
        if(on==s.Trigger)return;
        Input(bot,EAresEquippableInput::EquippableInput_Primary_Trigger,on?EAresInputState::Pressed:EAresInputState::Released);
        s.Trigger=on; if(on)++s.ShotsRequested;
    }
    void SetCrouch(AShooterCharacter* bot,FEntry& s,bool on)
    {
        if(on==s.Crouched)return;
        if(on)bot->Crouch(false); else bot->UnCrouch(false);
        s.Crouched=on;
    }

    FVector Follow(UWorld* world,AShooterCharacter* bot,FEntry& s,const FVector& goal,double now)
    {
        const FVector here=bot->K2_GetActorLocation();
        if(Length3(goal-s.PathGoal)>120 || now>=s.Repath) {
            s.Repath=now+0.7; s.PathGoal=goal; s.Path.clear(); s.Waypoint=1;
            auto path=UNavigationSystemV1::FindPathToLocationSynchronously(world,here,goal,bot,nullptr);
            if(path && path->IsValid())for(auto p:path->PathPoints)s.Path.push_back(p);
            // recast shortcut the capsule cant climb, go straight at the node
            for(size_t k=0;k+1<s.Path.size();++k) {
                const FVector a=s.Path[k], b=s.Path[k+1];
                if(b.Z-a.Z>90.f && b.Z-a.Z>0.85f*Length2(b-a)){s.Path.clear();s.SteepPaths++;break;}
            }
        }
        if(s.Path.empty())return Length2(goal-here)>30?Unit(goal-here):FVector{};
        // check Z too or stairs that double back skip a waypoint
        auto reached=[&](const FVector& p){FVector d=p-here;d.Z+=HalfHeight(bot);return Length2(d)<55&&fabsf(d.Z)<70;};
        while(s.Waypoint<s.Path.size() && reached(s.Path[s.Waypoint]))++s.Waypoint;
        if(s.Waypoint<s.Path.size()) {
            return Unit(s.Path[s.Waypoint]-here);
        }
        if(Length2(goal-here)>30 && Length2(goal-here)<400)return Unit(goal-here);
        return {};
    }

    int NextRouteNode(FEntry& s,const TacticalGraph& g,int here,int goal,const FTeam& team,double now)
    {
        if(goal<0||here<0)return -1;
        auto on=std::find(s.Route.begin(),s.Route.end(),here);
        if(goal!=s.RouteGoal||now>=s.Reroute||on==s.Route.end()) {
            s.RouteGoal=goal; s.Reroute=now+1.5;
            std::vector<float> penalty=team.Penalty.size()==(size_t)g.Count()?team.Penalty:std::vector<float>(g.Count(),0.f);
            // stuck spots, navmesh agent is smaller than us
            for(auto it=s.Avoid.begin();it!=s.Avoid.end();) {
                if(it->second<now){it=s.Avoid.erase(it);continue;}
                if(it->first>=0&&it->first<g.Count())penalty[it->first]+=6000.f;
                ++it;
            }
            BadSpots.erase(std::remove_if(BadSpots.begin(),BadSpots.end(),[&](const FBadSpot& b){return b.Until<now;}),BadSpots.end());
            std::vector<int> around;
            for(auto& b:BadSpots){g.Within(b.At.X,b.At.Y,b.At.Z,260.f,around);for(int n:around)penalty[n]+=8000.f;}
            for(int n:s.AvoidRoute)if(n>=0&&n<g.Count()&&n!=goal)penalty[n]+=2500.f;
            s.Route=g.Route(here,goal,penalty.data());
            on=std::find(s.Route.begin(),s.Route.end(),here);
            if(on==s.Route.end())return s.LastNext=goal;
        }
        size_t i=on-s.Route.begin();
        return s.LastNext=s.Route[std::min(i+1,s.Route.size()-1)];
    }

    // Hot is too low for slowing down, bots walked everywhere
    constexpr float Worth=0.03f;

    bool JumpEdge(AShooterCharacter* bot,FEntry& s,const TacticalGraph& g,int node,double now,FVector& move,float& speed)
    {
        auto m=bot->CharacterMovement; if(!m)return false;
        const FVector here=bot->K2_GetActorLocation();
        const bool airborne=m->MovementMode==EMovementMode::MOVE_Falling;
        if(airborne && s.JumpTo>=0 && now-s.JumpedAt<2.0) {
            move=Unit(NodeFloor(g,s.JumpTo)-here); move.Z=0; speed=1.f; s.OnJump=true; return true;
        }
        if(!airborne && s.JumpTo>=0 && now-s.JumpedAt>0.2) {
            // landing is often a node past the planned one
            if(node!=s.JumpFrom){s.JumpTries[(long long)s.JumpFrom*100000+s.JumpTo]=0;
                Ares::Log("botjump: %s landed on node %d (planned %d)",Short(bot).c_str(),node,s.JumpTo);}
            s.JumpTo=-1;
        }
        if(node<0||s.LastNext<0||s.LastNext==node)return false;
        const TacticalGraph::Link* jump=nullptr;
        for(auto& k:g.Links[node])if(k.To==s.LastNext&&k.Jump){jump=&k;break;}
        if(!jump)return false;
        const int to=jump->To;
        const long long key=(long long)node*100000+to;
        if(s.JumpTries[key]>=3){s.Avoid[to]=now+30;s.JumpTries[key]=0;s.Reroute=0;
            Ares::Log("botjump: %s gives up on the jump %d -> %d",Short(bot).c_str(),node,to);return false;}
        const FVector a=NodeFloor(g,node), b=NodeFloor(g,to);
        FVector dir=b-a; dir.Z=0; const float run=Length2(dir); if(run<1)return false; dir=dir*(1.f/run);
        const FVector feet=here-FVector{0,0,HalfHeight(bot)};
        const FVector d=feet-a; const float along=d.X*dir.X+d.Y*dir.Y, side=fabsf(d.X*dir.Y-d.Y*dir.X);
        s.OnJump=true;
        const float vz=m->JumpZVelocity, grav=-m->GetGravityZ(), rise=b.Z-a.Z;
        const float t=grav>0?(vz+sqrtf(std::max(0.f,vz*vz-2*grav*rise)))/grav:0.5f;
        const float need=run/std::max(0.1f,t);
        const FVector vel=bot->GetVelocity(); const float ahead=vel.X*dir.X+vel.Y*dir.Y;
        // standing jumps go straight up, back off for a run-up
        if(along>-20 && ahead<need*0.5f && now>=s.JumpBackoffUntil+0.8)s.JumpBackoffUntil=now+0.7;
        if(along<-160||side>40||now<s.JumpBackoffUntil) {
            const FVector p=a-dir*130.f;
            move=Unit(FVector{p.X-feet.X,p.Y-feet.Y,0}); speed=0.7f; return true;
        }
        speed=std::clamp(need/std::max(1.f,m->MaxWalkSpeed*0.92f)*1.15f,0.35f,1.f);
        move=dir;
        if(along>-40 && ahead>=need*0.75f && now>=s.NextJump && m->MovementMode==EMovementMode::MOVE_Walking) {
            bot->Jump(); s.NextJump=now+1.0; s.JumpedAt=now; s.JumpFrom=node; s.JumpTo=to; ++s.JumpTries[key];
            Ares::Log("botjump: %s jumps %.0f %.0f %.0f -> %.0f %.0f %.0f (rise %.0f, run-up %.0f%%)",Short(bot).c_str(),a.X,a.Y,a.Z,b.X,b.Y,b.Z,rise,speed*100);
        }
        return true;
    }

    // slicing the pie
    void ClearTowards(UWorld* world,AShooterCharacter* bot,FEntry& s,const TacticalGraph& g,FTeam& team,int goal,double now,
                      FVector& moveGoal,FVector& aim,float& speed)
    {
        const FVector here=bot->K2_GetActorLocation(); const int node=NodeOf(&g,bot);
        const int next=NextRouteNode(s,g,node,goal,team,now);
        if(next<0){moveGoal=goal>=0?NodeFloor(g,goal)+FVector{0,0,HalfHeight(bot)}:here;return;}
        moveGoal=NodeFloor(g,next)+FVector{0,0,HalfHeight(bot)};
        std::vector<int> newly;
        BotTactics::NewlyExposed(g,node,next,team.Hot,&newly);
        std::sort(newly.begin(),newly.end(),[&](int a,int b){return team.Threat[a]>team.Threat[b];});
        if(newly.size()>6)newly.resize(6);
        s.Slice=newly;
        if(newly.empty()) {
            int pre=-1; float best=1e30f, danger=1e30f;
            for(int h:team.Hot)if(g.Sees(next,h)){float d=g.Dist(next,h);if(d<best){best=d;pre=h;}if(team.Threat[h]>=Worth)danger=std::min(danger,d);}
            aim=pre>=0?NodeEye(g,pre):NodeEye(g,next)+(NodeEye(g,next)-here)*2.f;
            speed=danger<2000?0.5f:1.f; s.Pace=danger<2000?"walk: threat in view":"run";
            return;
        }
        // keep moving on the route, free steps got stuck in the A scaffold
        s.SliceCandidates=(int)newly.size();
        const FVector eyeNow=Eyes(bot);
        int revealed=-1;
        for(int h:newly)
            if(Sight(world,bot,nullptr,eyeNow,NodeEye(g,h))) {
                if(revealed<0)revealed=h;
                for(auto& t:team.Tracks)if(!t.Seen&&h<(int)t.B.P.size())t.B.P[h]*=0.05f;
            }
        s.SliceRevealed=revealed;
        if(revealed>=0 && team.Threat[revealed]>=Worth && revealed!=s.SliceAim && s.Checked[revealed]<now)
            {s.SliceAim=revealed;s.SlicePause=now+0.3;s.Checked[revealed]=now+4;}
        if(now<s.SlicePause && s.SliceAim>=0){moveGoal=here;aim=NodeEye(g,s.SliceAim);speed=0.45f;s.Pace="slice pause";return;}
        const FVector dir=Unit(moveGoal-here);
        int pre=newly[0]; float best=1e30f;
        for(int h:newly) {
            const FVector d=NodeEye(g,h)-eyeNow;
            const float score=fabsf(Wrap(YawTo(d)-YawTo(dir)))+Length3(d)*0.005f;
            if(score<best){best=score;pre=h;}
        }
        aim=NodeEye(g,pre);
        float nearest=1e30f;
        for(int h:newly)if(team.Threat[h]>=Worth)nearest=std::min(nearest,Length3(NodeEye(g,h)-eyeNow));
        bool recent=false;
        for(auto& t:team.Tracks)if((now-t.LastSeen<3||now-t.LastHeard<3)&&Length3(t.LastPos-here)<2500)recent=true;
        speed=nearest<1200||recent?0.45f:1.f; s.Pace=recent?"walk: recent contact":nearest<1200?"walk: slice near":"run (slicing)";
    }

    void Engage(UWorld* world,AShooterCharacter* bot,FEntry& s,AShooterCharacter* target,double now,float speedNow,FVector& move,bool& fire)
    {
        const FVector eye=Eyes(bot), head=Eyes(target), pos=bot->K2_GetActorLocation();
        const float distance=Length3(head-eye);
        s.Aim=s.Head?head+FVector{0,0,4}:head-FVector{0,0,42};
        if(s.Phase==0) {
            if(!BotPolicy::StillEnough(speedNow) && Rand()<Settings.Skill.Discipline+0.3f)move=Unit(bot->GetVelocity())*-1.f;
            const float tol=BotPolicy::FireTolerance(distance,s.Head);
            const bool confident=fabsf(s.ErrYaw)+fabsf(s.ErrPitch)<tol*2.5f;
            if(now>=s.ReactAt && BotPolicy::StillEnough(speedNow) && s.TrackError<std::max(0.6f,tol) && confident && s.Ammo!=0) {
                s.Phase=1; s.ShotsLeft=BotPolicy::BurstShots(s.Gun,distance); s.NextShot=now; s.PressUntil=0;
                SetCrouch(bot,s,distance>900&&s.ShotsLeft>=4&&Rand()<Settings.Skill.Discipline*0.5f);
            }
        }
        if(s.Phase==1) {
            if(s.Gun.Automatic) {
                fire=true;
                if(now>=s.NextShot){--s.ShotsLeft;s.NextShot=now+s.Gun.Interval;}
                if(s.ShotsLeft<0){fire=false;s.Phase=2;s.PhaseUntil=now+s.Gun.Recovery*(0.8f+0.5f*Rand());}
            } else {
                if(now>=s.NextShot) {
                    if(s.ShotsLeft--<=0){s.Phase=2;s.PhaseUntil=now+s.Gun.Recovery;}
                    else {s.NextShot=now+s.Gun.Interval;s.PressUntil=now+0.05;}
                }
                fire=now<s.PressUntil;
            }
        }
        if(s.Phase==2) {
            SetCrouch(bot,s,false);
            if(s.FromPeek && Length2(s.PeekOrigin-pos)<400) {
                // back behind the corner between bursts
                if(Length2(s.PeekOrigin-pos)>25)move=Unit(s.PeekOrigin-pos);
            } else if(distance<1800) {
                // only strafe to a side that keeps los
                if(s.StrafeSide==0) {
                    const float a=YawTo(head-eye)*Pi/180;
                    const FVector side{-sinf(a),cosf(a),0};
                    s.StrafeSide=-2;   // stand still
                    for(float sign:{s.Side,-s.Side})
                        if(Sight(world,bot,target,eye+side*(70.f*sign),head)){s.StrafeSide=sign;break;}
                }
                if(s.StrafeSide!=-2) {
                    const float a=YawTo(head-eye)*Pi/180;
                    move=FVector{-sinf(a)*s.StrafeSide,cosf(a)*s.StrafeSide,0};
                }
            }
            if(now>=s.PhaseUntil){s.Phase=0;s.StrafeSide=0;if(Rand()<0.7f)s.Side=-s.Side;}
        }
    }

    bool PlanPeek(UWorld* world,AShooterCharacter* bot,FEntry& s)
    {
        const FVector here=bot->K2_GetActorLocation(), lift=Eyes(bot)-here;
        const FVector aim=s.PeekAim;
        const FVector dir=Unit(aim-here); if(Length2(dir)<0.1f)return false;
        const FVector side{-dir.Y,dir.X,0};
        float bestOff=1e9f, bestSign=1.f;
        int walls=0, tested=0;
        const float offsets[]={35.f,60.f,90.f,130.f,180.f,240.f,320.f};
        // line trace, capsule sweep rejects every small step
        auto walkable=[&](const FVector& p){
            FVector floor{};
            return Sight(world,bot,nullptr,here,p) &&
                   UNavigationSystemV1::K2_ProjectPointToNavigation(world,p,&floor,nullptr,nullptr,FVector{40,40,150});
        };
        for(float sign:{1.f,-1.f}) for(float d:offsets) {
            const FVector p=here+side*(d*sign);
            ++tested;
            if(!walkable(p)){++walls;break;}
            if(Sight(world,bot,s.PeekEnemy.Get(),p+lift,aim)){if(d<bestOff){bestOff=d;bestSign=sign;}break;}
        }
        if(bestOff>1e8f) {
            static int Logged=0;
            if(Logged++<20)Ares::Log("botpeek: %s no corner toward %.0f %.0f (%d steps tested, %d walls) - moving on",
                                     Short(bot).c_str(),aim.X,aim.Y,tested,walls);
            return false;
        }
        float out=bestOff+25.f;                              // jiggle
        if(s.PeekKind==BotPolicy::PeekKind::Shoulder)out=std::max(20.f,bestOff-10.f);
        if(s.PeekKind==BotPolicy::PeekKind::Wide) {
            for(out=std::min(bestOff+220.f,420.f);out>bestOff+30.f;out-=60.f)
                if(walkable(here+side*(out*bestSign)))break;
        }
        s.PeekOrigin=here; s.PeekOut=here+side*(out*bestSign);
        Ares::Log("botpeek: %s %s peek: edge %.0f to the %s, out to %.0f",Short(bot).c_str(),BotPolicy::Name(s.PeekKind),
                  bestOff,bestSign>0?"left":"right",out);
        return true;
    }

    void Control(UWorld* world,AShooterCharacter* bot,FEntry& s)
    {
        const double now=UGameplayStatics::GetTimeSeconds(world);
        if(now==s.LastFrame)return; // processevent spams this per frame
        const float dt=s.LastFrame<0?0.016f:(float)std::clamp(now-s.LastFrame,0.0,0.05);
        s.LastFrame=now;
        auto teamId=TeamOf(bot); if(!teamId)return;
        FTeam& team=TeamFor(teamId);
        auto g=Map.G;
        const FVector here=bot->K2_GetActorLocation(), eye=Eyes(bot);
        auto gs=world->GameState?world->GameState->Cast<AShooterGameState>():nullptr;
        auto& ref=RoundReferee::View();
        const bool frozen=RoundReferee::Active() && (ref.Phase==RoundReferee::EPhase::Buy||ref.Phase==RoundReferee::EPhase::Ended);
        const bool active=s.Enabled && gs && gs->GameInfo.bGameplayActive && !frozen;

        auto LapT=std::chrono::steady_clock::now();
        auto Lap=[&](double& into){auto n=std::chrono::steady_clock::now();into+=std::chrono::duration<double,std::milli>(n-LapT).count();LapT=n;};
        if(now>=s.NextSense) {
            s.NextSense=now+0.1;
            AShooterCharacter* best=nullptr; float score=1e30f;
            int visible=0, allies=0;
            for(auto other:Roster(world)) {
                if(!Usable(other)||other==bot||!InWorld(other,world)||!Alive(other)||other==Ignored.Get())continue;
                auto theirs=TeamOf(other); if(!theirs)continue;
                const FVector oe=Eyes(other);
                if(theirs==teamId){if(Length3(oe-eye)<1500)++allies;continue;}
                if(Length3(oe-eye)>8000)continue;
                if(!Facing(bot,eye,oe,65.f)&&other!=s.Target.Get())continue;
                if(!Sight(world,bot,other,eye,oe))continue;
                ++visible;
                float d=Length3(oe-eye)*(other==s.Target.Get()?0.7f:1.f);
                if(d<score){score=d;best=other;}
            }
            s.VisibleEnemies=visible; s.NearAllies=allies;
            // got a kill, holders swap angle after
            if(auto was=s.Fighting.Get()) if(Usable(was)&&!Alive(was)) {
                s.Fighting=nullptr;
                if(now-s.LastFireAt<1.5){++s.Kills;
                    if(s.Want==Goal::Hold||s.Want==Goal::Guard)s.RelocateAt=now+1.2+1.5*Rand();}
            }
            if(best && best!=s.Target.Get()){s.Target=best;s.VisibleSince=-1;}
            if(best)s.Fighting=best;
            s.Health=bot->DamageHandler->GetRawHealth();
            if(s.Health<s.LastHealth-0.5f){s.HurtAt=now;s.HurtPos=bot->K2_GetActorLocation();}
            s.LastHealth=s.Health;
            s.Ammo=AmmoOf(world,bot,&s.Reserve);
            if(bot->Inventory)if(auto gun=bot->Inventory->GetCurrentEquippable()) {
                std::string cls=gun->Class->GetName();
                if(cls!=s.GunName){s.GunName=cls;s.Gun=BotPolicy::ProfileFor(cls.c_str());s.Magazine=0;}
                s.Magazine=std::max(s.Magazine,s.Ammo);
            }
        }
        auto target=s.Target.Get();
        if(target && (!Usable(target)||!Alive(target))) {target=nullptr;s.Target=nullptr;}
        const bool was=s.Visible;
        s.Visible=target && Sight(world,bot,target,eye,Eyes(target));
        if(s.Visible) {
            if(s.VisibleSince<0) {
                s.VisibleSince=now;
                const float off=fabsf(Wrap(YawTo(Eyes(target)-eye)-s.Rot.Yaw));
                // fast reacquire only for the same guy
                const bool same=s.LastTarget.Get()==target && now-s.LastSeen<0.8;
                s.ReactAt=now+(same?0.08+0.05*Rand():Settings.Skill.Reaction*(0.85f+0.35f*Rand())+std::min(0.25f,off*0.004f));
                s.LastTarget=target;
                s.NextThink=now;
                const float mag=Settings.Skill.Error*(0.6f+0.8f*Rand())+off*0.04f, ang=Rand()*2*Pi;
                s.ErrYaw=cosf(ang)*mag; s.ErrPitch=sinf(ang)*mag*0.6f;
                s.Head=Rand()<Settings.Skill.HeadRatio; s.Phase=0;
            }
            s.LastSeen=now; s.LastSeenPos=target->K2_GetActorLocation();
        } else if(was) s.VisibleSince=-1;
        const float decay=expf(-dt/std::max(0.05f,Settings.Skill.Settle));
        s.ErrYaw*=decay; s.ErrPitch*=decay;
        const int node=NodeOf(g,bot);

        Lap(Perf.Sense);
        if(now>=s.NextThink) {
            s.NextThink=now+0.17+0.07*Rand();
            BotPolicy::Situation st;
            st.Active=active; st.EnemyVisible=s.Visible; st.SinceSeen=now-s.LastSeen; st.SinceHurt=now-s.HurtAt;
            st.Health=s.Health; st.Ammo=s.Ammo; st.Magazine=s.Magazine; st.Reserve=s.Reserve;
            st.Exposed=g&&node>=0&&!team.Hot.empty()&&BotTactics::Exposure(*g,team.Threat,team.Hot,node)>0.25f;
            st.Allies=std::max(0,team.Alive-1); st.Enemies=team.Enemies;
            st.Want=s.Want;
            if(s.Want==Goal::Plant&&ref.Carrier!=bot)st.Want=Goal::Take;
            if(now<s.GoAt&&st.Want==Goal::Take)st.Want=Goal::None;
            st.Aggression=std::clamp(s.Aggression+(s.Temper-0.55f)*0.6f,0.1f,0.95f);
            if(now<s.TradeUntil)st.Aggression=std::max(st.Aggression,0.9f);
            if(s.GoalNode>=0&&g) st.AtGoal=Length2(here-NodeFloor(*g,s.GoalNode))<(s.Want==Goal::Plant||s.Want==Goal::Defuse?110.f:170.f);
            st.Committed=(s.Want==Goal::Plant&&ref.PlantProgress>RoundReferee::PlantSeconds*0.6f)||
                         (s.Want==Goal::Defuse&&ref.DefuseProgress>RoundReferee::DefuseSeconds*0.5f);
            st.VisibleEnemies=s.VisibleEnemies; st.NearAllies=s.NearAllies;
            {
                const FTrack* close=nullptr; float bd=2500.f; int local=0;
                for(auto& t:team.Tracks) {
                    auto w=t.Who.Get(); if(!w)continue;
                    const double age=now-std::max(t.LastSeen,t.LastHeard); if(age>6)continue;
                    const float d=Length3(t.LastPos-here);
                    if(d<3000)++local;
                    if(d<bd && !(s.Visible&&w==target)){bd=d;close=&t;}
                }
                // only peek if a nearby node can actually see them
                bool corner=false;
                if(close && g && node>=0) {
                    const int en=g->Nearest(close->LastPos.X,close->LastPos.Y,close->LastPos.Z-98,500);
                    if(en>=0) {
                        std::vector<int> around; g->Within(here.X,here.Y,here.Z-HalfHeight(bot),350.f,around);
                        for(int n:around)if(g->Sees(n,en)){corner=true;break;}
                    }
                }
                BotPolicy::PeekFacts pf;
                pf.Contact=close && corner && !s.Visible && now>=s.PeekFailUntil;
                if(close) {
                    pf.SinceInfo=now-std::max(close->LastSeen,close->LastHeard);
                    pf.EnemyReloading=close->LastAmmo>=0&&close->LastAmmo<=3;
                    pf.EnemyBusy=now-close->FiredAt<1.5&&now-s.HurtAt>1.5;
                    if(s.PeekEnemy.Get()!=close->Who.Get()){s.PeekEnemy=close->Who.Get();s.Jiggles=0;s.ContactSince=now;}
                    if(!(s.Now==Task::Peek&&s.PeekStage>0))s.PeekAim=close->LastPos+FVector{0,0,70};
                }
                pf.Health=s.Health; pf.Aggression=st.Aggression;
                pf.Defensive=s.Want==Goal::Hold||s.Want==Goal::Guard||s.Want==Goal::Regroup||s.Want==Goal::None;
                pf.HurtHere=now-s.HurtAt<6 && Length3(s.HurtPos-here)<350;
                pf.LocalAllies=s.NearAllies; pf.LocalEnemies=std::max(1,local); pf.Jiggles=s.Jiggles;
                st.Peek=(s.Now==Task::Peek&&s.PeekStage>0&&!s.Visible)?s.PeekKind:BotPolicy::ChoosePeek(pf);
            }
            Task next=BotPolicy::Choose(st);
            if(next!=s.Now) {
                if(next==Task::Retreat||next==Task::Reload)s.HasMove=false;
                s.FromPeek=s.Now==Task::Peek&&next==Task::Engage ? true : ((next==Task::Engage||next==Task::Recheck)?s.FromPeek:false);
                if(next==Task::Peek){s.PeekKind=st.Peek;s.PeekStage=0;}
                s.Now=next; s.TaskSince=now; s.Phase=0;
                Ares::Log("botai: %s -> %s%s%s (goal %s, %s)",Short(bot).c_str(),BotPolicy::Name(next),
                          next==Task::Peek?" ":"",next==Task::Peek?BotPolicy::Name(s.PeekKind):"",BotPolicy::Name(s.Want),s.Role.c_str());
            }
        }

        Lap(Perf.Decide);
        FVector move{}, moveGoal=here; bool wantMove=false, fire=false; float speed=1.f;
        const float speedNow=Length2(bot->GetVelocity());
        s.Aim=target?Eyes(target):eye+FVector{cosf(s.Rot.Yaw*Pi/180),sinf(s.Rot.Yaw*Pi/180),0}*1000.f;
        switch(s.Now) {
        case Task::Wait:
            if(g&&s.WatchNode>=0)s.Aim=NodeEye(*g,s.WatchNode);
            break;
        case Task::Engage:
            if(target)Engage(world,bot,s,target,now,speedNow,move,fire);
            break;
        case Task::Recheck:
            s.Aim=s.LastSeenPos+FVector{0,0,70};
            if(s.FromPeek && Length2(s.PeekOrigin-here)<400 && Length2(s.PeekOrigin-here)>25){move=Unit(s.PeekOrigin-here);speed=0.6f;}
            break;
        case Task::Retreat: case Task::Reload:
            if(!s.HasMove && g && node>=0) {
                int avoid=target?NodeOf(g,target):g->Nearest(s.LastSeenPos.X,s.LastSeenPos.Y,s.LastSeenPos.Z-98,500);
                int cover=BotTactics::Cover(*g,node,900,team.Threat,team.Hot,avoid);
                if(cover>=0){s.MoveGoal=NodeFloor(*g,cover)+FVector{0,0,HalfHeight(bot)};s.HasMove=true;}
            }
            if(s.HasMove){moveGoal=s.MoveGoal;wantMove=Length2(moveGoal-here)>60;}
            if(s.LastSeen>0)s.Aim=s.LastSeenPos+FVector{0,0,70};
            if(s.Now==Task::Reload||!s.Visible) {
                if(now>=s.ReloadAt && s.Ammo>=0 && s.Ammo<s.Magazine && s.Reserve>0) {
                    Input(bot,EAresEquippableInput::EquippableInput_Reload,EAresInputState::Pressed);
                    Input(bot,EAresEquippableInput::EquippableInput_Reload,EAresInputState::Released);
                    s.ReloadAt=now+2.5;
                }
            }
            break;
        case Task::Hold:
            if(g&&s.GoalNode>=0){moveGoal=NodeFloor(*g,s.GoalNode)+FVector{0,0,HalfHeight(bot)};wantMove=Length2(moveGoal-here)>90;speed=0.5f;}
            if(g&&s.WatchNode>=0)s.Aim=NodeEye(*g,s.WatchNode);
            break;
        case Task::Move:
            if(g&&s.GoalNode>=0&&node>=0) {
                int next=NextRouteNode(s,*g,node,s.GoalNode,team,now);
                moveGoal=NodeFloor(*g,next>=0?next:s.GoalNode)+FVector{0,0,HalfHeight(bot)};
                wantMove=true;
                int pre=-1; float best=1e30f, danger=1e30f;
                for(int h:team.Hot)if(next>=0&&g->Sees(next,h)){float d=g->Dist(next,h);if(d<best){best=d;pre=h;}if(team.Threat[h]>=Worth)danger=std::min(danger,d);}
                s.Aim=pre>=0?NodeEye(*g,pre):(s.WatchNode>=0?NodeEye(*g,s.WatchNode):moveGoal+FVector{0,0,70}+(moveGoal-here));
                speed=danger<1200?0.55f:1.f;
            }
            break;
        case Task::Clear: {
            int goal=s.GoalNode;
            if(g&&goal<0) {int p=-1;float b=1e30f;for(auto& t:team.Tracks){int k=t.B.Peak();if(k>=0&&node>=0&&g->Dist(node,k)<b){b=g->Dist(node,k);p=k;}}goal=p;}
            if(g&&goal>=0&&node>=0){ClearTowards(world,bot,s,*g,team,goal,now,moveGoal,s.Aim,speed);wantMove=Length2(moveGoal-here)>30;}
            else if(target){moveGoal=target->K2_GetActorLocation();wantMove=true;}
            else if(s.LastSeen>0){moveGoal=s.LastSeenPos;wantMove=true;}
            break; }
        case Task::Peek: {
            s.Aim=s.PeekAim;
            using BotPolicy::PeekKind;
            if(s.PeekStage==0) {
                if(!PlanPeek(world,bot,s)){s.PeekFailUntil=now+3;s.NextThink=now;break;}
                s.PeekStage=1; s.PeekUntil=now+(s.PeekKind==PeekKind::Wide?1.2:0.7);
            }
            if(s.PeekStage==1) {                       // out
                move=Unit(s.PeekOut-here); speed=s.PeekKind==PeekKind::Shoulder?0.55f:1.f;
                if(Length2(s.PeekOut-here)<25||now>s.PeekUntil) {
                    s.PeekStage=s.PeekKind==PeekKind::Wide?3:2; s.PeekUntil=now+0.8;
                }
            } else if(s.PeekStage==2) {                // back
                move=Unit(s.PeekOrigin-here);
                if(Length2(s.PeekOrigin-here)<25||now>s.PeekUntil){s.PeekStage=4;s.PeekUntil=now+0.35+0.5*Rand();++s.Jiggles;}
            } else if(s.PeekStage==3) {                // wide, stop and look
                if(!BotPolicy::StillEnough(speedNow))move=Unit(bot->GetVelocity())*-1.f;
                if(now>s.PeekUntil){s.Jiggles=9;s.PeekStage=0;s.PeekFailUntil=now+4;s.NextThink=now;}   // nobody there
            } else if(s.PeekStage==4) {                // pause
                if(now>s.PeekUntil){s.PeekStage=0;s.NextThink=now;if(s.Jiggles>=2)s.PeekFailUntil=now+3;}
            }
            break; }
        case Task::Plant:
            RoundReferee::WorkPlant(bot,dt);
            if(g&&s.WatchNode>=0)s.Aim=NodeEye(*g,s.WatchNode);
            SetCrouch(bot,s,true);
            break;
        case Task::Defuse:
            RoundReferee::WorkDefuse(bot,dt);
            SetCrouch(bot,s,true);
            break;
        }
        if(s.Now!=Task::Plant&&s.Now!=Task::Defuse&&s.Now!=Task::Engage)SetCrouch(bot,s,false);
        if(!active){move={};wantMove=false;fire=false;}
        s.OnJump=false;
        const bool jumpEdge=wantMove && Length2(move)<0.1f && g && JumpEdge(bot,s,*g,node,now,move,speed);
        if(jumpEdge) { s.ProgressAt=here; s.ProgressTime=now; }
        else if(wantMove && Length2(move)<0.1f) {
            move=Follow(world,bot,s,moveGoal,now);
            // jitter resets the stuck timer so check net progress too
            if(now-s.WindowStart>4) {
                if(s.WindowStart>0 && Length3(here-s.WindowAt)<150) {
                    if(s.LastNext>=0){s.Avoid[s.LastNext]=now+12;s.Reroute=0;s.Repath=0;}
                    if(s.Waypoint<s.Path.size()) {
                        const FVector at=s.Path[s.Waypoint];
                        bool known=false; for(auto& b:BadSpots)if(Length3(b.At-at)<200){b.Until=now+90;known=true;}
                        if(!known){BadSpots.push_back({at,now+90});Ares::Log("botmove: team routes avoid %.0f %.0f %.0f for 90 s (%s could not pass)",at.X,at.Y,at.Z,Short(bot).c_str());}
                    }
                    if(now>=s.NextOscLog) {
                        s.NextOscLog=now+10;
                        std::string pts;
                        for(size_t k=0;k<s.Path.size()&&k<6;++k){char b[64];snprintf(b,sizeof b," (%.0f %.0f %.0f)",s.Path[k].X,s.Path[k].Y,s.Path[k].Z);pts+=b;}
                        auto m=bot->CharacterMovement;
                        Ares::Log("botmove: %s path%s | step %.0f walkable-z %.2f mode %d max-walk %.0f speed %.0f",Short(bot).c_str(),pts.c_str(),
                            m?m->MaxStepHeight:-1.f,m?m->WalkableFloorZ:-1.f,m?(int)m->MovementMode:-1,m?m->MaxWalkSpeed:-1.f,Length2(bot->GetVelocity()));
                        Ares::Log("botmove: %s oscillating at %.0f %.0f %.0f task %s node %d next %d goal %d path %zu/%zu goal-dist %.0f blocked-by '%s'",
                            Short(bot).c_str(),here.X,here.Y,here.Z,BotPolicy::Name(s.Now),node,s.LastNext,s.GoalNode,s.Waypoint,s.Path.size(),
                            Length2(moveGoal-here),LastBlock.c_str());
                    }
                }
                s.WindowStart=now; s.WindowAt=here;
            }
            if(Length3(here-s.ProgressAt)>40){s.ProgressAt=here;s.ProgressTime=now;s.Stuck=0;}
            else if(now-s.ProgressTime>1.5){s.ProgressTime=now;++s.Stuck;s.Repath=0;s.Reroute=0;s.HasMove=false;
                if(s.LastNext>=0&&s.Stuck>=2){s.Avoid[s.LastNext]=now+10;Ares::Log("botmove: %s stuck near node %d, routing around it",Short(bot).c_str(),s.LastNext);}
                if(s.Stuck%2==0)s.Side=-s.Side;}
            if(s.Stuck>0 && Length2(move)>0.1f){float a=s.Side*0.9f;move=FVector{move.X*cosf(a)-move.Y*sinf(a),move.X*sinf(a)+move.Y*cosf(a),0};}
        } else { s.ProgressAt=here; s.ProgressTime=now; }

        Lap(Perf.Act);
        if(!s.RotInit){s.Rot=bot->K2_GetActorRotation();s.Rot.Pitch=0;s.RotInit=true;}
        {
            const FVector d=s.Aim-eye;
            const float wantYaw=YawTo(d)+(s.Now==Task::Engage?s.ErrYaw:0), wantPitch=PitchTo(d)+(s.Now==Task::Engage?s.ErrPitch:0);
            s.Rot.Yaw=Wrap(s.Rot.Yaw+s.Yaw.Update(dt,Wrap(wantYaw-s.Rot.Yaw),Settings.Skill.TurnRate));
            s.Rot.Pitch=std::clamp(s.Rot.Pitch+s.Pitch.Update(dt,wantPitch-s.Rot.Pitch,Settings.Skill.TurnRate),-80.f,80.f);
            s.Rot.Roll=0;
            s.TrackError=std::max(fabsf(Wrap(wantYaw-s.Rot.Yaw)),fabsf(wantPitch-s.Rot.Pitch));
            if(auto brain=bot->Controller)brain->SetControlRotation(s.Rot);
            if(fabsf(Wrap(s.Rot.Yaw-s.BodyYaw))>0.4f){s.BodyYaw=s.Rot.Yaw;bot->K2_SetActorRotation(FRotator{0,s.Rot.Yaw,0},false);}
        }

        Lap(Perf.Aim);
        s.HeldMove=FVector{};
        if(Length2(move)>0.1f) {
            // spread out or they jam in doorways
            FVector apart{};
            for(auto& [other,e]:Bots) {
                if(other==bot||!Usable(other)||!Alive(other)||TeamOf(other)!=teamId)continue;
                FVector d=here-other->K2_GetActorLocation(); d.Z=0; const float l=Length2(d);
                if(l>1&&l<140)apart=apart+Unit(d)*((140-l)/140);
            }
            if(Length2(apart)>0.05f && !s.OnJump)move=Unit(move)+apart*1.2f;
            if(s.OnJump){s.SteerAngle=0;s.NextSteer=now+0.1;}
            if(now>=s.NextSteer) {
                s.NextSteer=now+0.1;
                s.SteerAngle=0;
                const float drop=s.Waypoint<s.Path.size()?(here.Z-HalfHeight(bot))-s.Path[s.Waypoint].Z:0.f;
                bool clear=StepClear(world,bot,here,here+Unit(move)*45.f,drop>40?drop+20:0);
                if(!clear) {
                    ++s.BlockedFrames;
                    for(float degrees:{30.f,-30.f,60.f,-60.f,90.f,-90.f}) {
                        float a=degrees*s.Side*Pi/180;
                        FVector c{move.X*cosf(a)-move.Y*sinf(a),move.X*sinf(a)+move.Y*cosf(a),0};
                        if(StepClear(world,bot,here,here+Unit(c)*45.f,drop>40?drop+20:0)){s.SteerAngle=a;clear=true;break;}
                    }
                }
                // movement slides along walls anyway, dont freeze
                if(!clear) {
                    static std::unordered_map<std::string,int> Seen;
                    if(Seen[LastBlock]++%200==0)Ares::Log("botmove: %s step blocked: %s (x%d)",Short(bot).c_str(),LastBlock.c_str(),Seen[LastBlock]);
                }
            }
            if(s.SteerAngle!=0){const float a=s.SteerAngle;move=FVector{move.X*cosf(a)-move.Y*sinf(a),move.X*sinf(a)+move.Y*cosf(a),0};}
            // Tick refills this until movement consumes it
            s.HeldMove=Unit(move)*std::clamp(speed,0.2f,1.f); s.HeldWallUntil=Ares::Seconds()+0.06;
            bot->ControlInputVector=s.HeldMove;
            if(Ares::Profiling) {
                s.DiagTime+=dt; s.DiagDist+=Length2(bot->GetVelocity())*dt; s.DiagInput+=std::clamp(speed,0.2f,1.f)*dt;
                ++s.DiagCalls; if(bot->CharacterMovement&&Length2(bot->CharacterMovement->GetCurrentAcceleration())<1)++s.DiagIdle;
                const FVector u=Unit(move);
                if(Length2(s.DiagMove)>0.1f)s.DiagTurn+=acosf(std::clamp(u.X*s.DiagMove.X+u.Y*s.DiagMove.Y,-1.f,1.f))*180/Pi;
                s.DiagMove=u;
                if(Length3(moveGoal-s.DiagGoal)>1){++s.DiagGoals;s.DiagGoal=moveGoal;}
                if(now>=s.DiagAt) {
                    if(s.DiagTime>0.5)Ares::Log("botpace: %s %s/%s speed %.0f of %.0f requested, input turns %.0f deg/s, %d move goals in %.1fs, path %zu, %.0f controls/s, %d%% without acceleration, at %.0f %.0f %.0f",
                        Short(bot).c_str(),BotPolicy::Name(s.Now),s.Pace,s.DiagDist/s.DiagTime,
                        (bot->CharacterMovement?bot->CharacterMovement->MaxWalkSpeed:0)*s.DiagInput/s.DiagTime,s.DiagTurn/s.DiagTime,s.DiagGoals,s.DiagTime,s.Path.size(),
                        s.DiagCalls/s.DiagTime,s.DiagCalls?100*s.DiagIdle/s.DiagCalls:0,here.X,here.Y,here.Z);
                    s.DiagAt=now+2; s.DiagTime=s.DiagDist=s.DiagTurn=s.DiagInput=0; s.DiagGoals=s.DiagCalls=s.DiagIdle=0;
                }
            }
        }
        Fire(bot,s,active&&fire&&s.Visible);

        Lap(Perf.Move);
        if(now>=s.Report) {
            s.Report=now+1;
            auto dir=fs::path(Ares::Config().LogPath).parent_path()/"bots";
            std::error_code ec; fs::create_directories(dir,ec);
            std::ofstream out(dir/(std::to_string(bot->Index)+".json"));
            out<<"{\"name\":\""<<Short(bot)<<"\",\"role\":\""<<s.Role<<"\",\"kills\":"<<s.Kills<<",\"peek\":\""<<(s.Now==Task::Peek?BotPolicy::Name(s.PeekKind):"")<<"\",\"visible_enemies\":"<<s.VisibleEnemies<<",\"time\":"<<now<<",\"task\":\""<<BotPolicy::Name(s.Now)<<"\",\"goal\":\""<<BotPolicy::Name(s.Want)
               <<"\",\"goal_node\":"<<s.GoalNode<<",\"watch_node\":"<<s.WatchNode<<",\"node\":"<<node<<",\"active\":"<<active
               <<",\"pace\":\""<<s.Pace<<"\",\"velocity\":"<<(int)Length2(bot->GetVelocity())<<",\"visible\":"<<s.Visible<<",\"health\":"<<s.Health<<",\"ammo\":"<<s.Ammo<<",\"reserve\":"<<s.Reserve
               <<",\"gun\":\""<<s.GunName<<"\",\"target_health\":"<<(target&&target->DamageHandler?target->DamageHandler->GetRawHealth():-1)
               <<",\"trigger_requests\":"<<s.ShotsRequested<<",\"path_points\":"<<s.Path.size()<<",\"blocked_frames\":"<<s.BlockedFrames
               <<",\"stuck\":"<<s.Stuck<<",\"steep_paths\":"<<s.SteepPaths<<",\"slice\":"<<s.Slice.size()<<",\"slice_candidates\":"<<s.SliceCandidates<<",\"slice_revealed\":"<<s.SliceRevealed<<",\"aim_pitch\":"<<s.Rot.Pitch<<",\"read_pitch\":"
               <<(bot->Controller?bot->Controller->GetControlRotation().Pitch:0)<<",\"track_error\":"<<s.TrackError
               <<",\"team_hot_nodes\":"<<team.Hot.size()<<",\"position\":["<<here.X<<','<<here.Y<<','<<here.Z<<"]}";
        }
    }
}

void LocalBots::Register(AShooterCharacter* Bot, const FOptions& Options)
{
    if (!Bot) return;
    FEntry Fresh{};
    Fresh.Options = Options;
    Fresh.Ref=Bot; Fresh.World=UWorld::GetWorld();
    Fresh.Next = Ares::Seconds() + 1.0;
    Fresh.Sideways = (Bots.size() % 2) ? 1.f : -1.f;
    Fresh.Side = Fresh.Sideways;
    Fresh.Temper = 0.3f + 0.55f * Rand();
    Bots[Bot] = Fresh;
    RosterCache.At = -100;
    HookIsLocallyControlled(Bot);

    if (auto Movement = Bot->CharacterMovement)
        Movement->GravityScale = Options.bGravity ? 1.f : 0.f;
    // UpdateControlRotation wipes pitch every tick otherwise
    if (auto Brain = Bot->Controller ? Bot->Controller->Cast<AAIController>() : nullptr)
        Brain->bSetControlRotationFromPawnOrientation = 0;
    if (auto Brain = Bot->Controller)
        Brain->SetControlRotation(FRotator{0.f, Bot->K2_GetActorRotation().Yaw, 0.f});
}

// destroy reenters processevent -> Tick, erase first then destroy
int LocalBots::ClearBodies()
{
    FReentrancyGuard Guard(Updating);
    auto World = UWorld::GetWorld();
    auto Local = World ? UGameplayStatics::GetPlayerController(World, 0) : nullptr;
    auto Ours = Local ? Local->Pawn : nullptr;
    std::vector<AShooterCharacter*> Doomed;
    for (AShooterCharacter* Body : GetObjectsOfClass<AShooterCharacter>())
        if (Usable(Body) && InWorld(Body,World) && Body != Ours && !Alive(Body)) Doomed.push_back(Body);
    for (auto Body : Doomed) Bots.erase(Body);
    for (auto Body : Doomed) DestroyCharacter(Body);
    return (int)Doomed.size();
}

int LocalBots::ClearTactical()
{
    FReentrancyGuard Guard(Updating);
    std::vector<AShooterCharacter*> Doomed;
    for(auto it=Bots.begin();it!=Bots.end();) {
        if(it->second.Options.Behaviour!=EBehaviour::Combat){++it;continue;}
        auto bot=it->second.Ref.Get();
        if(bot && Usable(bot)){Fire(bot,it->second,false);Doomed.push_back(bot);}
        it=Bots.erase(it);
    }
    for(auto bot:Doomed)DestroyCharacter(bot);
    return (int)Doomed.size();
}

std::vector<AShooterCharacter*> LocalBots::Tactical()
{
    std::vector<AShooterCharacter*> out;
    auto world=UWorld::GetWorld();
    for(auto& [bot,s]:Bots)if(s.Ref.Get()==bot&&s.World.Get()==world&&s.Options.Behaviour==EBehaviour::Combat&&Usable(bot))out.push_back(bot);
    return out;
}

void LocalBots::Tick()
{
    if (Ares::Config().Mode != Ares::EMode::Standalone || Bots.empty()) return;
    // refill input every call or some movement ticks get none and legs float
    if (!Updating)
    {
        static auto LastHold = std::chrono::steady_clock::time_point{};
        const auto Clock = std::chrono::steady_clock::now();
        if (Clock - LastHold >= std::chrono::microseconds(200))
        {
            LastHold = Clock;
            const double Wall = Ares::Seconds();
            for (auto& [Bot, State] : Bots)
                if (Wall < State.HeldWallUntil && State.Ref.Get() == Bot && !Bot->bActorIsBeingDestroyed &&
                    Bot->ControlInputVector.X == 0 && Bot->ControlInputVector.Y == 0)
                    Bot->ControlInputVector = State.HeldMove;
        }
    }
    // called from every processevent, only run once per game frame
    {
        static auto LastRun = std::chrono::steady_clock::time_point{};
        const auto Clock = std::chrono::steady_clock::now();
        if (Clock - LastRun < std::chrono::milliseconds(1)) return;
        LastRun = Clock;
    }
    FReentrancyGuard Guard(Updating);
    if (!Guard.Entered()) return;
    {
        static double LastFrameTime = -1;
        auto W = UWorld::GetWorld();
        if (!W) return;
        const double T = UGameplayStatics::GetTimeSeconds(W);
        if (T == LastFrameTime) return;
        LastFrameTime = T;
    }
    FTimer TotalTimer{Perf.Total};
    if(Ares::Seconds()>=Perf.Next) {
        if(Perf.Next>0&&Ares::Profiling)Ares::Log("perf: bots %.1f ms/s total - character scans %.1f ms/s (%.0f scans/s over %d objects), team update %.1f ms/s, per-bot control %.1f ms/s",
            Perf.Total/5,Perf.Scan/5,Perf.Scans/5.0,Perf.Objects,Perf.Team/5,Perf.Control/5);
        if(Perf.Next>0&&Ares::Profiling)Ares::Log("perf: control = sense %.1f, decide %.1f, act %.1f, aim %.1f, move %.1f ms/s",Perf.Sense/5,Perf.Decide/5,Perf.Act/5,Perf.Aim/5,Perf.Move/5);
        Perf=FPerf{}; Perf.Next=Ares::Seconds()+5;
    }

    auto World = UWorld::GetWorld();
    if(!World)return;
    auto Local = UGameplayStatics::GetPlayerController(World, 0);
    auto Ours = Local ? Local->Pawn : nullptr;

    const double Now = Ares::Seconds();
    bool AnyTactical=false;
    for (auto It = Bots.begin(); It != Bots.end();)
    {
        auto Bot = It->first; auto& State = It->second;
        if (State.Ref.Get()!=Bot || State.World.Get()!=World || !Usable(Bot)) { It = Bots.erase(It); continue; }
        if(State.Options.Behaviour==EBehaviour::Combat)AnyTactical=true;
        ++It;
    }
    {
        int k=0;
        for(auto& [bot,st]:Bots)if(k<64)BotSlots[k++].store(bot,std::memory_order_relaxed);
        for(;k<64;++k)BotSlots[k].store(nullptr,std::memory_order_relaxed);
    }
    {
        if(AnyTactical)BuildMap(World);
        static double NextTeams=0;
        const double game=UGameplayStatics::GetTimeSeconds(World);
        if(game>=NextTeams || game<NextTeams-5) {
            NextTeams=game+0.2;
            FTimer teamTimer{Perf.Team};
            // bodies keep collision here, turn it off
            {
                static std::unordered_map<AShooterCharacter*,bool> Ghosted;
                for(auto c:Roster(World)) {
                    if(!Usable(c)||!InWorld(c,World))continue;
                    if(Alive(c)){Ghosted.erase(c);continue;}
                    if(Ghosted[c])continue;
                    Ghosted[c]=true;
                    if(auto inv=c->Inventory) {
                        AAresEquippable* best=nullptr;
                        auto items=inv->GetEquippables();
                        for(auto slot:{EAresItemSlot::Primary,EAresItemSlot::Secondary}) {
                            for(auto e:items)if(e&&e->EquippableSlot==slot&&e->bCanBeDropped){best=e;break;}
                            if(best)break;
                        }
                        items.Free();
                        std::string report; AAresOnGroundEquippable* pickup=nullptr;
                        if(best&&!LocalDrop::Drop(inv,best,report,&pickup))Ares::Log("botworld: %s's gun did not drop: %s",Short(c).c_str(),report.c_str());
                        if(best&&pickup) {
                            // dead body never unequips so gun stays on the hand socket
                            best->K2_DetachFromActor(EDetachmentRule::KeepWorld,EDetachmentRule::KeepWorld,EDetachmentRule::KeepWorld);
                            auto parts=best->GetComponentsByClass(USceneComponent::StaticClass());
                            for(auto part:parts) {
                                auto scene=(USceneComponent*)part; if(!scene)continue;
                                auto parent=scene->GetAttachParent();
                                if(parent&&parent->GetOwner()==c)
                                    scene->K2_DetachFromComponent(EDetachmentRule::KeepWorld,EDetachmentRule::KeepWorld,EDetachmentRule::KeepWorld,false);
                            }
                            parts.Free();
                            pickup->OnRep_MyEquippable();
                            // pickup doesnt grab it either, snap 3P onto it and hide 1P
                            if(auto floor=pickup->RootComponent) {
                                auto all=best->GetComponentsByClass(USceneComponent::StaticClass());
                                for(auto part:all) {
                                    auto scene=(USceneComponent*)part; if(!scene)continue;
                                    const std::string n=scene->GetName();
                                    if(n=="SceneRoot3P") {
                                        scene->K2_AttachToComponent(floor,FName(),EAttachmentRule::SnapToTarget,EAttachmentRule::SnapToTarget,EAttachmentRule::KeepWorld,false);
                                        FHitResult h{}; scene->K2_SetRelativeLocation(FVector{0,0,8},false,&h,true);
                                    } else if(n=="WeaponMesh1P") scene->SetVisibility(false,true);
                                }
                                all.Free();
                            }
                            auto pp=pickup->K2_GetActorLocation();
                            Ares::Log("botworld: %s dropped %s on the floor at %.0f %.0f %.0f",Short(c).c_str(),best->GetName().c_str(),pp.X,pp.Y,pp.Z);
                        }
                    }
                    if(auto m=c->CharacterMovement){m->StopMovementImmediately();m->DisableMovement();m->GravityScale=0;}
                    if(c->CapsuleComponent)c->CapsuleComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                    if(c->Mesh)c->Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
                    Ares::Log("botworld: %s's body no longer blocks movement",Short(c).c_str());
                }
            }
            if(AnyTactical) {
            auto all=Fighters(World);
            for(auto& [bot,s]:Bots)if(s.Options.Behaviour==EBehaviour::Combat&&Usable(bot))if(auto t=TeamOf(bot))TeamFor(t);
            for(auto& team:Teams) {
                UpdateBeliefs(World,team,all,game);
                if(RoundReferee::Active())PlanRound(team,World,game); else PlanSkirmish(team,World,game);
                ApplyOverrides(team,World,game);
            }
            ReadBrain(game);
            WriteBrainState(World,game);
            }
        }
    }

    for (auto& [Bot, State] : Bots)
    {
        if (!Usable(Bot)) continue;
        if (!Alive(Bot)) { Fire(Bot,State,false); continue; }
        if (State.Options.Behaviour==EBehaviour::Combat) { FTimer t{Perf.Control}; Control(World,Bot,State); continue; }

        if (State.Options.bWatch && Ours && Ours != Bot)
        {
            const FVector Here = Bot->K2_GetActorLocation();
            const FVector There = Ours->K2_GetActorLocation();
            const float Yaw = atan2f(There.Y - Here.Y, There.X - Here.X) * 180.f / Pi;
            Bot->K2_SetActorRotation(FRotator{0.f, Yaw, 0.f}, false);
            if (auto Brain = Bot->Controller) Brain->SetControlRotation(FRotator{0.f, Yaw, 0.f});
        }
        if (State.Options.Behaviour == EBehaviour::Still) continue;

        const FRotator Facing = Bot->K2_GetActorRotation();
        const float Yaw = Facing.Yaw * Pi / 180.0f;
        const FVector Forward{cosf(Yaw), sinf(Yaw), 0.f};
        const FVector Right{-sinf(Yaw), cosf(Yaw), 0.f};
        switch (State.Options.Behaviour)
        {
        case EBehaviour::Walk: Bot->AddMovementInput(Forward, 1.f, true); break;
        case EBehaviour::Jump:
            Bot->AddMovementInput(Forward, 0.5f, true);
            if (Now >= State.Next) { Bot->Jump(); State.Next = Now + 1.5; }
            break;
        case EBehaviour::Strafe:
            if (Now >= State.Next) { State.Sideways = -State.Sideways; State.Next = Now + 0.35; }
            Bot->AddMovementInput(Right, State.Sideways * 0.6f, true);
            break;
        default: break;
        }
        if (!State.Options.bWatch)
            if (auto Brain = Bot->Controller) Brain->SetControlRotation(FRotator{0.f, Facing.Yaw, 0.f});
    }
}

std::string LocalBots::Order(const std::string& text)
{
    FReentrancyGuard Guard(Updating);
    if(!Guard.Entered())return "botorder: controller busy, retry";
    if(text=="stop" || text=="clear") {
        int count=0;
        std::vector<AShooterCharacter*> doomed;
        for(auto it=Bots.begin();it!=Bots.end();) {
            auto bot=it->second.Ref.Get();auto& s=it->second;
            if(!bot || s.World.Get()!=UWorld::GetWorld() || s.Options.Behaviour!=EBehaviour::Combat){++it;continue;}
            Fire(bot,s,false);s.Enabled=false;++count;
            if(text=="clear") {doomed.push_back(bot);it=Bots.erase(it);}else ++it;
        }
        for(auto bot:doomed)DestroyCharacter(bot);
        return "botorder: "+text+" applied to "+std::to_string(count)+" tactical bots";
    }
    BotPolicy::Order order;
    if(text.rfind("goto",0)==0) {
        float x=0,y=0,z=0;
        if(sscanf(text.c_str()+4,"%f %f %f",&x,&y,&z)!=3)return "botorder: goto x y z";
        auto g=Map.G; if(!g)return "botorder: no tactical map";
        const int n=g->Nearest(x,y,z,600.f); if(n<0)return "botorder: no tactical node near that spot";
        int count=0; const double now=UWorld::GetWorld()?UGameplayStatics::GetTimeSeconds(UWorld::GetWorld()):0;
        for(auto& [bot,s]:Bots)if(s.Options.Behaviour==EBehaviour::Combat){s.ManualNode=n;s.ManualUntil=now+40;s.Reroute=0;++count;}
        auto& N=g->Nodes[n];
        return "botorder: "+std::to_string(count)+" bots go to node "+std::to_string(n)+" at "+std::to_string((int)N.X)+" "+std::to_string((int)N.Y)+" "+std::to_string((int)N.Z)+" for 40 s";
    }
    if(text=="hold")order=BotPolicy::Order::Hold;
    else if(text=="push")order=BotPolicy::Order::Push;
    else if(text=="auto")order=BotPolicy::Order::Auto;
    else return "botorder: use hold, push, auto, stop or clear";
    Settings.Intent=order;
    int count=0;
    for(auto& [bot,s]:Bots)if(s.Ref.Get()==bot && s.World.Get()==UWorld::GetWorld() && s.Options.Behaviour==EBehaviour::Combat) {
        s.Enabled=true; s.GoAt=0; s.GoalNode=-1; ++count;
    }
    return "botorder: "+text+" applied to "+std::to_string(count)+" tactical bots"+(RoundReferee::Active()?" (rounds are running: the round plan takes precedence)":"");
}

std::string LocalBots::Configure(const std::string& Command, const std::string& Argument)
{
    if(Command=="botskill") {
        if(Argument!="easy"&&Argument!="normal"&&Argument!="hard"&&Argument!="pro")return "botskill: easy, normal, hard or pro (now "+std::string(Settings.Skill.Name)+")";
        Settings.Skill=BotPolicy::SkillFor(Argument.c_str());
        char b[200]; snprintf(b,sizeof b,"botskill: %s - reaction %.2fs, turn %.0f deg/s, error %.1f deg, head %.0f%%",Settings.Skill.Name,
                              Settings.Skill.Reaction,Settings.Skill.TurnRate,Settings.Skill.Error,Settings.Skill.HeadRatio*100);
        return b;
    }
    if(Command=="botawareness") {
        if(Argument=="exact")Settings.Exact=true; else if(Argument=="fair")Settings.Exact=false;
        else return std::string("botawareness: fair (sight, sound, teammates) or exact (always knows positions); now ")+(Settings.Exact?"exact":"fair");
        return std::string("botawareness: ")+(Settings.Exact?"exact - bots always know where enemies are":"fair - bots know what their team sees and hears");
    }
    if(Command=="botaudio" && Argument.rfind("local",0)==0) {
        // botaudio local<rva>
        const uintptr_t rva=strtoull(Argument.c_str()+5,nullptr,16);
        for(auto& r:RemoteCallers){uintptr_t v=rva;if(r.compare_exchange_strong(v,0)){char b[80];snprintf(b,sizeof b,"botaudio: caller +0x%llX sees bots as local again",(unsigned long long)rva);return b;}}
        return "botaudio: that caller was not in the remote list";
    }
    if(Command=="botaudio" && Argument.rfind("remote",0)==0) {
        // botaudio remote<rva>
        const uintptr_t rva=strtoull(Argument.c_str()+6,nullptr,16);
        for(auto& r:RemoteCallers){uintptr_t z=0;if(r.compare_exchange_strong(z,rva)){char b[80];snprintf(b,sizeof b,"botaudio: caller +0x%llX now sees bots as remote",(unsigned long long)rva);return b;}}
        return "botaudio: remote list full";
    }
    if(Command=="botaudio") {
        std::ostringstream o;
        auto world=UWorld::GetWorld(); if(!world)return "botaudio: no world";
        for(auto c:Roster(world)) {
            auto at=c->K2_GetActorLocation();
            o<<Short(c)<<(Bots.count(c)?" [bot]":" [player]")<<" at "<<(int)at.X<<" "<<(int)at.Y<<" "<<(int)at.Z
             <<" local="<<c->IsLocallyControlled()<<" firstperson="<<c->IsFirstPerson();
            if(auto f=(UFootstepsComponent*)c->GetComponentByClass(UFootstepsComponent::StaticClass())) {
                o<<" footstep-audible="<<f->FootstepAudibleDistance<<" events="<<f->RunFootstepEvents.Num();
                if(f->RunFootstepEvents.Num()&&f->RunFootstepEvents[0])o<<" ("<<f->RunFootstepEvents[0]->GetName()<<")";
            }
            auto aks=c->GetComponentsByClass(UAkComponent::StaticClass());
            o<<" ak="<<aks.Num();
            for(auto a:aks)if(auto ak=(UAkComponent*)a)o<<" ["<<ak->GetName()<<" atten="<<ak->AttenuationScalingFactor<<"]";
            aks.Free();
            o<<"\n";
        }
        {
            std::lock_guard<std::mutex> l(CallerLock);
            std::vector<std::pair<long,uintptr_t>> v; for(auto& [c,n]:Callers)v.push_back({n,c});
            std::sort(v.rbegin(),v.rend());
            o<<"IsLocallyControlled callers for bots:";
            for(auto& [n,c]:v){char b[48];snprintf(b,sizeof b," +0x%llX x%ld",(unsigned long long)c,n);o<<b;}
            o<<"\n";
        }
        Ares::Log("botaudio:\n%s",o.str().c_str());
        return o.str();
    }
    std::ostringstream o;
    o<<"bots: skill "<<Settings.Skill.Name<<", awareness "<<(Settings.Exact?"exact":"fair")<<", tactical map "
     <<(Map.G?std::to_string(Map.G->Count())+" nodes":"missing (run tacbake)");
    for(auto& t:Teams)if(t.Id)o<<"\n  team "<<t.Id->GetTeamName().ToString()<<": "<<t.Alive<<" alive, "<<t.Tracks.size()<<" tracked enemies, "
        <<t.Hot.size()<<" threatened nodes"<<(t.Site>=0&&t.Site<(int)Map.Sites.size()?std::string(", site ")+Map.Sites[t.Site].Name:"");
    for(auto& [bot,s]:Bots)if(Usable(bot)&&s.Options.Behaviour==EBehaviour::Combat)
        o<<"\n  "<<Short(bot)<<" ["<<bot->Index<<"] "<<(Alive(bot)?BotPolicy::Name(s.Now):"dead")<<" / "<<BotPolicy::Name(s.Want)<<" hp "<<(int)s.Health<<" ammo "<<s.Ammo;
    return o.str();
}

std::string LocalBots::BrainStatus()
{
    if(Brain.Seq<0)return "brain: no external orders received (tools/brain.py writes logs/bots/brain/orders.txt)";
    return "brain: "+Brain.Last;
}

bool LocalBots::IsUpdating() { return Updating; }

void LocalBots::SetIgnored(APawn* Pawn)
{
    Ignored = Pawn;
    if (Pawn) for (auto& [bot, s] : Bots) if (s.Target.Get() == Pawn) { s.Target = nullptr; s.Visible = false; }
}

void LocalBots::DestroyCharacter(AShooterCharacter* Character)
{
    if (!Character || Character->bActorIsBeingDestroyed) return;
    std::vector<AAresEquippable*> Carried;
    if (auto Inventory = Character->Inventory)
    {
        auto Items = Inventory->GetEquippables();
        for (auto Item : Items)
            // skip dropped guns, pickup owns them
            if (Item && !Item->bActorIsBeingDestroyed &&
                !(Item->Owner && Item->Owner->IsA(AAresOnGroundEquippable::StaticClass())))
                Carried.push_back(Item);
        Items.Free();
    }
    for (auto Item : Carried) Item->K2_DestroyActor();
    Character->K2_DestroyActor();
}

void LocalBots::RetireCharacter(AShooterCharacter* Character)
{
    if (!Character || Character->bActorIsBeingDestroyed) return;
    Character->SetActorHiddenInGame(true);
    Character->SetActorEnableCollision(false);
    if (auto m = Character->CharacterMovement) { m->StopMovementImmediately(); m->DisableMovement(); m->GravityScale = 0; }
    const FVector At = Character->K2_GetActorLocation();
    FHitResult Hit{};
    Character->K2_SetActorLocation(FVector{At.X, At.Y, -20000.f}, false, &Hit, true);
    if (Character->DamageHandler) Character->DamageHandler->bAlive = false;
    Ares::Log("botworld: retired %s under the map", Short(Character).c_str());
}

std::vector<AShooterCharacter*> LocalBots::Characters()
{
    auto World = UWorld::GetWorld();
    return World ? Roster(World) : std::vector<AShooterCharacter*>{};
}

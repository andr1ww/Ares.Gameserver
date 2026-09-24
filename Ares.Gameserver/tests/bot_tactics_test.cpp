#include "Headers/Modules/ShooterGame/BotTactics.h"
#include "Headers/Modules/ShooterGame/BotPolicy.h"
#include <cassert>
#include <cstdio>
#include <sstream>

static void Link(TacticalGraph& G, int A, int B)
{
    const float C = G.Dist(A, B);
    G.Links[A].push_back({B, C});
    G.Links[B].push_back({A, C});
}

// L corridor, 7 is a pocket off 1
static TacticalGraph Corridor()
{
    TacticalGraph G;
    G.Reset(8);
    G.Level = "Test";
    float P[8][2] = {{0, 0}, {200, 0}, {400, 0}, {600, 0}, {600, -200}, {800, -200}, {1000, -200}, {200, 200}};
    for (int I = 0; I < 8; ++I) G.Nodes[I] = {P[I][0], P[I][1], 0};
    Link(G, 0, 1); Link(G, 1, 2); Link(G, 2, 3); Link(G, 3, 4); Link(G, 4, 5); Link(G, 5, 6); Link(G, 1, 7);
    for (int A = 0; A <= 3; ++A) for (int B = A + 1; B <= 3; ++B) G.SetSees(A, B);
    for (int A = 3; A <= 6; ++A) for (int B = A + 1; B <= 6; ++B) G.SetSees(A, B);
    G.SetSees(2, 4);   // slice past the corner
    G.SetSees(1, 7);
    G.Index();
    return G;
}

int main()
{
    {
        auto G = Corridor();
        std::stringstream S;
        G.Write(S);
        TacticalGraph R; std::string Error;
        assert(R.Read(S, Error));
        assert(R.Count() == 8 && R.Level == "Test");
        for (int A = 0; A < 8; ++A) for (int B = 0; B < 8; ++B) assert(R.Sees(A, B) == G.Sees(A, B));
        std::stringstream Bad("TACTICAL 1\nlevel X\neye 150\nnodes 2\n0 0 0\n1 1 1\nlinks 1\n0 5 1\n");
        assert(!R.Read(Bad, Error));
    }
    auto G = Corridor();
    {
        std::vector<float> D; G.Distances({0}, D);
        assert(std::fabs(D[6] - 1200.f) < 1.f);
        auto Route = G.Route(0, 6);
        assert(Route.size() == 7 && Route.front() == 0 && Route.back() == 6);
        assert(G.Nearest(590, -190, 0) == 4);
        assert(G.Nearest(5000, 5000, 0) == -1);
        std::vector<int> Sizes;
        auto Comp = G.Components(Sizes);
        assert(Sizes.size() == 1 && Sizes[0] == 8 && Comp[7] == Comp[0]);
        std::vector<float> Pen(8, 0.f); Pen[3] = 1000;
        G.Distances({0}, D, Pen.data());
        assert(std::fabs(D[6] - 2200.f) < 1.f);
    }
    {
        Belief B; B.Observe(G, 6);
        B.Spread(G, 100.f);
        float Sum = 0; for (float V : B.P) Sum += V;
        assert(std::fabs(Sum - 1.f) < 1e-4f && B.P[5] > 0 && B.P[6] > 0 && B.P[0] == 0);
        std::vector<int> Fallback{0, 1, 2};
        B.Clear({4, 5, 6}, 0.f);
        B.Normalize(G, Fallback);
        Sum = 0; for (float V : B.P) Sum += V;
        assert(std::fabs(Sum - 1.f) < 1e-4f && B.P[0] > 0 && B.P[6] == 0);
        B.Hear(G, 800, -200, 0, 250);
        assert(B.P[5] > 0 && B.P[0] == 0);
    }
    {
        std::vector<float> FromEnemy, ToArea;
        G.Distances({6}, FromEnemy);
        G.Distances({0}, ToArea);
        std::vector<char> Approach;
        BotTactics::Approaches(FromEnemy, ToArea, 1.2f, Approach);
        assert(Approach[3] && Approach[4] && Approach[0]);
        assert(BotTactics::FirstContact(G, 1, Approach, FromEnemy) == 3);
        assert(BotTactics::FirstContact(G, 2, Approach, FromEnemy) == 4);
        auto H = BotTactics::ChooseHold(G, {0, 1, 2}, Approach, FromEnemy, {}, {});
        assert(H.Node >= 0 && H.Watch >= 0);
        std::vector<float> Threat(8, 0.f); Threat[3] = 1.f;
        std::vector<int> Hot; BotTactics::HotList(Threat, Hot);
        assert(BotTactics::Exposure(G, Threat, Hot, 1) > 0 && BotTactics::Exposure(G, Threat, Hot, 7) == 0);
        assert(BotTactics::Cover(G, 1, 500, Threat, Hot) == 7);
        std::vector<float> T2(8, 0.f); T2[5] = T2[6] = 0.5f;
        std::vector<int> Hot2; BotTactics::HotList(T2, Hot2);
        assert(BotTactics::NewlyExposed(G, 2, 3, Hot2) == 2);
        assert(BotTactics::NewlyExposed(G, 3, 4, Hot2) == 0);
        auto Route = G.Route(0, 6);
        int Stage = BotTactics::Staging(G, Route, {6});
        assert(Stage == 2);
        auto Entries = BotTactics::Entrances(G, 0, {6}, 2);
        assert(!Entries.empty() && Entries[0].back() == 6);
    }
    {
        using namespace BotPolicy;
        Situation S; S.Active = true; S.Want = Goal::Hold;
        assert(Choose(S) == Task::Move);
        S.AtGoal = true; assert(Choose(S) == Task::Hold);
        S.EnemyVisible = true; S.Ammo = 20; assert(Choose(S) == Task::Engage);
        S.Health = 20; S.SinceHurt = 0.2; S.Allies = 2; assert(Choose(S) == Task::Retreat);
        S.Allies = 0; assert(Choose(S) == Task::Engage);
        S.Ammo = 0; assert(Choose(S) == Task::Retreat);
        S.EnemyVisible = false; S.Reserve = 50; S.Exposed = false; assert(Choose(S) == Task::Reload);
        S.Ammo = 20; S.Health = 100; S.SinceSeen = 0.5; assert(Choose(S) == Task::Recheck);
        S.SinceSeen = 10; S.Want = Goal::Plant; S.AtGoal = false; assert(Choose(S) == Task::Clear);
        S.AtGoal = true; assert(Choose(S) == Task::Plant);
        S.EnemyVisible = true; S.Committed = true; assert(Choose(S) == Task::Plant);
        S.Active = false; assert(Choose(S) == Task::Wait);
        Situation D; D.Active = true; D.EnemyVisible = true; D.Ammo = 20; D.VisibleEnemies = 2;
        assert(Choose(D) == Task::Retreat);
        D.NearAllies = 1; assert(Choose(D) == Task::Engage);
        Situation P; P.Active = true; P.Ammo = 20; P.Want = Goal::Hunt; P.Peek = PeekKind::Wide;
        assert(Choose(P) == Task::Peek);
        P.SinceSeen = 0.3; assert(Choose(P) == Task::Recheck);
        P.SinceSeen = 10; P.Want = Goal::Plant; assert(Choose(P) == Task::Clear);
        PeekFacts F; assert(ChoosePeek(F) == PeekKind::None);
        F.Contact = true; F.SinceInfo = 5; assert(ChoosePeek(F) == PeekKind::Jiggle);
        F.Jiggles = 2; assert(ChoosePeek(F) == PeekKind::Shoulder);
        F.Aggression = 0.8f; assert(ChoosePeek(F) == PeekKind::Wide);
        F.HurtHere = true; assert(ChoosePeek(F) == PeekKind::None);
        F.HurtHere = false; F.LocalEnemies = 3; F.LocalAllies = 0; F.Aggression = 0.5f;
        assert(ChoosePeek(F) == PeekKind::None);
        F.EnemyReloading = true; assert(ChoosePeek(F) == PeekKind::Wide);
        PeekFacts H; H.Contact = true; H.Defensive = true; assert(ChoosePeek(H) == PeekKind::None);
        H.EnemyBusy = true; assert(ChoosePeek(H) == PeekKind::Wide);
        auto Rifle = ProfileFor("AssaultRifle_AK_C");
        assert(Rifle.Automatic && BurstShots(Rifle, 3000) == 1 && BurstShots(Rifle, 500) > 4);
        assert(!ProfileFor("RevolverPistol_C").Automatic);
        assert(FireTolerance(3000, true) < FireTolerance(500, true));
        assert(StillEnough(20) && !StillEnough(300));
        assert(SkillFor("pro").Reaction < SkillFor("easy").Reaction);
    }
    std::puts("bot tactics tests passed");
    return 0;
}

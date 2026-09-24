#pragma once
#include <Headers/Modules/ShooterGame/TacticalGraph.h>
#include <cstdint>

namespace BotTactics
{
    constexpr float Hot = 0.004f;

    inline void Threat(const std::vector<const Belief*>& Tracks, int N, std::vector<float>& Out)
    {
        Out.assign(N, 0.f);
        for (auto T : Tracks)
            if (T && (int)T->P.size() == N)
                for (int I = 0; I < N; ++I) Out[I] += T->P[I];
    }

    inline void HotList(const std::vector<float>& Threat, std::vector<int>& Out, float Eps = Hot)
    {
        Out.clear();
        for (int I = 0; I < (int)Threat.size(); ++I) if (Threat[I] > Eps) Out.push_back(I);
    }

    inline float RangeWeight(float Distance) { return Distance < 1500 ? 1.f : (Distance > 4500 ? 0.35f : 1.f - 0.65f * (Distance - 1500) / 3000); }

    inline float Exposure(const TacticalGraph& G, const std::vector<float>& Threat, const std::vector<int>& HotNodes, int I)
    {
        float E = 0;
        for (int J : HotNodes) if (G.Sees(I, J)) E += Threat[J] * RangeWeight(G.Dist(I, J));
        return E;
    }

    inline void ExposurePenalty(const TacticalGraph& G, const std::vector<float>& Threat, const std::vector<int>& HotNodes,
                                float CostPerEnemy, std::vector<float>& Out)
    {
        Out.assign(G.Count(), 0.f);
        if (HotNodes.empty()) return;
        for (int I = 0; I < G.Count(); ++I) Out[I] = CostPerEnemy * Exposure(G, Threat, HotNodes, I);
    }

    inline void Approaches(const std::vector<float>& FromEnemy, const std::vector<float>& ToArea, float Slack, std::vector<char>& Out)
    {
        const int N = (int)FromEnemy.size();
        float Direct = INFINITY;
        for (int I = 0; I < N; ++I) if (ToArea[I] == 0) Direct = std::min(Direct, FromEnemy[I]);
        Out.assign(N, 0);
        if (!std::isfinite(Direct)) return;
        for (int I = 0; I < N; ++I)
            if (std::isfinite(FromEnemy[I]) && std::isfinite(ToArea[I]) && FromEnemy[I] + ToArea[I] <= Direct * Slack + 400.f)
                Out[I] = 1;
    }

    inline float Bearing(const TacticalGraph& G, int From, int To)
    {
        const auto& A = G.Nodes[From]; const auto& B = G.Nodes[To];
        return std::atan2(B.Y - A.Y, B.X - A.X);
    }

    inline float AngleGap(float A, float B)
    {
        float D = std::fabs(A - B);
        while (D > 6.2831853f) D -= 6.2831853f;
        return D > 3.14159265f ? 6.2831853f - D : D;
    }

    inline int FirstContact(const TacticalGraph& G, int Hold, const std::vector<char>& Approach,
                            const std::vector<float>& FromEnemy, float Range = 3500.f)
    {
        int Best = -1; float Score = INFINITY;
        for (int J = 0; J < G.Count(); ++J)
        {
            if (!Approach[J] || J == Hold || !G.Sees(Hold, J) || G.Dist(Hold, J) > Range) continue;
            bool Edge = false;
            for (auto& K : G.Links[J]) if (Approach[K.To] && !G.Sees(Hold, K.To)) { Edge = true; break; }
            if (!Edge) continue;
            if (FromEnemy[J] < Score) { Score = FromEnemy[J]; Best = J; }
        }
        return Best;
    }

    struct HoldSpot { int Node = -1; int Watch = -1; float Score = -INFINITY; };

    inline HoldSpot ChooseHold(const TacticalGraph& G, const std::vector<int>& Candidates, const std::vector<char>& Approach,
                               const std::vector<float>& FromEnemy, const std::vector<int>& TakenWatch,
                               const std::vector<int>& TakenNodes, uint32_t Seed = 0)
    {
        HoldSpot Best;
        for (int H : Candidates)
        {
            const int Watch = FirstContact(G, H, Approach, FromEnemy);
            if (Watch < 0) continue;
            int Seen = 0; float Spread = 0; const float Center = Bearing(G, H, Watch);
            for (int J = 0; J < G.Count(); ++J)
            {
                if (!Approach[J] || !G.Sees(H, J) || G.Dist(H, J) > 3500) continue;
                ++Seen;
                Spread = std::max(Spread, AngleGap(Center, Bearing(G, H, J)));
            }
            bool Cover = false;
            for (auto& K : G.Links[H]) if (!G.Sees(K.To, Watch)) { Cover = true; break; }
            const float Distance = G.Dist(H, Watch);
            float Score = Approach[H] ? -0.3f : 0.f;
            Score -= Spread * 2.2f;
            Score -= Seen * 0.015f;
            Score += Cover ? 1.0f : 0.f;
            Score += Distance > 700 && Distance < 2600 ? 0.6f : 0.f;
            for (int W : TakenWatch) if (W >= 0 && (W == Watch || G.Dist(W, Watch) < 450)) Score -= 1.5f;
            for (int T : TakenNodes) if (T >= 0 && G.Dist(T, H) < 350) Score -= 1.0f;
            // bit of jitter so holds change per round
            Score += ((H * 2654435761u + Seed) % 1000) / 1000.f * 0.35f;
            if (Score > Best.Score) Best = {H, Watch, Score};
        }
        return Best;
    }

    inline int Cover(const TacticalGraph& G, int From, float Budget, const std::vector<float>& Threat,
                     const std::vector<int>& HotNodes, int Avoid = -1)
    {
        if (From < 0) return -1;
        std::vector<float> D;
        G.Distances({From}, D);
        int Best = -1; float Score = INFINITY;
        for (int I = 0; I < G.Count(); ++I)
        {
            if (!(D[I] <= Budget)) continue;
            if (Avoid >= 0 && G.Sees(I, Avoid)) continue;
            const float S = Exposure(G, Threat, HotNodes, I) * 4000.f + D[I];
            if (S < Score) { Score = S; Best = I; }
        }
        return Best;
    }

    inline std::vector<std::vector<int>> Entrances(const TacticalGraph& G, int From, const std::vector<int>& Area, int Count)
    {
        std::vector<std::vector<int>> Routes;
        std::vector<float> Penalty(G.Count(), 0.f);
        std::vector<char> InArea(G.Count(), 0);
        for (int A : Area) InArea[A] = 1;
        for (int R = 0; R < Count; ++R)
        {
            std::vector<float> ToArea;
            G.Distances(Area, ToArea);
            std::vector<float> D;
            G.Distances({From}, D, Penalty.data());
            int Goal = -1; float Best = INFINITY;
            for (int A : Area) if (D[A] < Best) { Best = D[A]; Goal = A; }
            if (Goal < 0) break;
            auto Route = G.Route(From, Goal, Penalty.data());
            if (Route.empty()) break;
            bool Duplicate = false;
            for (auto& Earlier : Routes)
            {
                int Shared = 0;
                for (int N : Route) for (int M : Earlier) if (N == M) { ++Shared; break; }
                if (Shared > (int)Route.size() * 0.7f) { Duplicate = true; break; }
            }
            // push next route to a different entrance
            for (size_t I = Route.size() > 12 ? Route.size() - 12 : 0; I < Route.size(); ++I)
                if (!InArea[Route[I]]) { Penalty[Route[I]] += 2500.f; for (auto& K : G.Links[Route[I]]) Penalty[K.To] += 1200.f; }
            if (!Duplicate) Routes.push_back(Route);
        }
        return Routes;
    }

    inline int Staging(const TacticalGraph& G, const std::vector<int>& Route, const std::vector<int>& Area)
    {
        int Last = -1;
        for (int N : Route)
        {
            bool Seen = false;
            for (int A : Area) if (G.Sees(N, A) && G.Dist(N, A) < 3500) { Seen = true; break; }
            if (Seen) break;
            Last = N;
        }
        return Last;
    }

    inline int NewlyExposed(const TacticalGraph& G, int I, int J, const std::vector<int>& HotNodes, std::vector<int>* Out = nullptr)
    {
        int Count = 0;
        if (Out) Out->clear();
        for (int H : HotNodes)
            if (G.Sees(J, H) && !G.Sees(I, H) && G.Dist(J, H) < 3000) { ++Count; if (Out) Out->push_back(H); }
        return Count;
    }
}

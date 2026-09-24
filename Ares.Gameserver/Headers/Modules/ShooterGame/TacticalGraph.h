#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <istream>
#include <ostream>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

struct TacticalGraph
{
    struct Node { float X = 0, Y = 0, Z = 0; };
    struct Link { int To = 0; float Cost = 0; bool Jump = false; };

    std::string Level;
    float Eye = 150.f;
    std::vector<Node> Nodes;
    std::vector<std::vector<Link>> Links;
    std::vector<uint64_t> Vis;                    // N*N bits
    int Words = 0;

    int Count() const { return (int)Nodes.size(); }

    void Reset(int N)
    {
        Nodes.assign(N, {});
        Links.assign(N, {});
        Words = (N + 63) / 64;
        Vis.assign((size_t)Words * N, 0);
        Cells.clear();
    }

    bool Sees(int A, int B) const
    {
        if (A == B) return true;
        return (Vis[(size_t)A * Words + (B >> 6)] >> (B & 63)) & 1;
    }

    void SetSees(int A, int B)
    {
        Vis[(size_t)A * Words + (B >> 6)] |= 1ull << (B & 63);
        Vis[(size_t)B * Words + (A >> 6)] |= 1ull << (A & 63);
    }

    static float Dist(const Node& A, const Node& B)
    {
        const float DX = A.X - B.X, DY = A.Y - B.Y, DZ = A.Z - B.Z;
        return std::sqrt(DX * DX + DY * DY + DZ * DZ);
    }
    float Dist(int A, int B) const { return Dist(Nodes[A], Nodes[B]); }

    // jumps section is optional so old readers still work
    void Write(std::ostream& Out) const
    {
        Out << "TACTICAL 1\nlevel " << Level << "\neye " << Eye << "\nnodes " << Nodes.size() << '\n';
        for (auto& N : Nodes) Out << N.X << ' ' << N.Y << ' ' << N.Z << '\n';
        size_t L = 0, J = 0;
        for (auto& List : Links) for (auto& K : List) ++(K.Jump ? J : L);
        Out << "links " << L << '\n';
        for (int A = 0; A < Count(); ++A)
            for (auto& K : Links[A]) if (!K.Jump) Out << A << ' ' << K.To << ' ' << K.Cost << '\n';
        Out << "vis\n";
        for (int A = 0; A < Count(); ++A)
        {
            std::vector<int> Seen;
            for (int B = A + 1; B < Count(); ++B) if (Sees(A, B)) Seen.push_back(B);
            Out << A << ' ' << Seen.size();
            for (int B : Seen) Out << ' ' << B;
            Out << '\n';
        }
        if (J)
        {
            Out << "jumps " << J << '\n';
            for (int A = 0; A < Count(); ++A)
                for (auto& K : Links[A]) if (K.Jump) Out << A << ' ' << K.To << ' ' << K.Cost << '\n';
        }
        Out << "end\n";
    }

    bool Read(std::istream& In, std::string& Error)
    {
        auto Fail = [&](const char* Why) { Error = Why; Nodes.clear(); return false; };
        std::string Word; int Version = 0; size_t N = 0, L = 0;
        if (!(In >> Word >> Version) || Word != "TACTICAL" || Version != 1) return Fail("bad header");
        if (!(In >> Word >> Level) || Word != "level") return Fail("missing level");
        if (!(In >> Word >> Eye) || Word != "eye") return Fail("missing eye");
        if (!(In >> Word >> N) || Word != "nodes" || N == 0 || N > 20000) return Fail("bad node count");
        Reset((int)N);
        for (auto& Node : Nodes)
            if (!(In >> Node.X >> Node.Y >> Node.Z) || !std::isfinite(Node.X) || !std::isfinite(Node.Y) || !std::isfinite(Node.Z))
                return Fail("bad node");
        if (!(In >> Word >> L) || Word != "links") return Fail("missing links");
        for (size_t I = 0; I < L; ++I)
        {
            int A, B; float C;
            if (!(In >> A >> B >> C) || A < 0 || B < 0 || A >= (int)N || B >= (int)N || !(C > 0)) return Fail("bad link");
            Links[A].push_back({B, C});
        }
        if (!(In >> Word) || Word != "vis") return Fail("missing vis");
        for (size_t I = 0; I < N; ++I)
        {
            int A; size_t K;
            if (!(In >> A >> K) || A != (int)I || K > N) return Fail("bad vis row");
            for (size_t J = 0; J < K; ++J)
            {
                int B;
                if (!(In >> B) || B <= A || B >= (int)N) return Fail("bad vis entry");
                SetSees(A, B);
            }
        }
        if (!(In >> Word)) return Fail("missing end");
        if (Word == "jumps")
        {
            size_t J = 0;
            if (!(In >> J) || J > 100000) return Fail("bad jump count");
            for (size_t I = 0; I < J; ++I)
            {
                int A, B; float C;
                if (!(In >> A >> B >> C) || A < 0 || B < 0 || A >= (int)N || B >= (int)N || !(C > 0)) return Fail("bad jump");
                Links[A].push_back({B, C, true});
            }
            if (!(In >> Word)) return Fail("missing end");
        }
        if (Word != "end") return Fail("missing end");
        Index();
        return true;
    }

    // skip unlinked nodes or bots snap onto dead ledges
    void Index(bool AllNodes = false)
    {
        Cells.clear();
        std::vector<char> Linked(Count(), AllNodes ? 1 : 0);
        for (int A = 0; A < Count(); ++A)
            for (auto& K : Links[A]) { Linked[A] = 1; Linked[K.To] = 1; }
        for (int I = 0; I < Count(); ++I) if (Linked[I]) Cells[Key(Nodes[I].X, Nodes[I].Y)].push_back(I);
    }

    std::vector<int> Components(std::vector<int>& Sizes) const
    {
        std::vector<std::vector<int>> Undirected(Count());
        for (int A = 0; A < Count(); ++A)
            for (auto& K : Links[A]) { Undirected[A].push_back(K.To); Undirected[K.To].push_back(A); }
        std::vector<int> Comp(Count(), -1);
        Sizes.clear();
        for (int S = 0; S < Count(); ++S)
        {
            if (Comp[S] >= 0) continue;
            const int Id = (int)Sizes.size();
            Sizes.push_back(0);
            std::vector<int> Stack{S};
            Comp[S] = Id;
            while (!Stack.empty())
            {
                const int A = Stack.back(); Stack.pop_back(); ++Sizes[Id];
                for (int B : Undirected[A]) if (Comp[B] < 0) { Comp[B] = Id; Stack.push_back(B); }
            }
        }
        return Comp;
    }

    int Nearest(float X, float Y, float Z, float MaxDist = 450.f) const
    {
        int Best = -1; float Score = MaxDist * MaxDist;
        const int CX = Cell(X), CY = Cell(Y);
        for (int DY = -1; DY <= 1; ++DY) for (int DX = -1; DX <= 1; ++DX)
        {
            auto Found = Cells.find(Pack(CX + DX, CY + DY));
            if (Found == Cells.end()) continue;
            for (int I : Found->second)
            {
                const auto& N = Nodes[I];
                const float EX = N.X - X, EY = N.Y - Y, EZ = (N.Z - Z) * 3.f;
                const float S = EX * EX + EY * EY + EZ * EZ;
                if (S < Score) { Score = S; Best = I; }
            }
        }
        return Best;
    }

    void Within(float X, float Y, float Z, float Radius, std::vector<int>& Out) const
    {
        Out.clear();
        const int Span = (int)std::ceil(Radius / CellSize);
        const int CX = Cell(X), CY = Cell(Y);
        for (int DY = -Span; DY <= Span; ++DY) for (int DX = -Span; DX <= Span; ++DX)
        {
            auto Found = Cells.find(Pack(CX + DX, CY + DY));
            if (Found == Cells.end()) continue;
            for (int I : Found->second)
            {
                const auto& N = Nodes[I];
                const float EX = N.X - X, EY = N.Y - Y, EZ = N.Z - Z;
                if (EX * EX + EY * EY + EZ * EZ <= Radius * Radius) Out.push_back(I);
            }
        }
    }

    void Distances(const std::vector<int>& Sources, std::vector<float>& Out, const float* Penalty = nullptr) const
    {
        Out.assign(Count(), INFINITY);
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> Open;
        for (int S : Sources) if (S >= 0 && S < Count()) { Out[S] = 0; Open.push({0.f, S}); }
        while (!Open.empty())
        {
            auto [D, A] = Open.top(); Open.pop();
            if (D > Out[A]) continue;
            for (auto& K : Links[A])
            {
                const float Next = D + K.Cost + (Penalty ? Penalty[K.To] : 0.f);
                if (Next < Out[K.To]) { Out[K.To] = Next; Open.push({Next, K.To}); }
            }
        }
    }

    std::vector<int> Route(int From, int To, const float* Penalty = nullptr) const
    {
        std::vector<int> Path;
        if (From < 0 || To < 0 || From >= Count() || To >= Count()) return Path;
        std::vector<float> Best(Count(), INFINITY);
        std::vector<int> Prev(Count(), -1);
        using Item = std::pair<float, int>;
        std::priority_queue<Item, std::vector<Item>, std::greater<Item>> Open;
        Best[From] = 0; Open.push({0.f, From});
        while (!Open.empty())
        {
            auto [D, A] = Open.top(); Open.pop();
            if (A == To) break;
            if (D > Best[A]) continue;
            for (auto& K : Links[A])
            {
                const float Next = D + K.Cost + (Penalty ? Penalty[K.To] : 0.f);
                if (Next < Best[K.To]) { Best[K.To] = Next; Prev[K.To] = A; Open.push({Next, K.To}); }
            }
        }
        if (!std::isfinite(Best[To])) return Path;
        for (int At = To; At != -1; At = Prev[At]) Path.push_back(At);
        std::reverse(Path.begin(), Path.end());
        return Path;
    }

private:
    static constexpr float CellSize = 400.f;
    std::unordered_map<long long, std::vector<int>> Cells;
    static int Cell(float V) { return (int)std::floor(V / CellSize); }
    static long long Pack(int X, int Y) { return ((long long)X << 32) ^ (unsigned)Y; }
    static long long Key(float X, float Y) { return Pack(Cell(X), Cell(Y)); }
};

struct Belief
{
    std::vector<float> P;

    void Uniform(const TacticalGraph& G, const std::vector<int>& Over)
    {
        P.assign(G.Count(), 0.f);
        if (Over.empty()) { std::fill(P.begin(), P.end(), 1.f / std::max(1, G.Count())); return; }
        for (int I : Over) P[I] += 1.f / Over.size();
    }

    void Observe(const TacticalGraph& G, int Node)
    {
        P.assign(G.Count(), 0.f);
        if (Node >= 0 && Node < (int)P.size()) P[Node] = 1.f;
    }

    void Hear(const TacticalGraph& G, float X, float Y, float Z, float Radius)
    {
        std::vector<int> Near;
        G.Within(X, Y, Z, Radius, Near);
        if (Near.empty()) { int N = G.Nearest(X, Y, Z, Radius * 2); if (N >= 0) Observe(G, N); return; }
        P.assign(G.Count(), 0.f);
        for (int I : Near) P[I] = 1.f / Near.size();
    }

    void Spread(const TacticalGraph& G, float Travel)
    {
        if ((int)P.size() != G.Count()) { Uniform(G, {}); return; }
        std::vector<float> Next(P.size(), 0.f);
        for (int A = 0; A < G.Count(); ++A)
        {
            if (P[A] <= 0) continue;
            const auto& Out = G.Links[A];
            if (Out.empty()) { Next[A] += P[A]; continue; }
            float Moving = 0;
            for (auto& K : Out)
            {
                const float Share = P[A] * std::min(1.f, Travel / std::max(1.f, K.Cost)) / Out.size();
                Next[K.To] += Share; Moving += Share;
            }
            Next[A] += P[A] - Moving;
        }
        P.swap(Next);
    }

    void Clear(const std::vector<int>& Seen, float Factor)
    {
        for (int I : Seen) if (I >= 0 && I < (int)P.size()) P[I] *= Factor;
    }

    float Normalize(const TacticalGraph& G, const std::vector<int>& Fallback)
    {
        float Sum = 0;
        for (float V : P) Sum += V;
        if (Sum < 1e-6f) { Uniform(G, Fallback); return 1.f; }
        for (float& V : P) V /= Sum;
        return 1.f - Sum;
    }

    // peak is noise once spread thin, use area mass
    int Hotspot(const TacticalGraph& G, float Radius, std::vector<float>& Mass) const
    {
        Mass.assign(G.Count(), 0.f);
        if ((int)P.size() != G.Count()) return -1;
        float Max = 0;
        for (float V : P) Max = std::max(Max, V);
        std::vector<int> Near;
        for (int J = 0; J < G.Count(); ++J)
        {
            if (P[J] <= 0 || P[J] < Max * 0.01f) continue;
            const auto& N = G.Nodes[J];
            G.Within(N.X, N.Y, N.Z, Radius, Near);
            for (int I : Near) Mass[I] += P[J];
        }
        return (int)(std::max_element(Mass.begin(), Mass.end()) - Mass.begin());
    }

    int Peak() const
    {
        return P.empty() ? -1 : (int)(std::max_element(P.begin(), P.end()) - P.begin());
    }
};

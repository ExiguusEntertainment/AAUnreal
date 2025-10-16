#include "SectorMeshBuilderActor.h"
#include "DoomWadLoader.h"
#include "Engine/World.h"
#include "ProceduralMeshComponent.h"
#include "Components/SceneComponent.h"
#include "Materials/MaterialInterface.h"
#include "KismetProceduralMeshLibrary.h"

ASectorMeshBuilderActor::ASectorMeshBuilderActor()
{
    PrimaryActorTick.bCanEverTick = false;
    SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    SetRootComponent(SceneRoot);
}

void ASectorMeshBuilderActor::OnConstruction(const FTransform& Transform)
{
    Super::OnConstruction(Transform);
}

void ASectorMeshBuilderActor::ClearChildren()
{
    if (!bClearPrevious) return;
    TArray<UActorComponent*> Comps;
    GetComponents(Comps);
    for (UActorComponent* C : Comps)
    {
        if (C && C->IsA(UProceduralMeshComponent::StaticClass()))
        {
            C->DestroyComponent();
        }
    }
}

static int32 FindNextEdgeStart(int32 CurrentEnd, const TMultiMap<int32, int32>& FromTo)
{
    TArray<int32> ToCandidates; FromTo.MultiFind(CurrentEnd, ToCandidates);
    if (ToCandidates.Num() > 0) return ToCandidates[0];
    return INDEX_NONE;
}

void ASectorMeshBuilderActor::BuildSectorLoops(int32 SectorIndex,
                                               const TArray<FIntPoint>& Lines,
                                               const TArray<FIntPoint>& LineSectors,
                                               TArray<TArray<int32>>& OutLoops)
{
    OutLoops.Reset();

    TMultiMap<int32,int32> FromTo;
    TSet<uint64> EdgeSet;

    auto MakeKey = [](int32 A, int32 B)->uint64 { return (uint64(uint32(A))<<32) | uint32(B); };

    for (int32 i=0;i<Lines.Num();++i)
    {
        const FIntPoint& L = Lines[i];
        const FIntPoint& LS = LineSectors[i];
        if (LS.X == SectorIndex)
        {
            if (L.X != L.Y)
            {
                FromTo.Add(L.X, L.Y);
                EdgeSet.Add(MakeKey(L.X, L.Y));
            }
        }
        if (LS.Y == SectorIndex)
        {
            if (L.X != L.Y)
            {
                FromTo.Add(L.Y, L.X);
                EdgeSet.Add(MakeKey(L.Y, L.X));
            }
        }
    }

    TSet<uint64> Used;
    for (auto It = EdgeSet.CreateConstIterator(); It; ++It)
    {
        const uint64 Key = *It;
        if (Used.Contains(Key)) continue;

        int32 StartA = int32(Key >> 32);
        int32 StartB = int32(Key & 0xffffffff);
        TArray<int32> Loop;
        Loop.Add(StartA);
        Loop.Add(StartB);
        Used.Add(Key);

        int32 Curr = StartB;
        int32 Guard = 0;
        while (Guard++ < 100000)
        {
            int32 Next = INDEX_NONE;
            TArray<int32> Candidates; FromTo.MultiFind(Curr, Candidates);
            for (int32 C : Candidates)
            {
                uint64 K2 = MakeKey(Curr, C);
                if (!Used.Contains(K2)) { Next = C; Used.Add(K2); break; }
            }
            if (Next == INDEX_NONE) break;
            if (Next == Loop[0]) { /* close without duplicating first */ break; }
            Loop.Add(Next);
            Curr = Next;
        }
        if (Loop.Num() >= 3)
        {
            OutLoops.Add(Loop);
        }
    }
}

static void AppendMeshToPMC(UProceduralMeshComponent* PMC, const TArray<FVector>& Verts, const TArray<int32>& Indices,
                            const TArray<FVector>& Normals, const TArray<FVector2D>& UV0, const TArray<FColor>& Colors, const TArray<FProcMeshTangent>& Tangents)
{
    int32 Section = PMC->GetNumSections();
    PMC->CreateMeshSection(Section, Verts, Indices, Normals, UV0, Colors, Tangents, true);
}

void ASectorMeshBuilderActor::BuildPolygonMesh(const TArray<FVector2D>& Poly, float Z, bool bCeiling, float UnitsToUU, float UVScale,
                                               TArray<FVector>& OutVerts, TArray<int32>& OutIndices, TArray<FVector>& OutNormals, TArray<FVector2D>& OutUV0, TArray<FColor>& OutColors, TArray<FProcMeshTangent>& OutTangents)
{
    OutVerts.Reset(); OutIndices.Reset(); OutNormals.Reset(); OutUV0.Reset(); OutColors.Reset(); OutTangents.Reset();
    if (Poly.Num() < 3) return;

    // Triangulate with orientation handling inside the ear clipper
    TArray<int32> LocalTri;
    if (!TriangulateEarClip(Poly, LocalTri)) return;

    for (const FVector2D& Pt : Poly)
    {
        OutVerts.Add(FVector(Pt.X * UnitsToUU, Pt.Y * UnitsToUU, Z));
        OutUV0.Add(FVector2D(Pt.X * UVScale, Pt.Y * UVScale));
        OutColors.Add(FColor::White);
    }

    OutIndices = LocalTri;

    // Flip floor winding so it faces toward +Z viewer by default; keep ceiling as produced so it faces -Z viewer
    if (!bCeiling)
    {
        for (int32 i=0;i+2<OutIndices.Num(); i+=3)
        {
            Swap(OutIndices[i+1], OutIndices[i+2]);
        }
    }

    // Compute normals/tangents from geometry and UVs
    UKismetProceduralMeshLibrary::CalculateTangentsForMesh(OutVerts, OutIndices, OutUV0, OutNormals, OutTangents);
}

static void CalculateWallQuadUVs(const FVector& Q0, const FVector& Q1, const FVector& Q2, const FVector& Q3,
                                  float Length, float WallUScale, float WallVScale,
                                  TArray<FVector2D>& WallUV, TArray<FColor>& WallColors)
{
    // UVs per-quad based on edge length and vertical size
    const float ULen = Length * WallUScale;
    const float VLen = (Q1 - Q0).Size() * WallVScale;
    WallUV.Add(FVector2D(0.f, 0.f));
    WallUV.Add(FVector2D(0.f, VLen));
    WallUV.Add(FVector2D(ULen, VLen));
    WallUV.Add(FVector2D(ULen, 0.f));
    WallColors.Add(FColor::White); WallColors.Add(FColor::White); WallColors.Add(FColor::White); WallColors.Add(FColor::White);
}

bool ASectorMeshBuilderActor::TriangulateEarClip(const TArray<FVector2D>& InPoly, TArray<int32>& OutIndices)
{
    OutIndices.Reset();
    const int32 N = InPoly.Num();
    if (N < 3) return false;

    auto Area2 = [&](int32 a, int32 b, int32 c) -> float
    {
        const FVector2D &A = InPoly[a], &B=InPoly[b], &C=InPoly[c];
        return (B.X - A.X)*(C.Y - A.Y) - (B.Y - A.Y)*(C.X - A.X);
    };

    // Orientation
    float Sum = 0; for (int32 i=0;i<N;i++){ int32 j=(i+1)%N; Sum += InPoly[i].X*InPoly[j].Y - InPoly[j].X*InPoly[i].Y; }
    bool bCCW = Sum > 0;

    TArray<int32> V; V.Reserve(N);
    if (bCCW) { for (int32 i=0;i<N;++i) V.Add(i);} else { for (int32 i=0;i<N;++i) V.Add(N-1-i);}    

    auto PointInTri = [&](const FVector2D& A, const FVector2D& B, const FVector2D& C, const FVector2D& P)->bool
    {
        float a = ((B.Y - C.Y)*(P.X - C.X) + (C.X - B.X)*(P.Y - C.Y)) /
                  ((B.Y - C.Y)*(A.X - C.X) + (C.X - B.X)*(A.Y - C.Y));
        float b = ((C.Y - A.Y)*(P.X - C.X) + (A.X - C.X)*(P.Y - C.Y)) /
                  ((B.Y - C.Y)*(A.X - C.X) + (C.X - B.X)*(A.Y - C.Y));
        float c = 1.0f - a - b;
        return a >= 0 && b >= 0 && c >= 0;
    };

    int32 Guard=0;
    while (V.Num() > 2 && Guard++ < 100000)
    {
        bool Clipped=false;
        const int32 n=V.Num();
        for (int32 i=0;i<n;i++)
        {
            int32 i0 = (i + n - 1) % n;
            int32 i1 = i;
            int32 i2 = (i + 1) % n;
            if (Area2(V[i0], V[i1], V[i2]) <= 0) continue; // reflex or colinear
            bool HasInside=false;
            for (int32 j=0;j<n;j++) if (j!=i0 && j!=i1 && j!=i2){ if (PointInTri(InPoly[V[i0]],InPoly[V[i1]],InPoly[V[i2]], InPoly[V[j]])){ HasInside=true; break; } }
            if (HasInside) continue;
            OutIndices.Add(V[i0]); OutIndices.Add(V[i1]); OutIndices.Add(V[i2]);
            V.RemoveAt(i1); Clipped=true; break;
        }
        if (!Clipped) break;
    }

    return OutIndices.Num() >= 3;
}

void ASectorMeshBuilderActor::BuildSectorsFromLastWad()
{
    UWorld* World = GetWorld(); if (!World) return;

    // Fetch topology
    TArray<FVector2D> Verts2D; TArray<FIntPoint> Lines; TArray<FIntPoint> LineSectors; TArray<FIntPoint> SectorHeights;
    if (!UDoomWadLoader::GetLastWadWallTopology(Verts2D, Lines, LineSectors, SectorHeights))
    {
        UE_LOG(LogTemp, Warning, TEXT("No WAD topology loaded. Use AA.LoadWad first."));
        return;
    }

    ClearChildren();

    // For each sector, build loops and generate floor/ceiling; walls TBD
    for (int32 SectorIdx = 0; SectorIdx < SectorHeights.Num(); ++SectorIdx)
    {
        if (SectorFilter >= 0 && SectorIdx != SectorFilter) continue;

        const int32 FloorZ = SectorHeights[SectorIdx].X;
        const int32 CeilZ = SectorHeights[SectorIdx].Y;
        if (CeilZ <= FloorZ) continue;

        UProceduralMeshComponent* PMC_Walls = nullptr;
        UProceduralMeshComponent* PMC_Floor = nullptr;
        UProceduralMeshComponent* PMC_Ceil = nullptr;
        TArray<FVector> WallVerts; TArray<int32> WallIndices; TArray<FVector> WallNormals; TArray<FVector2D> WallUV; TArray<FColor> WallColors; TArray<FProcMeshTangent> WallTangents;

        if (bBuildFloorsAndCeilings)
        {
            PMC_Floor = NewObject<UProceduralMeshComponent>(this);
            PMC_Floor->RegisterComponent(); PMC_Floor->AttachToComponent(SceneRoot, FAttachmentTransformRules::KeepRelativeTransform);
            if (FloorMaterial) PMC_Floor->SetMaterial(0, FloorMaterial);

            PMC_Ceil = NewObject<UProceduralMeshComponent>(this);
            PMC_Ceil->RegisterComponent(); PMC_Ceil->AttachToComponent(SceneRoot, FAttachmentTransformRules::KeepRelativeTransform);
            if (CeilingMaterial) PMC_Ceil->SetMaterial(0, CeilingMaterial);
        }

        // Select polygon sources: subsector polys (preferred) or fallback linedef loops
        struct FPolyList { TArray<int32> Offsets; TArray<int32> Counts; TArray<FVector2D> Points; };
        FPolyList PolyList;
        bool bHaveSubsectorPolys = false;
        if (bUseSubsectorPolysForFloors)
        {
            bHaveSubsectorPolys = UDoomWadLoader::GetLastWadSectorSubsectorPolys(SectorIdx, PolyList.Offsets, PolyList.Counts, PolyList.Points);
        }

        TArray<TArray<int32>> Loops; // fallback loops
        if (!bHaveSubsectorPolys)
        {
            BuildSectorLoops(SectorIdx, Lines, LineSectors, Loops);
        }

        auto BuildFloorCeilForPoly = [&](const TArray<FVector2D>& Poly)
        {
            if (!bBuildFloorsAndCeilings) return;
            // Floor
            {
                TArray<FVector> V; TArray<int32> I; TArray<FVector> N; TArray<FVector2D> UV; TArray<FColor> C; TArray<FProcMeshTangent> T;
                BuildPolygonMesh(Poly, (float)FloorZ * UnitsToUU, false, UnitsToUU, FloorCeilUVScale, V, I, N, UV, C, T);
                if (V.Num() && I.Num()) AppendMeshToPMC(PMC_Floor, V, I, N, UV, C, T);
            }
            // Ceiling
            {
                TArray<FVector> V; TArray<int32> I; TArray<FVector> N; TArray<FVector2D> UV; TArray<FColor> C; TArray<FProcMeshTangent> T;
                BuildPolygonMesh(Poly, (float)CeilZ * UnitsToUU, true, UnitsToUU, FloorCeilUVScale, V, I, N, UV, C, T);
                if (V.Num() && I.Num()) AppendMeshToPMC(PMC_Ceil, V, I, N, UV, C, T);
            }
        };

        if (bHaveSubsectorPolys)
        {
            for (int32 i=0;i<PolyList.Offsets.Num(); ++i)
            {
                const int32 Off = PolyList.Offsets[i];
                const int32 Cnt = PolyList.Counts[i];
                if (Cnt < 3 || Off < 0 || Off + Cnt > PolyList.Points.Num()) continue;
                TArray<FVector2D> Poly; Poly.Reserve(Cnt);
                for (int32 k=0;k<Cnt;++k) Poly.Add(PolyList.Points[Off + k]);
                BuildFloorCeilForPoly(Poly);
            }
        }
        else
        {
            for (const TArray<int32>& Loop : Loops)
            {
                if (Loop.Num() < 3) continue;
                const int32 LastIndex = (Loop.Last() == Loop[0]) ? Loop.Num()-1 : Loop.Num();
                TArray<FVector2D> Poly; Poly.Reserve(LastIndex);
                for (int32 i=0;i<LastIndex;++i) { if (Verts2D.IsValidIndex(Loop[i])) Poly.Add(Verts2D[Loop[i]]); }
                if (Poly.Num() < 3) continue;
                BuildFloorCeilForPoly(Poly);
            }
        }

        // Walls: derive working loops
        TArray<TArray<int32>> WallLoops;
        if (!bHaveSubsectorPolys)
        {
            WallLoops = Loops;
        }
        else
        {
            // Build pseudo-loops from subsector polys by mapping to nearest vertex indices
            for (int32 i=0;i<PolyList.Offsets.Num(); ++i)
            {
                const int32 Off = PolyList.Offsets[i];
                const int32 Cnt = PolyList.Counts[i];
                if (Cnt < 3 || Off < 0 || Off + Cnt > PolyList.Points.Num()) continue;
                TArray<int32> Loop;
                for (int32 k=0;k<Cnt;++k)
                {
                    const FVector2D Pt = PolyList.Points[Off+k];
                    int32 BestIdx=-1; float BestD2=FLT_MAX;
                    for (int32 vi=0; vi<Verts2D.Num(); ++vi)
                    {
                        const float d2 = FVector2D::DistSquared(Pt, Verts2D[vi]);
                        if (d2 < BestD2) { BestD2 = d2; BestIdx = vi; }
                    }
                    if (BestIdx>=0) Loop.Add(BestIdx);
                }
                if (Loop.Num()>=3) WallLoops.Add(Loop);
            }
        }

        if (bBuildWalls && WallLoops.Num() > 0)
        {
            PMC_Walls = NewObject<UProceduralMeshComponent>(this);
            PMC_Walls->RegisterComponent();
            PMC_Walls->AttachToComponent(SceneRoot, FAttachmentTransformRules::KeepRelativeTransform);
            if (WallMaterial) PMC_Walls->SetMaterial(0, WallMaterial);

            for (const TArray<int32>& Loop : WallLoops)
            {
                if (Loop.Num() < 3) continue;
                const int32 LastIndex = Loop.Num();
                for (int32 i=0;i<LastIndex;++i)
                {
                    const int32 AIdx = Loop[i];
                    const int32 BIdx = Loop[(i+1) % LastIndex];
                    if (!Verts2D.IsValidIndex(AIdx) || !Verts2D.IsValidIndex(BIdx)) continue;

                    int32 LineIndex = -1; bool bReversed = false;
                    for (int32 li=0; li<Lines.Num(); ++li)
                    {
                        const FIntPoint& L = Lines[li];
                        if (L.X == AIdx && L.Y == BIdx) { LineIndex = li; bReversed = false; break; }
                        if (L.X == BIdx && L.Y == AIdx) { LineIndex = li; bReversed = true; break; }
                    }
                    if (LineIndex == -1) continue;

                    const FIntPoint& LS = LineSectors[LineIndex];
                    int32 Neighbor = -1;
                    if (!bReversed)
                    {
                        if (LS.X == SectorIdx) Neighbor = LS.Y; else if (LS.Y == SectorIdx) Neighbor = LS.X;
                    }
                    else
                    {
                        if (LS.Y == SectorIdx) Neighbor = LS.X; else if (LS.X == SectorIdx) Neighbor = LS.Y;
                    }

                    const FVector2D A2 = Verts2D[AIdx];
                    const FVector2D B2 = Verts2D[BIdx];
                    const FVector ABase(A2.X * UnitsToUU, A2.Y * UnitsToUU, (float)FloorZ * UnitsToUU);
                    const FVector BBase(B2.X * UnitsToUU, B2.Y * UnitsToUU, (float)FloorZ * UnitsToUU);
                    const FVector ATop (A2.X * UnitsToUU, A2.Y * UnitsToUU, (float)CeilZ * UnitsToUU);
                    const FVector BTop (B2.X * UnitsToUU, B2.Y * UnitsToUU, (float)CeilZ * UnitsToUU);

                    const FVector Edge = (BBase - ABase);
                    const float Length = FVector(Edge.X, Edge.Y, 0.f).Size();
                    if (Length <= KINDA_SMALL_NUMBER) continue;

                    const FVector2D Dir2 = (B2 - A2).GetSafeNormal();
                    const FVector InNormal = FVector(-Dir2.Y, Dir2.X, 0.f);

                    auto AddQuad = [&](const FVector& Q0, const FVector& Q1, const FVector& Q2, const FVector& Q3)
                    {
                        const int32 Base = WallVerts.Num();
                        WallVerts.Add(Q0); WallVerts.Add(Q1); WallVerts.Add(Q2); WallVerts.Add(Q3);
                        CalculateWallQuadUVs(Q0, Q1, Q2, Q3, Length, WallUScale, WallVScale, WallUV, WallColors);
                        WallIndices.Add(Base + 0); WallIndices.Add(Base + 1); WallIndices.Add(Base + 2);
                        WallIndices.Add(Base + 0); WallIndices.Add(Base + 2); WallIndices.Add(Base + 3);
                    };

                    if (Neighbor == -1)
                    {
                        AddQuad(ABase, ATop, BTop, BBase);
                    }
                    else
                    {
                        const int32 NFloor = SectorHeights.IsValidIndex(Neighbor) ? SectorHeights[Neighbor].X : FloorZ;
                        const int32 NCeil  = SectorHeights.IsValidIndex(Neighbor) ? SectorHeights[Neighbor].Y : CeilZ;

                        if (NFloor > FloorZ)
                        {
                            const float Z0 = (float)FloorZ * UnitsToUU;
                            const float Z1 = (float)FMath::Min(CeilZ, NFloor) * UnitsToUU;
                            if (Z1 > Z0 + KINDA_SMALL_NUMBER)
                            {
                                AddQuad(
                                    FVector(ABase.X, ABase.Y, Z0),
                                    FVector(ABase.X, ABase.Y, Z1),
                                    FVector(BBase.X, BBase.Y, Z1),
                                    FVector(BBase.X, BBase.Y, Z0));
                            }
                        }

                        if (NCeil < CeilZ)
                        {
                            const float Z0 = (float)FMath::Max(FloorZ, NCeil) * UnitsToUU;
                            const float Z1 = (float)CeilZ * UnitsToUU;
                            if (Z1 > Z0 + KINDA_SMALL_NUMBER)
                            {
                                AddQuad(
                                    FVector(ABase.X, ABase.Y, Z0),
                                    FVector(ABase.X, ABase.Y, Z1),
                                    FVector(BBase.X, BBase.Y, Z1),
                                    FVector(BBase.X, BBase.Y, Z0));
                            }
                        }

                        if (bBuildMiddleBands)
                        {
                            const int32 ZMin = FMath::Max(FloorZ, NFloor);
                            const int32 ZMax = FMath::Min(CeilZ, NCeil);
                            if (ZMax > ZMin)
                            {
                                const float Z0 = (float)ZMin * UnitsToUU;
                                const float Z1 = (float)ZMax * UnitsToUU;
                                AddQuad(
                                    FVector(ABase.X, ABase.Y, Z0),
                                    FVector(ABase.X, ABase.Y, Z1),
                                    FVector(BBase.X, BBase.Y, Z1),
                                    FVector(BBase.X, BBase.Y, Z0));
                            }
                        }
                    }
                }
            }

            // Compute normals/tangents for walls from geometry
            UKismetProceduralMeshLibrary::CalculateTangentsForMesh(WallVerts, WallIndices, WallUV, WallNormals, WallTangents);

            if (WallVerts.Num() && WallIndices.Num())
            {
                AppendMeshToPMC(PMC_Walls, WallVerts, WallIndices, WallNormals, WallUV, WallColors, WallTangents);
            }
        }
    }

    UE_LOG(LogTemp, Display, TEXT("Built per-sector meshes."));
}

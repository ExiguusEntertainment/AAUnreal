#include "DoomWadLoader.h"
#include "DoomWadTypes.h"
#include "AAOriginalAPI.h"
#include "CoreMinimal.h"
#include "Misc/FileHelper.h"
#include "HAL/PlatformFilemanager.h"
#include "Misc/Paths.h"
#include "Logging/LogMacros.h"

DEFINE_LOG_CATEGORY_STATIC(LogDoomWad, Log, All);

namespace AA
{
#pragma pack(push,1)
    struct FDoomThing { int16 X; int16 Y; int16 Angle; int16 Type; int16 Flags; };
#pragma pack(pop)

    struct FLumpRef { int32 Offset = 0; int32 Size = 0; };

    static bool ReadWholeFile(const FString& AbsPath, TArray<uint8>& OutData)
    {
        if (!FFileHelper::LoadFileToArray(OutData, *AbsPath))
        {
            UE_LOG(LogDoomWad, Error, TEXT("Failed to read WAD file %s"), *AbsPath);
            return false;
        }
        return true;
    }

    static FString MakeUpper8(const char* Name)
    {
        TCHAR Temp[9] = {0};
        for (int i = 0; i < 8; i++)
        {
            char c = Name[i];
            if (c == 0) break;
            Temp[i] = TCHAR(FChar::ToUpper((TCHAR)c));
        }
        return FString(Temp).TrimEnd();
    }

    class FDoomWadParser
    {
    public:
        bool Load(const FString& WadPath, const FString& MapName)
        {
            Data.Reset();
            if (!ReadWholeFile(WadPath, Data)) return false;
            if (Data.Num() < (int32)sizeof(T_wadHeader)) { UE_LOG(LogDoomWad, Error, TEXT("File too small.")); return false; }

            const T_wadHeader* Header = reinterpret_cast<const T_wadHeader*>(Data.GetData());
            FString Sig = FString(ANSI_TO_TCHAR((const char*)Header->Signature)).Left(4);
            if (!(Sig == TEXT("IWAD") || Sig == TEXT("PWAD")))
            {
                UE_LOG(LogDoomWad, Error, TEXT("Bad WAD signature: %s"), *Sig);
                return false;
            }
            int32 NumLumps = Header->NumLumps;
            int32 DirOffset = Header->DirOffset;
            if (DirOffset + NumLumps * (int32)sizeof(T_directoryEntry) > Data.Num())
            {
                UE_LOG(LogDoomWad, Error, TEXT("Directory out of range."));
                return false;
            }
            const T_directoryEntry* Dir = reinterpret_cast<const T_directoryEntry*>(Data.GetData() + DirOffset);
            FString Wanted = MapName.ToUpper();
            int32 MapIndex = -1;
            for (int32 i = 0; i < NumLumps; i++)
            {
                FString Name = MakeUpper8((const char*)Dir[i].Name);
                if (Name == Wanted)
                {
                    MapIndex = i;
                    break;
                }
            }

            // Helper: gather required lumps either from a map section (after a map marker)
            // or globally (markerless single-map WADs)
            auto GatherLumpsFromSection = [&](int32 StartIndex, TMap<FString, FLumpRef>& OutLumps)
            {
                for (int32 i = StartIndex; i < NumLumps; i++)
                {
                    const T_directoryEntry& E = Dir[i];
                    FString Name = MakeUpper8((const char*)E.Name);
                    if (E.Size == 0) break; // hit next map marker
                    FLumpRef Ref; Ref.Offset = E.FilePos; Ref.Size = E.Size; OutLumps.Add(Name, Ref);
                }
            };

            auto GatherLumpsGlobally = [&](TMap<FString, FLumpRef>& OutLumps)
            {
                for (int32 i = 0; i < NumLumps; i++)
                {
                    const T_directoryEntry& E = Dir[i];
                    if (E.Size <= 0) continue; // skip markers or invalid
                    const FString Name = MakeUpper8((const char*)E.Name);
                    if (!OutLumps.Contains(Name))
                    {
                        FLumpRef Ref; Ref.Offset = E.FilePos; Ref.Size = E.Size; OutLumps.Add(Name, Ref);
                    }
                }
            };

            TMap<FString, FLumpRef> Lumps;
            static const TCHAR* Needed[] = { TEXT("VERTEXES"), TEXT("LINEDEFS"), TEXT("SIDEDEFS"), TEXT("SECTORS") };

            if (MapIndex != -1)
            {
                GatherLumpsFromSection(MapIndex + 1, Lumps);
            }
            else
            {
                // Fallback: try markerless single-map WADs by scanning lumps globally
                UE_LOG(LogDoomWad, Warning, TEXT("Map marker %s not found. Attempting markerless single-map fallback."), *MapName);
                GatherLumpsGlobally(Lumps);
            }

            // Validate required lumps
            for (auto* N : Needed)
            {
                if (!Lumps.Contains(N))
                {
                    UE_LOG(LogDoomWad, Error, TEXT("Missing required lump %s"), N);
                    return false;
                }
            }
            if (Lumps[TEXT("VERTEXES")].Size % sizeof(T_3dVertex) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad VERTEXES size")); return false; }
            if (Lumps[TEXT("LINEDEFS")].Size % sizeof(T_3dLine) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad LINEDEFS size")); return false; }
            if (Lumps[TEXT("SIDEDEFS")].Size % sizeof(T_3dSide) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad SIDEDEFS size")); return false; }
            if (Lumps[TEXT("SECTORS")].Size % sizeof(T_3dSector) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad SECTORS size")); return false; }

            Vertices = MakeArray<T_3dVertex>(Lumps[TEXT("VERTEXES")]);
            Lines = MakeArray<T_3dLine>(Lumps[TEXT("LINEDEFS")]);
            Sides = MakeArray<T_3dSide>(Lumps[TEXT("SIDEDEFS")]);
            Sectors = MakeArray<T_3dSector>(Lumps[TEXT("SECTORS")]);

            // Optional BSP lumps
            if (Lumps.Contains(TEXT("SEGS")) && Lumps[TEXT("SEGS")].Size % sizeof(T_3dSeg) == 0)
            {
                Segs = MakeArray<T_3dSeg>(Lumps[TEXT("SEGS")]);
            }
            if (Lumps.Contains(TEXT("SSECTORS")) && Lumps[TEXT("SSECTORS")].Size % sizeof(T_3dSubSector) == 0)
            {
                SubSectors = MakeArray<T_3dSubSector>(Lumps[TEXT("SSECTORS")]);
            }
            if (Lumps.Contains(TEXT("NODES")) && Lumps[TEXT("NODES")].Size % sizeof(T_3dNode) == 0)
            {
                Nodes = MakeArray<T_3dNode>(Lumps[TEXT("NODES")]);
            }

            UE_LOG(LogDoomWad, Display, TEXT("Loaded map %s: V=%d L=%d Sides=%d Sec=%d Segs=%d SSub=%d Nodes=%d"), *MapName, Vertices.Num(), Lines.Num(), Sides.Num(), Sectors.Num(), Segs.Num(), SubSectors.Num(), Nodes.Num());
            return true;
        }

        template<typename T>
        TArray<T> MakeArray(const FLumpRef& Ref)
        {
            TArray<T> Arr;
            const uint8* Src = Data.GetData() + Ref.Offset;
            int32 Count = Ref.Size / sizeof(T);
            Arr.AddUninitialized(Count);
            FMemory::Memcpy(Arr.GetData(), Src, Ref.Size);
            return Arr;
        }

        const TArray<T_3dVertex>& GetVertices() const { return Vertices; }
        const TArray<T_3dLine>& GetLines() const { return Lines; }
        const TArray<T_3dSide>& GetSides() const { return Sides; }
        const TArray<T_3dSector>& GetSectors() const { return Sectors; }
        const TArray<T_3dSeg>& GetSegs() const { return Segs; }
        const TArray<T_3dSubSector>& GetSubSectors() const { return SubSectors; }
        const TArray<T_3dNode>& GetNodes() const { return Nodes; }

    private:
        TArray<uint8> Data;
        TArray<T_3dVertex> Vertices;
        TArray<T_3dLine> Lines;
        TArray<T_3dSide> Sides;
        TArray<T_3dSector> Sectors;
        TArray<T_3dSeg> Segs;
        TArray<T_3dSubSector> SubSectors;
        TArray<T_3dNode> Nodes;
    };

    static FDoomWadLoadStats G_LastStats;
    static TArray<T_3dVertex> G_LastVertices;
    static TArray<T_3dLine> G_LastLines;
    static TArray<T_3dSide> G_LastSides;
    static TArray<T_3dSector> G_LastSectors;
    static TArray<T_3dSeg> G_LastSegs;
    static TArray<T_3dSubSector> G_LastSubSectors;
    static TArray<T_3dNode> G_LastNodes;
}

static void UpdateStats(const AA::FDoomWadParser& Parser, bool bSuccess)
{
    FDoomWadLoadStats Stats;
    if (bSuccess)
    {
        Stats.NumVertices = Parser.GetVertices().Num();
        Stats.NumLines = Parser.GetLines().Num();
        Stats.NumSides = Parser.GetSides().Num();
        Stats.NumSectors = Parser.GetSectors().Num();
        Stats.bValid = true;

        AA::G_LastVertices = Parser.GetVertices();
        AA::G_LastLines = Parser.GetLines();
        AA::G_LastSides = Parser.GetSides();
        AA::G_LastSectors = Parser.GetSectors();
        AA::G_LastSegs = Parser.GetSegs();
        AA::G_LastSubSectors = Parser.GetSubSectors();
        AA::G_LastNodes = Parser.GetNodes();
    }
    else
    {
        AA::G_LastVertices.Reset();
        AA::G_LastLines.Reset();
        AA::G_LastSides.Reset();
        AA::G_LastSectors.Reset();
        AA::G_LastSegs.Reset();
        AA::G_LastSubSectors.Reset();
        AA::G_LastNodes.Reset();
    }
    AA::G_LastStats = Stats;
}

AAORIGINAL_API bool AA_LoadDoomMap(const FString& WadPath, const FString& MapName)
{
    AA::FDoomWadParser Parser;
    const bool bOk = Parser.Load(WadPath, MapName);
    UpdateStats(Parser, bOk);
    return bOk;
}

bool UDoomWadLoader::LoadDoomWadMap(const FString& WadAbsolutePath, const FString& MapName)
{
    return AA_LoadDoomMap(WadAbsolutePath, MapName);
}

FDoomWadLoadStats UDoomWadLoader::GetLastLoadStats()
{
    return AA::G_LastStats;
}

bool UDoomWadLoader::GetLastWad2DGeometry(TArray<FVector2D>& OutVertices, TArray<FIntPoint>& OutLines)
{
    if (!AA::G_LastStats.bValid || AA::G_LastVertices.Num() == 0 || AA::G_LastLines.Num() == 0)
    {
        return false;
    }
    OutVertices.Reset(AA::G_LastVertices.Num());
    for (const T_3dVertex& V : AA::G_LastVertices)
    {
        OutVertices.Add(FVector2D((float)V.X, (float)V.Y));
    }
    OutLines.Reset(AA::G_LastLines.Num());
    for (const T_3dLine& L : AA::G_LastLines)
    {
        OutLines.Add(FIntPoint((int32)L.From, (int32)L.To));
    }
    return true;
}

bool UDoomWadLoader::GetLastWadWallTopology(TArray<FVector2D>& OutVertices, TArray<FIntPoint>& OutLines, TArray<FIntPoint>& OutLineSectors, TArray<FIntPoint>& OutSectorHeights)
{
    if (!AA::G_LastStats.bValid || AA::G_LastVertices.Num() == 0 || AA::G_LastLines.Num() == 0 || AA::G_LastSectors.Num() == 0)
    {
        return false;
    }

    // Vertices and lines
    OutVertices.Reset(AA::G_LastVertices.Num());
    for (const T_3dVertex& V : AA::G_LastVertices)
    {
        OutVertices.Add(FVector2D((float)V.X, (float)V.Y));
    }
    OutLines.Reset(AA::G_LastLines.Num());
    for (const T_3dLine& L : AA::G_LastLines)
    {
        OutLines.Add(FIntPoint((int32)L.From, (int32)L.To));
    }

    // Sector heights
    OutSectorHeights.Reset(AA::G_LastSectors.Num());
    for (const T_3dSector& S : AA::G_LastSectors)
    {
        OutSectorHeights.Add(FIntPoint((int32)S.FloorHeight, (int32)S.CeilingHeight));
    }

    // For each linedef, map to front/back sector via sidedefs
    OutLineSectors.Reset(AA::G_LastLines.Num());
    for (int32 i = 0; i < AA::G_LastLines.Num(); ++i)
    {
        const T_3dLine& L = AA::G_LastLines[i];
        int32 FrontSector = -1;
        int32 BackSector = -1;
        // Front side
        if (L.Side[0] >= 0 && L.Side[0] < AA::G_LastSides.Num())
        {
            const T_3dSide& SD = AA::G_LastSides[L.Side[0]];
            if (SD.Sector >= 0 && SD.Sector < AA::G_LastSectors.Num())
            {
                FrontSector = (int32)SD.Sector;
            }
        }
        // Back side
        if (L.Side[1] >= 0 && L.Side[1] < AA::G_LastSides.Num())
        {
            const T_3dSide& SD = AA::G_LastSides[L.Side[1]];
            if (SD.Sector >= 0 && SD.Sector < AA::G_LastSectors.Num())
            {
                BackSector = (int32)SD.Sector;
            }
        }
        OutLineSectors.Add(FIntPoint(FrontSector, BackSector));
    }
    return true;
}

static bool ChainSubsectorSegsToPolys(const TArray<T_3dSeg>& InSegs, const TArray<T_3dVertex>& AllVerts,
                                      const TArray<T_3dLine>& AllLines, const TArray<T_3dSide>& AllSides,
                                      const TArray<T_3dSector>& AllSectors, int32 WantedSector,
                                      TArray<TArray<int32>>& OutLoops)
{
    OutLoops.Reset();
    if (InSegs.Num() == 0) return false;

    // Build adjacency: seg i -> from vertex, to vertex
    struct FSegInfo { int32 From; int32 To; int32 Linedef; int32 Side; };
    TArray<FSegInfo> S; S.Reserve(InSegs.Num());
    for (const auto& SG : InSegs)
    {
        FSegInfo Info { (int32)SG.From, (int32)SG.To, (int32)SG.Linedef, (int32)SG.Side };
        // Filter seg that doesn't belong to this sector
        int32 FrontSector = -1, BackSector = -1;
        if (Info.Linedef >= 0 && Info.Linedef < AllLines.Num())
        {
            const auto& L = AllLines[Info.Linedef];
            if (Info.Side == 0 && L.Side[0] >= 0 && L.Side[0] < AllSides.Num()) FrontSector = AllSides[L.Side[0]].Sector;
            if (Info.Side == 1 && L.Side[1] >= 0 && L.Side[1] < AllSides.Num()) BackSector = AllSides[L.Side[1]].Sector;
        }
        const bool bBelongs = (Info.Side == 0) ? (FrontSector == WantedSector) : (BackSector == WantedSector);
        if (!bBelongs) continue;
        S.Add(Info);
    }
    if (S.Num() == 0) return false;

    // Greedy chain-building per seg set
    TMultiMap<int32,int32> FromTo;
    TSet<int32> Remaining;
    for (int32 i=0;i<S.Num();++i)
    {
        FromTo.Add(S[i].From, S[i].To);
        Remaining.Add(i);
    }

    while (Remaining.Num() > 0)
    {
        int32 StartSegIdx = *Remaining.CreateConstIterator();
        int32 StartFrom = S[StartSegIdx].From;
        int32 StartTo   = S[StartSegIdx].To;
        TArray<int32> Loop; Loop.Add(StartFrom); Loop.Add(StartTo);
        Remaining.Remove(StartSegIdx);

        int32 Curr = StartTo;
        int32 Guard=0;
        while (Guard++ < 100000)
        {
            TArray<int32> NextCandidates; FromTo.MultiFind(Curr, NextCandidates);
            int32 NextV = INDEX_NONE;
            for (int32 v : NextCandidates)
            {
                // verify exists unused seg from Curr->v
                bool bFound = false; int32 FoundIdx = -1;
                for (int32 idx : Remaining)
                {
                    if (S[idx].From == Curr && S[idx].To == v) { bFound = true; FoundIdx = idx; break; }
                }
                if (bFound) { NextV = v; Remaining.Remove(FoundIdx); break; }
            }
            if (NextV == INDEX_NONE) break;
            if (NextV == Loop[0]) { Loop.Add(NextV); break; }
            Loop.Add(NextV);
            Curr = NextV;
        }
        if (Loop.Num() >= 3) OutLoops.Add(Loop);
    }
    return OutLoops.Num() > 0;
}

bool UDoomWadLoader::GetLastWadSectorSubsectorPolys(int32 SectorIndex, TArray<int32>& OutPolyOffsets, TArray<int32>& OutPolyCounts, TArray<FVector2D>& OutPolyVerts)
{
    using namespace AA;
    OutPolyOffsets.Reset(); OutPolyCounts.Reset(); OutPolyVerts.Reset();
    if (!G_LastStats.bValid || SectorIndex < 0 || SectorIndex >= G_LastSectors.Num()) return false;
    if (G_LastSegs.Num() == 0 || G_LastSubSectors.Num() == 0) return false;

    // Build subsector -> segs mapping
    TArray<TArray<int32>> SubsectorSegIndices; SubsectorSegIndices.SetNum(G_LastSubSectors.Num());
    for (int32 ssi = 0; ssi < G_LastSubSectors.Num(); ++ssi)
    {
        const auto& SS = G_LastSubSectors[ssi];
        for (int32 k=0;k<SS.NumSegs; ++k)
        {
            const int32 SegIndex = (int32)SS.FirstSeg + k;
            if (G_LastSegs.IsValidIndex(SegIndex))
            {
                SubsectorSegIndices[ssi].Add(SegIndex);
            }
        }
    }

    // For each subsector, if any seg points into SectorIndex (via linedef+side), collect a poly by chaining segs
    for (int32 ssi=0; ssi<SubsectorSegIndices.Num(); ++ssi)
    {
        TArray<T_3dSeg> SegsForSS;
        for (int32 segi : SubsectorSegIndices[ssi]) SegsForSS.Add(G_LastSegs[segi]);

        TArray<TArray<int32>> Loops;
        if (!ChainSubsectorSegsToPolys(SegsForSS, G_LastVertices, G_LastLines, G_LastSides, G_LastSectors, SectorIndex, Loops))
        {
            continue;
        }

        for (const TArray<int32>& Loop : Loops)
        {
            const int32 Start = OutPolyVerts.Num();
            int32 Count = 0;
            // If last equals first, drop the last
            const int32 End = (Loop.Last() == Loop[0]) ? Loop.Num()-1 : Loop.Num();
            for (int32 i=0;i<End;++i)
            {
                const int32 vi = Loop[i];
                if (!G_LastVertices.IsValidIndex(vi)) continue;
                const auto& V = G_LastVertices[vi];
                OutPolyVerts.Add(FVector2D((float)V.X, (float)V.Y));
                ++Count;
            }
            if (Count >= 3)
            {
                OutPolyOffsets.Add(Start);
                OutPolyCounts.Add(Count);
            }
            else
            {
                // discard unused verts
                OutPolyVerts.SetNum(Start);
            }
        }
    }

    return OutPolyOffsets.Num() > 0;
}

extern "C" AAORIGINAL_API void AA_GetLastDoomWadStats(int32& OutVerts, int32& OutLines, int32& OutSides, int32& OutSectors, bool& bValid)
{
    const FDoomWadLoadStats S = UDoomWadLoader::GetLastLoadStats();
    OutVerts = S.NumVertices;
    OutLines = S.NumLines;
    OutSides = S.NumSides;
    OutSectors = S.NumSectors;
    bValid = S.bValid;
}

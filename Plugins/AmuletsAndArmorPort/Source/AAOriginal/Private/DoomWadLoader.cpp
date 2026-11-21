#include "DoomWadLoader.h"
#include "DoomWadTypes.h"
#include "AAOriginalAPI.h"
#include "CoreMinimal.h"
#include "CompGeom/Delaunay2.h"
#include "Misc/FileHelper.h"
#include "Misc/Base64.h"
#include "HAL/PlatformFilemanager.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Logging/LogMacros.h"
#include "Serialization/JsonWriter.h"

DEFINE_LOG_CATEGORY_STATIC(LogDoomWad, Log, All);

namespace AA
{
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

    template<int32 Length>
    static FString ReadWadName(const char (&Name)[Length])
    {
        TCHAR Temp[Length + 1] = {};
        for (int32 i = 0; i < Length; ++i)
        {
            char c = Name[i];
            if (c == 0) break;
            Temp[i] = TCHAR(FChar::ToUpper((TCHAR)c));
        }
        return FString(Temp).TrimEnd();
    }

    static bool IsStandardMapMarker(const FString& Name)
    {
        return (Name.Len() == 5 && Name.StartsWith(TEXT("MAP"))
                && FChar::IsDigit(Name[3]) && FChar::IsDigit(Name[4]))
            || (Name.Len() == 4 && Name[0] == 'E' && FChar::IsDigit(Name[1])
                && Name[2] == 'M' && FChar::IsDigit(Name[3]));
    }

    static const FString RequiredLumpNames[] = { TEXT("VERTEXES"), TEXT("LINEDEFS"), TEXT("SIDEDEFS"), TEXT("SECTORS") };
    static const FString OptionalLumpNames[] = { TEXT("THINGS"), TEXT("SEGS"), TEXT("SSECTORS"), TEXT("NODES"), TEXT("REJECT"), TEXT("BLOCKMAP") };

    class FDoomWadParser
    {
    public:
        bool Load(const FString& WadPath, const FString& MapName)
        {
            Data.Reset();
            if (!ReadWholeFile(WadPath, Data)) return false;
            if (Data.Num() < (int32)sizeof(T_wadHeader)) { UE_LOG(LogDoomWad, Error, TEXT("File too small.")); return false; }

            const T_wadHeader* Header = reinterpret_cast<const T_wadHeader*>(Data.GetData());
            const FString Sig = ReadWadName(Header->Signature);
            if (!(Sig == TEXT("IWAD") || Sig == TEXT("PWAD")))
            {
                UE_LOG(LogDoomWad, Error, TEXT("Bad WAD signature: %s"), *Sig);
                return false;
            }
            int32 NumLumps = Header->NumLumps;
            int32 DirOffset = Header->DirOffset;
            if (DirOffset < int32(sizeof(T_wadHeader)) || NumLumps <= 0
                || int64(DirOffset) + int64(NumLumps) * sizeof(T_directoryEntry) > Data.Num())
            {
                UE_LOG(LogDoomWad, Error, TEXT("Directory out of range."));
                return false;
            }
            const T_directoryEntry* Dir = reinterpret_cast<const T_directoryEntry*>(Data.GetData() + DirOffset);
            const FString Wanted = MapName.ToUpper();
            int32 MapIndex = INDEX_NONE;
            int32 SingleMarkerIndex = INDEX_NONE;
            int32 NumMarkers = 0;
            for (int32 i = 0; i < NumLumps; i++)
            {
                const T_directoryEntry& Entry = Dir[i];
                const FString Name = ReadWadName(Entry.Name);
                if (Entry.Size < 0 || (Entry.Size > 0 && (Entry.FilePos < 0 || int64(Entry.FilePos) + Entry.Size > Data.Num())))
                {
                    UE_LOG(LogDoomWad, Error, TEXT("Lump %s is out of range."), *Name);
                    return false;
                }
                if (MapIndex == INDEX_NONE && Name == Wanted)
                {
                    MapIndex = i;
                }
                if (IsStandardMapMarker(Name))
                {
                    SingleMarkerIndex = i;
                    ++NumMarkers;
                }
            }

            if (MapIndex == INDEX_NONE && NumMarkers > 1)
            {
                UE_LOG(LogDoomWad, Error, TEXT("Map marker %s not found in a WAD with multiple maps."), *Wanted);
                return false;
            }
            if (MapIndex == INDEX_NONE && NumMarkers == 1)
            {
                MapIndex = SingleMarkerIndex;
            }
            Map.Marker = MapIndex != INDEX_NONE ? ReadWadName(Dir[MapIndex].Name) : FString();
            Signature = Sig;
            FirstEntry = MapIndex != INDEX_NONE ? MapIndex : 0;
            EndEntry = NumLumps;

            TMap<FString, FLumpRef> Lumps;
            for (int32 i = MapIndex + 1; i < NumLumps; ++i)
            {
                const T_directoryEntry& Entry = Dir[i];
                const FString Name = ReadWadName(Entry.Name);
                const bool bRequired = MakeArrayView(RequiredLumpNames).Contains(Name);
                if (MapIndex != INDEX_NONE && (IsStandardMapMarker(Name)
                    || (Entry.Size == 0 && !bRequired && !MakeArrayView(OptionalLumpNames).Contains(Name))))
                {
                    EndEntry = i;
                    break;
                }
                if (MapIndex == INDEX_NONE && Lumps.Contains(Name))
                {
                    if (bRequired)
                    {
                        UE_LOG(LogDoomWad, Error, TEXT("Ambiguous markerless WAD: repeated %s lump."), *Name);
                        return false;
                    }
                    continue;
                }
                Lumps.Add(Name, FLumpRef{Entry.FilePos, Entry.Size});
            }

            // Validate required lumps
            for (const FString& Name : RequiredLumpNames)
            {
                if (!Lumps.Contains(Name))
                {
                    UE_LOG(LogDoomWad, Error, TEXT("Missing required lump %s"), *Name);
                    return false;
                }
            }
            if (Lumps[TEXT("VERTEXES")].Size % sizeof(T_3dVertex) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad VERTEXES size")); return false; }
            if (Lumps[TEXT("LINEDEFS")].Size % sizeof(T_3dLine) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad LINEDEFS size")); return false; }
            if (Lumps[TEXT("SIDEDEFS")].Size % sizeof(T_3dSide) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad SIDEDEFS size")); return false; }
            if (Lumps[TEXT("SECTORS")].Size % sizeof(T_3dSector) != 0) { UE_LOG(LogDoomWad, Error, TEXT("Bad SECTORS size")); return false; }

            Map.Vertices = MakeArray<T_3dVertex>(Lumps[TEXT("VERTEXES")]);
            Map.Lines = MakeArray<T_3dLine>(Lumps[TEXT("LINEDEFS")]);
            Map.Sides = MakeArray<T_3dSide>(Lumps[TEXT("SIDEDEFS")]);
            Map.Sectors = MakeArray<T_3dSector>(Lumps[TEXT("SECTORS")]);
            Map.Things = MakeArray<FWadThing>(Lumps.FindRef(TEXT("THINGS")));
            Map.Segs = MakeArray<T_3dSeg>(Lumps.FindRef(TEXT("SEGS")));
            Map.SubSectors = MakeArray<T_3dSubSector>(Lumps.FindRef(TEXT("SSECTORS")));
            Map.Nodes = MakeArray<T_3dNode>(Lumps.FindRef(TEXT("NODES")));

            UE_LOG(LogDoomWad, Display, TEXT("Loaded map %s: V=%d L=%d Sides=%d Sec=%d Segs=%d SSub=%d Nodes=%d"), *MapName, Map.Vertices.Num(), Map.Lines.Num(), Map.Sides.Num(), Map.Sectors.Num(), Map.Segs.Num(), Map.SubSectors.Num(), Map.Nodes.Num());
            return true;
        }

        template<typename T>
        TArray<T> MakeArray(const FLumpRef& Ref) const
        {
            TArray<T> Arr;
            if (Ref.Size > 0 && Ref.Size % sizeof(T) == 0)
            {
                Arr.AddUninitialized(Ref.Size / sizeof(T));
                FMemory::Memcpy(Arr.GetData(), Data.GetData() + Ref.Offset, Ref.Size);
            }
            return Arr;
        }

        TArrayView<const T_directoryEntry> GetSelectedDirectory() const
        {
            const auto* Header = reinterpret_cast<const T_wadHeader*>(Data.GetData());
            const auto* Dir = reinterpret_cast<const T_directoryEntry*>(Data.GetData() + Header->DirOffset);
            return MakeArrayView(Dir + FirstEntry, EndEntry - FirstEntry);
        }

        TArrayView<const uint8> GetLumpBytes(const T_directoryEntry& Entry) const
        {
            return Entry.Size > 0 ? MakeArrayView(Data.GetData() + Entry.FilePos, Entry.Size) : TArrayView<const uint8>();
        }

        FDoomWadMapData Map;
        FString Signature;

    private:
        TArray<uint8> Data;
        int32 FirstEntry = 0;
        int32 EndEntry = 0;
    };

    static FDoomWadLoadStats G_LastStats;
    static FDoomWadMapData G_LastMap;

    using FMapJsonWriter = TJsonWriter<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>;

    template<typename RecordType, typename WriteFields>
    static void WriteRecords(FMapJsonWriter& Writer, const TCHAR* Name, const TArray<RecordType>& Records, WriteFields Write)
    {
        Writer.WriteArrayStart(Name);
        for (int32 Index = 0; Index < Records.Num(); ++Index)
        {
            Writer.WriteObjectStart();
            Writer.WriteValue(TEXT("id"), Index);
            Write(Records[Index]);
            Writer.WriteObjectEnd();
        }
        Writer.WriteArrayEnd();
    }

    static uint16 ReadWord(TArrayView<const uint8> Bytes, int32 Offset)
    {
        return uint16(Bytes[Offset]) | (uint16(Bytes[Offset + 1]) << 8);
    }

    // Preserve all supplemental bytes. Expose word-oriented tables as numbers for inspection.
    static void WriteSupplementalBytes(FMapJsonWriter& Writer, TArrayView<const uint8> Bytes, bool bWords)
    {
        if (bWords && Bytes.Num() % 2 == 0)
        {
            Writer.WriteArrayStart(TEXT("wordsLe16"));
            for (int32 Offset = 0; Offset < Bytes.Num(); Offset += 2)
            {
                Writer.WriteValue(ReadWord(Bytes, Offset));
            }
            Writer.WriteArrayEnd();
        }
        else
        {
            Writer.WriteValue(TEXT("base64"), FBase64::Encode(Bytes.GetData(), Bytes.Num()));
        }
    }
}

AAORIGINAL_API bool AA_LoadDoomMap(const FString& WadPath, const FString& MapName)
{
    AA::FDoomWadParser Parser;
    const bool bOk = Parser.Load(WadPath, MapName);
    AA::G_LastMap = bOk ? MoveTemp(Parser.Map) : FDoomWadMapData();
    AA::G_LastStats.NumVertices = AA::G_LastMap.Vertices.Num();
    AA::G_LastStats.NumLines = AA::G_LastMap.Lines.Num();
    AA::G_LastStats.NumSides = AA::G_LastMap.Sides.Num();
    AA::G_LastStats.NumSectors = AA::G_LastMap.Sectors.Num();
    AA::G_LastStats.bValid = bOk;
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

const FDoomWadMapData* UDoomWadLoader::GetLastWadMapData()
{
    return AA::G_LastStats.bValid ? &AA::G_LastMap : nullptr;
}

static TArray<UE::Geometry::FIndex2i> NormalizeOverlappingWadEdges(
    TConstArrayView<FVector2D> Vertices, TConstArrayView<UE::Geometry::FIndex2i> Edges)
{
    TMap<FIntPoint, uint8> EdgeDirections;
    for (const UE::Geometry::FIndex2i& Edge : Edges)
    {
        const FVector2D Origin = Vertices[Edge.A];
        const FVector2D Direction = Vertices[Edge.B] - Origin;
        const double LengthSquared = Direction.SizeSquared();
        TArray<TPair<double, int32>, TInlineAllocator<4>> Points{{0, Edge.A}, {LengthSquared, Edge.B}};
        for (int32 Index = 0; Index < Vertices.Num(); ++Index)
        {
            const FVector2D Offset = Vertices[Index] - Origin;
            const double Along = FVector2D::DotProduct(Direction, Offset);
            if (Along > 0 && Along < LengthSquared && FVector2D::CrossProduct(Direction, Offset) == 0)
            {
                Points.Emplace(Along, Index);
            }
        }
        Points.Sort([](const auto& A, const auto& B) { return A.Key < B.Key; });
        for (int32 Index = 1; Index < Points.Num(); ++Index)
        {
            const int32 A = Points[Index - 1].Value;
            const int32 B = Points[Index].Value;
            const FIntPoint Segment(FMath::Min(A, B), FMath::Max(A, B));
            EdgeDirections.FindOrAdd(Segment) |= A < B ? 1 : 2;
        }
    }

    TArray<UE::Geometry::FIndex2i> Result;
    // Collapse duplicates along the same side. Opposing edges put the sector on both sides of the segment.
    for (const auto& [Segment, Direction] : EdgeDirections)
    {
        if (Direction == 1)
        {
            Result.Emplace(Segment.X, Segment.Y);
        }
        else if (Direction == 2)
        {
            Result.Emplace(Segment.Y, Segment.X);
        }
    }
    return Result;
}

static bool SplitCrossingWadEdges(TArray<FVector2D>& Vertices, TArray<UE::Geometry::FIndex2i>& Edges)
{
    using FPointOnEdge = TPair<double, int32>;
    TArray<TArray<FPointOnEdge, TInlineAllocator<4>>> Cuts;
    Cuts.SetNum(Edges.Num());
    bool bFoundCrossing = false;
    for (int32 First = 0; First < Edges.Num(); ++First)
    {
        const FVector2D A = Vertices[Edges[First].A];
        const FVector2D DirectionA = Vertices[Edges[First].B] - A;
        for (int32 Second = First + 1; Second < Edges.Num(); ++Second)
        {
            const FVector2D B = Vertices[Edges[Second].A];
            const FVector2D DirectionB = Vertices[Edges[Second].B] - B;
            const double Cross = FVector2D::CrossProduct(DirectionA, DirectionB);
            if (Cross == 0)
            {
                continue;
            }
            const double AlongA = FVector2D::CrossProduct(B - A, DirectionB) / Cross;
            const double AlongB = FVector2D::CrossProduct(B - A, DirectionA) / Cross;
            if (AlongA <= 0 || AlongA >= 1 || AlongB <= 0 || AlongB >= 1)
            {
                continue;
            }
            const FVector2D Point = A + AlongA * DirectionA;
            int32 Vertex = Vertices.IndexOfByPredicate([&Point](const FVector2D& Existing)
            {
                return Existing.Equals(Point, UE_DOUBLE_KINDA_SMALL_NUMBER);
            });
            if (Vertex == INDEX_NONE)
            {
                Vertex = Vertices.Add(Point);
            }
            Cuts[First].AddUnique({AlongA, Vertex});
            Cuts[Second].AddUnique({AlongB, Vertex});
            bFoundCrossing = true;
        }
    }
    if (!bFoundCrossing)
    {
        return false;
    }

    TArray<UE::Geometry::FIndex2i> Result;
    for (int32 Index = 0; Index < Edges.Num(); ++Index)
    {
        auto& Points = Cuts[Index];
        Points.Sort([](const FPointOnEdge& A, const FPointOnEdge& B) { return A.Key < B.Key; });
        int32 Start = Edges[Index].A;
        for (const FPointOnEdge& Cut : Points)
        {
            if (Start != Cut.Value)
            {
                Result.Emplace(Start, Cut.Value);
                Start = Cut.Value;
            }
        }
        if (Start != Edges[Index].B)
        {
            Result.Emplace(Start, Edges[Index].B);
        }
    }
    Edges = MoveTemp(Result);
    return true;
}

bool UDoomWadLoader::GetLastWadSectorFootprint(int32 SectorIndex, FDoomWadSectorFootprint& OutFootprint)
{
    OutFootprint = FDoomWadSectorFootprint();
    const FDoomWadMapData* Map = GetLastWadMapData();
    if (!Map || !Map->Sectors.IsValidIndex(SectorIndex))
    {
        return false;
    }

    FDoomWadSectorFootprint Footprint;
    TMap<FIntPoint, int32> VertexByPosition;
    TArray<UE::Geometry::FIndex2i> Constraints;
    const auto GetOrAddVertex = [&](const FWadVertex& Vertex)
    {
        const FIntPoint Position(Vertex.X, Vertex.Y);
        if (const int32* Index = VertexByPosition.Find(Position))
        {
            return *Index;
        }
        const int32 Index = Footprint.Vertices.Emplace(Position.X, Position.Y);
        VertexByPosition.Add(Position, Index);
        return Index;
    };

    for (const FWadLineDef& Line : Map->Lines)
    {
        const bool bFront = Map->GetSectorForSide(Line.Side[0]) == SectorIndex;
        const bool bBack = Map->GetSectorForSide(Line.Side[1]) == SectorIndex;
        if (bFront == bBack)
        {
            continue;
        }
        if (!Map->Vertices.IsValidIndex(Line.From) || !Map->Vertices.IsValidIndex(Line.To))
        {
            return false;
        }

        const FWadVertex& A = Map->Vertices[bFront ? Line.From : Line.To];
        const FWadVertex& B = Map->Vertices[bFront ? Line.To : Line.From];
        if (A.X == B.X && A.Y == B.Y)
        {
            continue;
        }
        const int32 From = GetOrAddVertex(A);
        const int32 To = GetOrAddVertex(B);
        Constraints.Emplace(From, To);
    }

    TArray<UE::Geometry::FIndex3i> FilledTriangles;
    const auto Fill = [&](TConstArrayView<UE::Geometry::FIndex2i> Edges)
    {
        TArray<int32> EdgeBalance;
        EdgeBalance.SetNumZeroed(Footprint.Vertices.Num());
        for (const UE::Geometry::FIndex2i& Edge : Edges)
        {
            ++EdgeBalance[Edge.A];
            --EdgeBalance[Edge.B];
        }
        if (Edges.Num() < 3 || EdgeBalance.ContainsByPredicate([](int32 Balance) { return Balance != 0; }))
        {
            return false;
        }
        UE::Geometry::FDelaunay2 Triangulation;
        FilledTriangles.Reset();
        // Odd winding fills exterior rings and islands while keeping nested holes clear.
        return Triangulation.Triangulate(Footprint.Vertices, Edges)
            && Triangulation.GetFilledTriangles(FilledTriangles, Edges, UE::Geometry::FDelaunay2::EFillMode::OddWinding)
            && !FilledTriangles.IsEmpty();
    };

    if (!Fill(Constraints))
    {
        // Some maps have a short linedef lying on a longer boundary of the same sector.
        Constraints = NormalizeOverlappingWadEdges(Footprint.Vertices, Constraints);
        if (!Fill(Constraints)
            && (!SplitCrossingWadEdges(Footprint.Vertices, Constraints) || !Fill(Constraints)))
        {
            return false;
        }
        Footprint.bNormalizedOverlap = true;
    }
    for (const UE::Geometry::FIndex2i& Edge : Constraints)
    {
        Footprint.BoundaryEdges.Emplace(Edge.A, Edge.B);
    }

    Footprint.Triangles.Reserve(3 * FilledTriangles.Num());
    for (const UE::Geometry::FIndex3i& Triangle : FilledTriangles)
    {
        const FVector2D& A = Footprint.Vertices[Triangle.A];
        const FVector2D& B = Footprint.Vertices[Triangle.B];
        const FVector2D& C = Footprint.Vertices[Triangle.C];
        const bool bCounterClockwise = FVector2D::CrossProduct(B - A, C - A) > 0;
        Footprint.Triangles.Add(Triangle.A);
        Footprint.Triangles.Add(bCounterClockwise ? Triangle.B : Triangle.C);
        Footprint.Triangles.Add(bCounterClockwise ? Triangle.C : Triangle.B);
    }
    OutFootprint = MoveTemp(Footprint);
    return true;
}

bool UDoomWadLoader::ExportDoomWadMapJson(const FString& WadPath, const FString& MapName, const FString& OutputPath)
{
    using namespace AA;
    if (!FPaths::GetExtension(OutputPath).Equals(TEXT("json"), ESearchCase::IgnoreCase)
        || FPaths::IsSamePath(WadPath, OutputPath))
    {
        UE_LOG(LogDoomWad, Error, TEXT("Specify a separate .json output path."));
        return false;
    }

    FDoomWadParser Parser;
    if (!Parser.Load(WadPath, MapName))
    {
        return false;
    }

    FString Json;
    const auto Writer = TJsonWriterFactory<TCHAR, TPrettyJsonPrintPolicy<TCHAR>>::Create(&Json);
    const FDoomWadMapData& Map = Parser.Map;
    Writer->WriteObjectStart();
    Writer->WriteValue(TEXT("schemaVersion"), 1);
    Writer->WriteValue(TEXT("sourceFile"), FPaths::GetCleanFilename(WadPath));
    Writer->WriteValue(TEXT("mapName"), MapName);
    Writer->WriteValue(TEXT("mapMarker"), Map.Marker);
    Writer->WriteValue(TEXT("signature"), Parser.Signature);
    WriteRecords(*Writer, TEXT("vertices"), Map.Vertices, [&](const FWadVertex& Vertex)
    {
        Writer->WriteValue(TEXT("x"), Vertex.X);
        Writer->WriteValue(TEXT("y"), Vertex.Y);
    });
    WriteRecords(*Writer, TEXT("lines"), Map.Lines, [&](const FWadLineDef& Line)
    {
        Writer->WriteValue(TEXT("fromVertexId"), Line.From);
        Writer->WriteValue(TEXT("toVertexId"), Line.To);
        Writer->WriteValue(TEXT("flags"), Line.Flags);
        Writer->WriteValue(TEXT("special"), Line.Special);
        Writer->WriteValue(TEXT("tag"), Line.Tag);
        Writer->WriteValue(TEXT("frontSideId"), Line.Side[0]);
        Writer->WriteValue(TEXT("backSideId"), Line.Side[1]);
        Writer->WriteValue(TEXT("frontSectorId"), Map.GetSectorForSide(Line.Side[0]));
        Writer->WriteValue(TEXT("backSectorId"), Map.GetSectorForSide(Line.Side[1]));
    });
    WriteRecords(*Writer, TEXT("sides"), Map.Sides, [&](const FWadSideDef& Side)
    {
        Writer->WriteValue(TEXT("sectorId"), Side.Sector);
        Writer->WriteValue(TEXT("textureOffsetX"), Side.TexXOffset);
        Writer->WriteValue(TEXT("textureOffsetY"), Side.TexYOffset);
        Writer->WriteValue(TEXT("upperTexture"), ReadWadName(Side.UpperTex));
        Writer->WriteValue(TEXT("middleTexture"), ReadWadName(Side.MainTex));
        Writer->WriteValue(TEXT("lowerTexture"), ReadWadName(Side.LowerTex));
    });
    WriteRecords(*Writer, TEXT("sectors"), Map.Sectors, [&](const FWadSector& Sector)
    {
        Writer->WriteValue(TEXT("floorHeight"), Sector.FloorHeight);
        Writer->WriteValue(TEXT("ceilingHeight"), Sector.CeilingHeight);
        Writer->WriteValue(TEXT("floorTexture"), ReadWadName(Sector.FloorTex));
        Writer->WriteValue(TEXT("ceilingTexture"), ReadWadName(Sector.CeilingTex));
        Writer->WriteValue(TEXT("light"), Sector.Light);
        Writer->WriteValue(TEXT("type"), Sector.Type);
        Writer->WriteValue(TEXT("trigger"), Sector.Trigger);
        Writer->WriteValue(TEXT("startsClosed"), Sector.CeilingHeight <= Sector.FloorHeight);
    });
    WriteRecords(*Writer, TEXT("things"), Map.Things, [&](const FWadThing& Thing)
    {
        Writer->WriteValue(TEXT("x"), Thing.X);
        Writer->WriteValue(TEXT("y"), Thing.Y);
        Writer->WriteValue(TEXT("angle"), Thing.Angle);
        Writer->WriteValue(TEXT("type"), Thing.Type);
        Writer->WriteValue(TEXT("flags"), Thing.Flags);
    });
    WriteRecords(*Writer, TEXT("segs"), Map.Segs, [&](const FWadSeg& Seg)
    {
        Writer->WriteValue(TEXT("fromVertexId"), Seg.From);
        Writer->WriteValue(TEXT("toVertexId"), Seg.To);
        Writer->WriteValue(TEXT("angle"), uint16(Seg.Angle));
        Writer->WriteValue(TEXT("lineId"), Seg.Linedef);
        Writer->WriteValue(TEXT("side"), Seg.Side);
        Writer->WriteValue(TEXT("offset"), Seg.Offset);
    });
    WriteRecords(*Writer, TEXT("subsectors"), Map.SubSectors, [&](const FWadSubSector& SubSector)
    {
        Writer->WriteValue(TEXT("firstSegId"), SubSector.FirstSeg);
        Writer->WriteValue(TEXT("segCount"), SubSector.NumSegs);
    });
    WriteRecords(*Writer, TEXT("nodes"), Map.Nodes, [&](const FWadNode& Node)
    {
        Writer->WriteValue(TEXT("x"), Node.X);
        Writer->WriteValue(TEXT("y"), Node.Y);
        Writer->WriteValue(TEXT("dx"), Node.DX);
        Writer->WriteValue(TEXT("dy"), Node.DY);
        Writer->WriteArrayStart(TEXT("children"));
        for (int32 Side = 0; Side < 2; ++Side)
        {
            const uint16 Child = Node.Children[Side];
            Writer->WriteObjectStart();
            Writer->WriteValue(TEXT("isSubsector"), (Child & 0x8000) != 0);
            Writer->WriteValue(TEXT("id"), Child & 0x7fff);
            Writer->WriteValue(TEXT("rawChild"), Child);
            Writer->WriteArrayStart(TEXT("boundsTopBottomLeftRight"));
            for (const int16 Bound : Node.BBox[Side])
            {
                Writer->WriteValue(Bound);
            }
            Writer->WriteArrayEnd();
            Writer->WriteObjectEnd();
        }
        Writer->WriteArrayEnd();
    });

    const TMap<FString, int32> DecodedRecordSizes = {
        {TEXT("VERTEXES"), sizeof(FWadVertex)}, {TEXT("LINEDEFS"), sizeof(FWadLineDef)},
        {TEXT("SIDEDEFS"), sizeof(FWadSideDef)}, {TEXT("SECTORS"), sizeof(FWadSector)},
        {TEXT("THINGS"), sizeof(FWadThing)}, {TEXT("SEGS"), sizeof(FWadSeg)},
        {TEXT("SSECTORS"), sizeof(FWadSubSector)}, {TEXT("NODES"), sizeof(FWadNode)}
    };
    Writer->WriteArrayStart(TEXT("lumps"));
    for (const FWadLumpDirectoryEntry& Entry : Parser.GetSelectedDirectory())
    {
        const FString Name = ReadWadName(Entry.Name);
        Writer->WriteObjectStart();
        Writer->WriteValue(TEXT("name"), Name);
        Writer->WriteValue(TEXT("fileOffset"), Entry.FilePos);
        Writer->WriteValue(TEXT("byteCount"), Entry.Size);
        const TArrayView<const uint8> Bytes = Parser.GetLumpBytes(Entry);
        const int32* RecordSize = DecodedRecordSizes.Find(Name);
        if (RecordSize && Entry.Size % *RecordSize == 0)
        {
            Writer->WriteValue(TEXT("recordCount"), Entry.Size / *RecordSize);
        }
        else if (!Bytes.IsEmpty())
        {
            if (Name == TEXT("BLOCKMAP") && Bytes.Num() >= 8)
            {
                Writer->WriteValue(TEXT("originX"), int16(ReadWord(Bytes, 0)));
                Writer->WriteValue(TEXT("originY"), int16(ReadWord(Bytes, 2)));
                Writer->WriteValue(TEXT("columns"), ReadWord(Bytes, 4));
                Writer->WriteValue(TEXT("rows"), ReadWord(Bytes, 6));
            }
            WriteSupplementalBytes(*Writer, Bytes, Name == TEXT("BLOCKMAP"));
        }
        Writer->WriteObjectEnd();
    }
    Writer->WriteArrayEnd();

    Writer->WriteObjectStart(TEXT("companionFiles"));
    if (FPaths::GetExtension(WadPath).Equals(TEXT("map"), ESearchCase::IgnoreCase))
    {
        for (const TCHAR* Extension : {TEXT("I"), TEXT("GEN"), TEXT("LIT")})
        {
            const FString CompanionPath = FPaths::ChangeExtension(WadPath, Extension);
            if (!FPaths::FileExists(CompanionPath))
            {
                continue;
            }
            Writer->WriteObjectStart(Extension);
            if (FCString::Strcmp(Extension, TEXT("LIT")) == 0)
            {
                TArray<uint8> Bytes;
                if (!ReadWholeFile(CompanionPath, Bytes))
                {
                    return false;
                }
                WriteSupplementalBytes(*Writer, Bytes, true);
            }
            else
            {
                FString Contents;
                if (!FFileHelper::LoadFileToString(Contents, *CompanionPath))
                {
                    UE_LOG(LogDoomWad, Error, TEXT("Failed to read companion file %s"), *CompanionPath);
                    return false;
                }
                Writer->WriteValue(TEXT("text"), Contents);
            }
            Writer->WriteObjectEnd();
        }
    }
    Writer->WriteObjectEnd();
    Writer->WriteObjectEnd();
    if (!Writer->Close())
    {
        return false;
    }
    Json.AppendChar('\n');

    const FString OutputDirectory = FPaths::GetPath(OutputPath);
    if ((!OutputDirectory.IsEmpty() && !IFileManager::Get().MakeDirectory(*OutputDirectory, true))
        || !FFileHelper::SaveStringToFile(Json, *OutputPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
    {
        UE_LOG(LogDoomWad, Error, TEXT("Failed to write map JSON to %s"), *OutputPath);
        return false;
    }
    return true;
}

bool UDoomWadLoader::GetLastWad2DGeometry(TArray<FVector2D>& OutVertices, TArray<FIntPoint>& OutLines)
{
    if (!AA::G_LastStats.bValid || AA::G_LastMap.Vertices.Num() == 0 || AA::G_LastMap.Lines.Num() == 0)
    {
        return false;
    }
    OutVertices.Reset(AA::G_LastMap.Vertices.Num());
    for (const T_3dVertex& V : AA::G_LastMap.Vertices)
    {
        OutVertices.Add(FVector2D((float)V.X, (float)V.Y));
    }
    OutLines.Reset(AA::G_LastMap.Lines.Num());
    for (const T_3dLine& L : AA::G_LastMap.Lines)
    {
        OutLines.Add(FIntPoint((int32)L.From, (int32)L.To));
    }
    return true;
}

bool UDoomWadLoader::GetLastWadWallTopology(TArray<FVector2D>& OutVertices, TArray<FIntPoint>& OutLines, TArray<FIntPoint>& OutLineSectors, TArray<FIntPoint>& OutSectorHeights)
{
    if (!AA::G_LastStats.bValid || AA::G_LastMap.Vertices.Num() == 0 || AA::G_LastMap.Lines.Num() == 0 || AA::G_LastMap.Sectors.Num() == 0)
    {
        return false;
    }

    // Vertices and lines
    OutVertices.Reset(AA::G_LastMap.Vertices.Num());
    for (const T_3dVertex& V : AA::G_LastMap.Vertices)
    {
        OutVertices.Add(FVector2D((float)V.X, (float)V.Y));
    }
    OutLines.Reset(AA::G_LastMap.Lines.Num());
    for (const T_3dLine& L : AA::G_LastMap.Lines)
    {
        OutLines.Add(FIntPoint((int32)L.From, (int32)L.To));
    }

    // Sector heights
    OutSectorHeights.Reset(AA::G_LastMap.Sectors.Num());
    for (const T_3dSector& S : AA::G_LastMap.Sectors)
    {
        OutSectorHeights.Add(FIntPoint((int32)S.FloorHeight, (int32)S.CeilingHeight));
    }

    // For each linedef, map to front/back sector via sidedefs
    OutLineSectors.Reset(AA::G_LastMap.Lines.Num());
    for (const T_3dLine& L : AA::G_LastMap.Lines)
    {
        OutLineSectors.Emplace(AA::G_LastMap.GetSectorForSide(L.Side[0]), AA::G_LastMap.GetSectorForSide(L.Side[1]));
    }
    return true;
}

bool UDoomWadLoader::GetLastWadLineSideIndices(TArray<FIntPoint>& OutLineSideIndices)
{
    if (!AA::G_LastStats.bValid || AA::G_LastMap.Lines.Num() == 0)
    {
        return false;
    }

    OutLineSideIndices.Reset(AA::G_LastMap.Lines.Num());
    for (const T_3dLine& L : AA::G_LastMap.Lines)
    {
        OutLineSideIndices.Add(FIntPoint((int32)L.Side[0], (int32)L.Side[1]));
    }
    return true;
}

bool UDoomWadLoader::GetLastWadSectorTextureNames(int32 SectorIndex, FString& OutFloorTexture, FString& OutCeilingTexture)
{
    if (!AA::G_LastStats.bValid || !AA::G_LastMap.Sectors.IsValidIndex(SectorIndex))
    {
        OutFloorTexture.Reset();
        OutCeilingTexture.Reset();
        return false;
    }

    const T_3dSector& Sector = AA::G_LastMap.Sectors[SectorIndex];
    OutFloorTexture = AA::ReadWadName(Sector.FloorTex);
    OutCeilingTexture = AA::ReadWadName(Sector.CeilingTex);
    return true;
}

bool UDoomWadLoader::GetLastWadSideTextureNames(int32 SideIndex, FString& OutUpperTexture, FString& OutLowerTexture, FString& OutMainTexture, FVector2D& OutTextureOffset)
{
    if (!AA::G_LastStats.bValid || !AA::G_LastMap.Sides.IsValidIndex(SideIndex))
    {
        OutUpperTexture.Reset();
        OutLowerTexture.Reset();
        OutMainTexture.Reset();
        OutTextureOffset = FVector2D::ZeroVector;
        return false;
    }

    GetWadSideTextureNames(AA::G_LastMap.Sides[SideIndex], OutUpperTexture, OutLowerTexture, OutMainTexture, OutTextureOffset);
    return true;
}

void UDoomWadLoader::GetWadSideTextureNames(const FWadSideDef& Side, FString& OutUpperTexture, FString& OutLowerTexture,
    FString& OutMainTexture, FVector2D& OutTextureOffset)
{
    OutUpperTexture = AA::ReadWadName(Side.UpperTex);
    OutLowerTexture = AA::ReadWadName(Side.LowerTex);
    OutMainTexture = AA::ReadWadName(Side.MainTex);
    OutTextureOffset = FVector2D((float)Side.TexXOffset, (float)Side.TexYOffset);
}

static bool BuildSectorBoundaryLoopsInternal(int32 SectorIndex, TArray<TArray<int32>>& OutLoops)
{
    using namespace AA;
    OutLoops.Reset();

    if (!G_LastStats.bValid || SectorIndex < 0 || SectorIndex >= G_LastMap.Sectors.Num())
    {
        return false;
    }

    TArray<TPair<int32, int32>> DirectedEdges;
    DirectedEdges.Reserve(G_LastMap.Lines.Num());

    TMultiMap<int32, int32> OutgoingByVertex;

    for (const T_3dLine& L : G_LastMap.Lines)
    {
        const int32 From = (int32)L.From;
        const int32 To = (int32)L.To;
        if (!G_LastMap.Vertices.IsValidIndex(From) || !G_LastMap.Vertices.IsValidIndex(To) || From == To)
        {
            continue;
        }

        const int32 Side0 = (int32)L.Side[0];
        const int32 Side1 = (int32)L.Side[1];
        const int32 Sector0 = (Side0 >= 0 && Side0 < G_LastMap.Sides.Num()) ? (int32)G_LastMap.Sides[Side0].Sector : -1;
        const int32 Sector1 = (Side1 >= 0 && Side1 < G_LastMap.Sides.Num()) ? (int32)G_LastMap.Sides[Side1].Sector : -1;

        if (Sector0 == SectorIndex && Sector1 != SectorIndex)
        {
            const int32 EdgeIdx = DirectedEdges.Add(TPair<int32, int32>(From, To));
            OutgoingByVertex.Add(From, EdgeIdx);
        }
        else if (Sector1 == SectorIndex && Sector0 != SectorIndex)
        {
            const int32 EdgeIdx = DirectedEdges.Add(TPair<int32, int32>(To, From));
            OutgoingByVertex.Add(To, EdgeIdx);
        }
    }

    if (DirectedEdges.Num() == 0)
    {
        return false;
    }

    TBitArray<> Used(false, DirectedEdges.Num());
    for (int32 StartEdgeIdx = 0; StartEdgeIdx < DirectedEdges.Num(); ++StartEdgeIdx)
    {
        if (Used[StartEdgeIdx])
        {
            continue;
        }

        const int32 StartVertex = DirectedEdges[StartEdgeIdx].Key;
        const int32 NextVertex = DirectedEdges[StartEdgeIdx].Value;
        TArray<int32> Loop;
        Loop.Reserve(DirectedEdges.Num());
        Loop.Add(StartVertex);
        Loop.Add(NextVertex);
        Used[StartEdgeIdx] = true;

        int32 CurrentVertex = NextVertex;
        int32 Guard = 0;
        while (Guard++ < DirectedEdges.Num() + 1)
        {
            if (CurrentVertex == StartVertex)
            {
                break;
            }

            int32 NextEdgeIdx = INDEX_NONE;
            TArray<int32> Candidates;
            OutgoingByVertex.MultiFind(CurrentVertex, Candidates);
            for (const int32 CandidateIdx : Candidates)
            {
                if (!Used[CandidateIdx])
                {
                    NextEdgeIdx = CandidateIdx;
                    break;
                }
            }

            if (NextEdgeIdx == INDEX_NONE)
            {
                break;
            }

            Used[NextEdgeIdx] = true;
            const int32 CandidateVertex = DirectedEdges[NextEdgeIdx].Value;
            if (Loop.Last() != CandidateVertex)
            {
                Loop.Add(CandidateVertex);
            }
            CurrentVertex = CandidateVertex;
        }

        if (Loop.Num() >= 4 && Loop.Last() == Loop[0])
        {
            Loop.Pop(EAllowShrinking::No);
            OutLoops.Add(MoveTemp(Loop));
        }
    }

    return OutLoops.Num() > 0;
}

static bool ChainSubsectorSegsToPolys(const TArray<T_3dSeg>& InSegs, const TArray<T_3dVertex>& AllVerts,
                                      const TArray<T_3dLine>& AllLines, const TArray<T_3dSide>& AllSides,
                                      const TArray<T_3dSector>& AllSectors, int32 WantedSector,
                                      TArray<TArray<int32>>& OutLoops)
{
    OutLoops.Reset();
    if (InSegs.Num() == 0) return false;

    struct FSegInfo { int32 From; int32 To; };
    TArray<FSegInfo> S;
    S.Reserve(InSegs.Num());

    // Determine likely owning sector for this subsector.
    // Primary signal uses the seg-indicated sidedef (SG.Side). Fallback uses either side.
    TMap<int32, int32> PrimarySectorHitCount;
    TMap<int32, int32> AnySectorHitCount;
    auto AddHit = [&](TMap<int32, int32>& HitMap, int32 SectorIdx)
    {
        if (SectorIdx >= 0 && SectorIdx < AllSectors.Num())
        {
            int32& Count = HitMap.FindOrAdd(SectorIdx);
            ++Count;
        }
    };

    for (const auto& SG : InSegs)
    {
        const int32 From = (int32)SG.From;
        const int32 To = (int32)SG.To;
        if (!AllVerts.IsValidIndex(From) || !AllVerts.IsValidIndex(To) || From == To)
        {
            continue;
        }

        S.Add({ From, To });

        const int32 Linedef = (int32)SG.Linedef;
        if (Linedef >= 0 && Linedef < AllLines.Num())
        {
            const auto& L = AllLines[Linedef];

            // Primary ownership candidate from seg side.
            const int32 SegSide = (int32)SG.Side;
            if (SegSide >= 0 && SegSide <= 1)
            {
                const int32 SideRef = (int32)L.Side[SegSide];
                if (SideRef >= 0 && SideRef < AllSides.Num())
                {
                    AddHit(PrimarySectorHitCount, (int32)AllSides[SideRef].Sector);
                }
            }

            // Fallback ownership candidates from either side.
            for (int32 SideIdx = 0; SideIdx < 2; ++SideIdx)
            {
                const int32 SideRef = (int32)L.Side[SideIdx];
                if (SideRef >= 0 && SideRef < AllSides.Num())
                {
                    AddHit(AnySectorHitCount, (int32)AllSides[SideRef].Sector);
                }
            }
        }
    }

    if (S.Num() == 0) return false;

    int32 DominantSector = -1;
    int32 DominantCount = -1;
    auto ChooseDominant = [&](const TMap<int32, int32>& HitMap)
    {
        int32 BestSector = -1;
        int32 BestCount = -1;
        for (const TPair<int32, int32>& KV : HitMap)
        {
            if (KV.Value > BestCount)
            {
                BestSector = KV.Key;
                BestCount = KV.Value;
            }
        }
        DominantSector = BestSector;
        DominantCount = BestCount;
    };

    if (PrimarySectorHitCount.Num() > 0)
    {
        ChooseDominant(PrimarySectorHitCount);
    }
    else if (AnySectorHitCount.Num() > 0)
    {
        ChooseDominant(AnySectorHitCount);
    }

    if (DominantSector < 0 || DominantSector != WantedSector)
    {
        return false;
    }

    TBitArray<> Used(false, S.Num());

    for (int32 StartSegIdx = 0; StartSegIdx < S.Num(); ++StartSegIdx)
    {
        if (Used[StartSegIdx]) continue;

        const int32 StartV = S[StartSegIdx].From;
        int32 Curr = S[StartSegIdx].To;
        TArray<int32> Loop;
        Loop.Reserve(S.Num() + 1);
        Loop.Add(StartV);
        Loop.Add(Curr);
        Used[StartSegIdx] = true;

        int32 Guard = 0;
        while (Guard++ < 100000)
        {
            if (Curr == StartV)
            {
                break;
            }

            int32 NextSegIdx = INDEX_NONE;
            int32 NextV = INDEX_NONE;
            for (int32 i = 0; i < S.Num(); ++i)
            {
                if (Used[i]) continue;
                if (S[i].From == Curr)
                {
                    NextSegIdx = i;
                    NextV = S[i].To;
                    break;
                }
                if (S[i].To == Curr)
                {
                    NextSegIdx = i;
                    NextV = S[i].From;
                    break;
                }
            }

            if (NextSegIdx == INDEX_NONE)
            {
                break;
            }

            Used[NextSegIdx] = true;
            if (Loop.Last() != NextV)
            {
                Loop.Add(NextV);
            }
            Curr = NextV;
        }

        if (Loop.Num() >= 4 && Loop.Last() == Loop[0])
        {
            OutLoops.Add(MoveTemp(Loop));
        }
    }

    // Fallback: if we could not close loops with stitching, preserve ordered seg chain.
    if (OutLoops.Num() == 0 && S.Num() >= 3)
    {
        TArray<int32> Loop;
        Loop.Reserve(S.Num() + 1);
        Loop.Add(S[0].From);
        Loop.Add(S[0].To);
        for (int32 i = 1; i < S.Num(); ++i)
        {
            const FSegInfo& Seg = S[i];
            const int32 Tail = Loop.Last();
            if (Seg.From == Tail) Loop.Add(Seg.To);
            else if (Seg.To == Tail) Loop.Add(Seg.From);
            else Loop.Add(Seg.From);
        }
        if (Loop.Last() != Loop[0])
        {
            const int32 FirstVertex = Loop[0];
            Loop.Add(FirstVertex);
        }
        if (Loop.Num() >= 4) OutLoops.Add(MoveTemp(Loop));
    }

    return OutLoops.Num() > 0;
}

bool UDoomWadLoader::GetLastWadSectorSubsectorPolys(int32 SectorIndex, TArray<int32>& OutPolyOffsets, TArray<int32>& OutPolyCounts, TArray<FVector2D>& OutPolyVerts)
{
    using namespace AA;
    OutPolyOffsets.Reset(); OutPolyCounts.Reset(); OutPolyVerts.Reset();
    if (!G_LastStats.bValid || SectorIndex < 0 || SectorIndex >= G_LastMap.Sectors.Num()) return false;
    if (G_LastMap.Segs.Num() == 0 || G_LastMap.SubSectors.Num() == 0) return false;

    int32 NumSubsectorsVisited = 0;
    int32 NumSubsectorsWithSegs = 0;
    int32 NumSubsectorsMatchedSector = 0;
    int32 NumSubsectorsRejected = 0;
    int32 NumLoopsFromMatchedSubsectors = 0;
    int32 NumPolysAccepted = 0;
    int32 NumPolysRejectedDegenerate = 0;
    int32 NumVertsInAcceptedPolys = 0;

    // Build subsector -> segs mapping
    TArray<TArray<int32>> SubsectorSegIndices; SubsectorSegIndices.SetNum(G_LastMap.SubSectors.Num());
    for (int32 ssi = 0; ssi < G_LastMap.SubSectors.Num(); ++ssi)
    {
        const auto& SS = G_LastMap.SubSectors[ssi];
        for (int32 k=0;k<SS.NumSegs; ++k)
        {
            const int32 SegIndex = (int32)SS.FirstSeg + k;
            if (G_LastMap.Segs.IsValidIndex(SegIndex))
            {
                SubsectorSegIndices[ssi].Add(SegIndex);
            }
        }
    }

    // For each subsector, if any seg points into SectorIndex (via linedef+side), collect a poly by chaining segs
    for (int32 ssi=0; ssi<SubsectorSegIndices.Num(); ++ssi)
    {
        ++NumSubsectorsVisited;
        TArray<T_3dSeg> SegsForSS;
        for (int32 segi : SubsectorSegIndices[ssi]) SegsForSS.Add(G_LastMap.Segs[segi]);
        if (SegsForSS.Num() > 0) ++NumSubsectorsWithSegs;

        TArray<TArray<int32>> Loops;
        if (!ChainSubsectorSegsToPolys(SegsForSS, G_LastMap.Vertices, G_LastMap.Lines, G_LastMap.Sides, G_LastMap.Sectors, SectorIndex, Loops))
        {
            ++NumSubsectorsRejected;
            continue;
        }
        ++NumSubsectorsMatchedSector;
        NumLoopsFromMatchedSubsectors += Loops.Num();

        for (const TArray<int32>& Loop : Loops)
        {
            const int32 Start = OutPolyVerts.Num();
            int32 Count = 0;
            // If last equals first, drop the last
            const int32 End = (Loop.Last() == Loop[0]) ? Loop.Num()-1 : Loop.Num();
            for (int32 i=0;i<End;++i)
            {
                const int32 vi = Loop[i];
                if (!G_LastMap.Vertices.IsValidIndex(vi)) continue;
                const auto& V = G_LastMap.Vertices[vi];
                OutPolyVerts.Add(FVector2D((float)V.X, (float)V.Y));
                ++Count;
            }
            if (Count >= 3)
            {
                OutPolyOffsets.Add(Start);
                OutPolyCounts.Add(Count);
                ++NumPolysAccepted;
                NumVertsInAcceptedPolys += Count;
            }
            else
            {
                // discard unused verts
                OutPolyVerts.SetNum(Start);
                ++NumPolysRejectedDegenerate;
            }
        }
    }

    UE_LOG(LogDoomWad, Display,
        TEXT("Sector %d subsector stats: totalSS=%d withSegs=%d matched=%d rejected=%d loops=%d acceptedPolys=%d rejectedPolys=%d acceptedVerts=%d"),
        SectorIndex,
        NumSubsectorsVisited,
        NumSubsectorsWithSegs,
        NumSubsectorsMatchedSector,
        NumSubsectorsRejected,
        NumLoopsFromMatchedSubsectors,
        NumPolysAccepted,
        NumPolysRejectedDegenerate,
        NumVertsInAcceptedPolys);

    return OutPolyOffsets.Num() > 0;
}

bool UDoomWadLoader::GetLastWadSectorBoundaryLoops(int32 SectorIndex, TArray<int32>& OutLoopOffsets, TArray<int32>& OutLoopCounts, TArray<FVector2D>& OutLoopVerts)
{
    using namespace AA;
    OutLoopOffsets.Reset();
    OutLoopCounts.Reset();
    OutLoopVerts.Reset();

    TArray<TArray<int32>> Loops;
    if (!BuildSectorBoundaryLoopsInternal(SectorIndex, Loops))
    {
        return false;
    }

    for (const TArray<int32>& Loop : Loops)
    {
        const int32 Start = OutLoopVerts.Num();
        int32 Count = 0;
        for (const int32 VertexIndex : Loop)
        {
            if (!G_LastMap.Vertices.IsValidIndex(VertexIndex))
            {
                continue;
            }
            const T_3dVertex& V = G_LastMap.Vertices[VertexIndex];
            OutLoopVerts.Add(FVector2D((float)V.X, (float)V.Y));
            ++Count;
        }

        if (Count >= 3)
        {
            OutLoopOffsets.Add(Start);
            OutLoopCounts.Add(Count);
        }
        else
        {
            OutLoopVerts.SetNum(Start);
        }
    }

    return OutLoopOffsets.Num() > 0;
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

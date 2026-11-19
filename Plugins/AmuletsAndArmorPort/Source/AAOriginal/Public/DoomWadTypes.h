#pragma once
#include <cstdint>
#include "CoreMinimal.h"

/* Minimal self-contained DOOM WAD structures (little-endian, packed). */
#pragma pack(push,1)
struct FWadHeader
{
    char Signature[4];      // "IWAD" or "PWAD"
    int32 NumLumps;         // Number of directory entries
    int32 DirOffset;        // File offset to directory
};

struct FWadLumpDirectoryEntry
{
    int32 FilePos;          // Offset to lump data
    int32 Size;             // Lump size in bytes
    char Name[8];           // Lump name (not null terminated if 8 chars)
};

struct FWadVertex { int16 X; int16 Y; };
struct FWadThing
{
    int16 X;
    int16 Y;
    uint16 Angle;
    int16 Type;
    uint16 Flags;
};

struct FWadLineDef
{
    int16 From;             // start vertex index
    int16 To;               // end vertex index
    uint16 Flags;           // line flags
    uint16 Special;         // special type / action
    uint16 Tag;             // id / tag number
    int16 Side[2];          // front/back sidedef indices (-1 if none)
};

struct FWadSideDef
{
    int16 TexXOffset;       // texture X offset
    int16 TexYOffset;       // texture Y offset
    char UpperTex[8];       // upper texture name
    char LowerTex[8];       // lower texture name
    char MainTex[8];        // middle texture name
    int16 Sector;           // sector number this sidedef references
};

struct FWadSector
{
    int16 FloorHeight;
    int16 CeilingHeight;
    char FloorTex[8];
    char CeilingTex[8];
    uint16 Light;           // light level (0-255 typical), stored as 0-255
    uint16 Type;            // special type
    uint16 Trigger;         // tag / trigger
};

// BSP-related lumps
struct FWadSeg
{
    int16 From;     // start vertex index
    int16 To;       // end vertex index
    int16 Angle;    // angle
    int16 Linedef;  // linedef index
    int16 Side;     // 0 = front, 1 = back
    int16 Offset;   // offset along linedef
};

struct FWadSubSector
{
    int16 NumSegs;   // number of segs
    int16 FirstSeg;  // first seg index
};

struct FWadNode
{
    int16 X; int16 Y; int16 DX; int16 DY;   // partition line
    int16 BBox[2][4];                       // 2 child bounding boxes (top,bottom,left,right)
    uint16 Children[2];                     // child indices; high bit (0x8000) set = subsector
};
#pragma pack(pop)

// Legacy compatible aliases so existing parsing code remains unchanged if it used old names.
using T_wadHeader      = FWadHeader;
using T_directoryEntry = FWadLumpDirectoryEntry;
using T_3dVertex       = FWadVertex;
using T_3dLine         = FWadLineDef;
using T_3dSide         = FWadSideDef;
using T_3dSector       = FWadSector;
using T_3dSeg          = FWadSeg;
using T_3dSubSector    = FWadSubSector;
using T_3dNode         = FWadNode;

// Compile-time size checks (will fail build if mismatched unexpectedly)
static_assert(sizeof(FWadHeader) == 12, "FWadHeader size mismatch");
static_assert(sizeof(FWadLumpDirectoryEntry) == 16, "FWadLumpDirectoryEntry size mismatch");
static_assert(sizeof(FWadVertex) == 4, "FWadVertex size mismatch");
static_assert(sizeof(FWadThing) == 10, "FWadThing size mismatch");
static_assert(sizeof(FWadLineDef) == 14, "FWadLineDef size mismatch");
static_assert(sizeof(FWadSideDef) == 30, "FWadSideDef size mismatch");
static_assert(sizeof(FWadSector) == 26, "FWadSector size mismatch");
static_assert(sizeof(FWadSeg) == 12, "FWadSeg size mismatch");
static_assert(sizeof(FWadSubSector) == 4, "FWadSubSector size mismatch");
static_assert(sizeof(FWadNode) == 28, "FWadNode size mismatch");

// Source-order arrays: a record's zero-based index is its stable ID within this map.
// Positions, heights, offsets, and flags retain the original values.
struct FDoomWadMapData
{
    FString Marker;
    TArray<FWadVertex> Vertices;
    TArray<FWadLineDef> Lines;
    TArray<FWadSideDef> Sides;
    TArray<FWadSector> Sectors;
    TArray<FWadThing> Things;
    TArray<FWadSeg> Segs;
    TArray<FWadSubSector> SubSectors;
    TArray<FWadNode> Nodes;

    int32 GetSectorForSide(int32 SideIndex) const
    {
        if (Sides.IsValidIndex(SideIndex) && Sectors.IsValidIndex(Sides[SideIndex].Sector))
        {
            return Sides[SideIndex].Sector;
        }
        return INDEX_NONE;
    }
};

// One sector in source XY units. Triangle indices are counterclockwise triples.
struct FDoomWadSectorFootprint
{
    TArray<FVector2D> Vertices;
    TArray<FIntPoint> BoundaryEdges;
    TArray<int32> Triangles;
};

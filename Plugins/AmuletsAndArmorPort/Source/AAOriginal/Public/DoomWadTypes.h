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
#pragma pack(pop)

// Legacy compatible aliases so existing parsing code remains unchanged if it used old names.
using T_wadHeader      = FWadHeader;
using T_directoryEntry = FWadLumpDirectoryEntry;
using T_3dVertex       = FWadVertex;
using T_3dLine         = FWadLineDef;
using T_3dSide         = FWadSideDef;
using T_3dSector       = FWadSector;

// Compile-time size checks (will fail build if mismatched unexpectedly)
static_assert(sizeof(FWadHeader) == 12, "FWadHeader size mismatch");
static_assert(sizeof(FWadLumpDirectoryEntry) == 16, "FWadLumpDirectoryEntry size mismatch");
static_assert(sizeof(FWadVertex) == 4, "FWadVertex size mismatch");
static_assert(sizeof(FWadLineDef) == 14, "FWadLineDef size mismatch");
static_assert(sizeof(FWadSideDef) == 30, "FWadSideDef size mismatch");
static_assert(sizeof(FWadSector) == 26, "FWadSector size mismatch");

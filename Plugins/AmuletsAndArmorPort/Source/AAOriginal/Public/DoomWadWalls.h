#pragma once

#include "DoomWadTypes.h"
#include "DoomWadWalls.generated.h"

UENUM(BlueprintType)
enum class EWadWallBand : uint8 { Lower, Middle, Upper };
enum class EWadWallBlend : uint8 { Opaque, Masked, Translucent };

struct FWadSectorHeightRange
{
    double Floor = 0.0;
    double Ceiling = 0.0;
};

struct FWadWallSurface
{
    int32 SideIndex = INDEX_NONE;
    int32 SectorIndex = INDEX_NONE;
    double Bottom = 0.0;
    double Top = 0.0;
    double VOrigin = 0.0;
    EWadWallBand Band = EWadWallBand::Middle;
    EWadWallBlend Blend = EWadWallBlend::Opaque;
    bool bBack = false;
};

struct FWadWallCollision
{
    double Bottom = 0.0;
    double Top = 0.0;
    bool bCreatureOnly = false;
};

// Heights and endpoints remain in source map units. The opening describes the shared
// vertical range even when it contains a pane or an invisible collision barrier.
struct FWadWallLine
{
    FVector2D From = FVector2D::ZeroVector;
    FVector2D To = FVector2D::ZeroVector;
    FIntPoint Sectors = FIntPoint(INDEX_NONE, INDEX_NONE);
    double OpeningBottom = 0.0;
    double OpeningTop = 0.0;
    TArray<FWadWallSurface, TInlineAllocator<6>> Surfaces;
    TArray<FWadWallCollision, TInlineAllocator<3>> Collision;

    bool HasOpening() const { return OpeningTop > OpeningBottom; }
};

AAORIGINAL_API bool BuildWadWallLine(const FDoomWadMapData& Map, int32 LineIndex, FWadWallLine& Out,
    TArrayView<const FWadSectorHeightRange> Heights = {});

#include "DoomWadWalls.h"

namespace
{
    constexpr uint16 Impassable = 0x0001;
    constexpr uint16 CreatureImpassable = 0x0002;
    constexpr uint16 TwoSided = 0x0004;
    constexpr uint16 Translucent = 0x0020;

    bool HasTexture(const char (&Name)[8])
    {
        for (char Character : Name)
        {
            if (Character == '\0' || Character == '-')
            {
                return false;
            }
            if (Character != ' ')
            {
                return true;
            }
        }
        return false;
    }
}

bool BuildWadWallLine(const FDoomWadMapData& Map, int32 LineIndex, FWadWallLine& Out,
    TArrayView<const FWadSectorHeightRange> Heights)
{
    Out = FWadWallLine();
    if (!Map.Lines.IsValidIndex(LineIndex))
    {
        return false;
    }
    const FWadLineDef& Line = Map.Lines[LineIndex];
    if (!Map.Vertices.IsValidIndex(Line.From) || !Map.Vertices.IsValidIndex(Line.To))
    {
        return false;
    }
    const FWadVertex& A = Map.Vertices[Line.From];
    const FWadVertex& B = Map.Vertices[Line.To];
    Out.From = FVector2D(A.X, A.Y);
    Out.To = FVector2D(B.X, B.Y);
    if (Out.From == Out.To)
    {
        return false;
    }

    Out.Sectors = FIntPoint(Map.GetSectorForSide(Line.Side[0]), Map.GetSectorForSide(Line.Side[1]));
    const auto HeightForSector = [&](int32 Index) -> FWadSectorHeightRange
    {
        if (Heights.IsValidIndex(Index))
        {
            return Heights[Index];
        }
        const FWadSector& Sector = Map.Sectors[Index];
        return {double(Sector.FloorHeight), double(Sector.CeilingHeight)};
    };
    const bool bTwoSided = (Line.Flags & TwoSided) != 0
        && Out.Sectors.X != INDEX_NONE && Out.Sectors.Y != INDEX_NONE;
    if (bTwoSided)
    {
        const FWadSectorHeightRange Front = HeightForSector(Out.Sectors.X);
        const FWadSectorHeightRange Back = HeightForSector(Out.Sectors.Y);
        Out.OpeningBottom = FMath::Max(Front.Floor, Back.Floor);
        Out.OpeningTop = FMath::Min(Front.Ceiling, Back.Ceiling);
    }

    auto AddCollision = [&](double Bottom, double Top, bool bCreatureOnly = false)
    {
        if (Bottom < Top)
        {
            Out.Collision.Add(FWadWallCollision{Bottom, Top, bCreatureOnly});
        }
    };

    for (int32 Side = 0; Side < 2; ++Side)
    {
        const int32 SectorIndex = Out.Sectors[Side];
        if (SectorIndex == INDEX_NONE)
        {
            continue;
        }
        const FWadSectorHeightRange Sector = HeightForSector(SectorIndex);
        const double Bottom = Sector.Floor;
        const double Top = Sector.Ceiling;
        if (Bottom >= Top)
        {
            continue;
        }

        const int32 SideIndex = Line.Side[Side];
        const FWadSideDef& SideDef = Map.Sides[SideIndex];
        const auto AddSurface = [&](double Low, double High, EWadWallBand Band, EWadWallBlend Blend, const char (&Name)[8], double VOrigin = 0.0)
        {
            if (Low < High && HasTexture(Name))
            {
                Out.Surfaces.Add(FWadWallSurface{SideIndex, SectorIndex, Low, High, VOrigin, Band, Blend, Side == 1});
            }
        };

        if (!bTwoSided)
        {
            AddSurface(Bottom, Top, EWadWallBand::Middle, EWadWallBlend::Opaque, SideDef.MainTex);
            AddCollision(Bottom, Top);
            continue;
        }

        const FWadSectorHeightRange Neighbor = HeightForSector(Out.Sectors[1 - Side]);
        const double LowerTop = FMath::Clamp(Neighbor.Floor, Bottom, Top);
        const double UpperBottom = FMath::Clamp(Neighbor.Ceiling, LowerTop, Top);
        AddSurface(Bottom, LowerTop, EWadWallBand::Lower, EWadWallBlend::Opaque, SideDef.LowerTex, Neighbor.Floor);
        AddSurface(UpperBottom, Top, EWadWallBand::Upper, EWadWallBlend::Opaque, SideDef.UpperTex, Neighbor.Ceiling);
        AddSurface(Out.OpeningBottom, Out.OpeningTop, EWadWallBand::Middle,
            (Line.Flags & Translucent) != 0 ? EWadWallBlend::Translucent : EWadWallBlend::Masked, SideDef.MainTex);

        // Each step or ceiling lip is solid even if its source texture is blank.
        AddCollision(Bottom, LowerTop);
        AddCollision(UpperBottom, Top);
    }

    if (bTwoSided && (Line.Flags & (Impassable | CreatureImpassable)) != 0)
    {
        AddCollision(Out.OpeningBottom, Out.OpeningTop, (Line.Flags & Impassable) == 0);
    }
    return Out.Sectors.X != INDEX_NONE || Out.Sectors.Y != INDEX_NONE;
}

#include "NativeWadLevel.h"

namespace
{
    constexpr int32 FullWeight = 256;
    constexpr int32 LightingPasses = 2;

    template <typename TRead>
    int32 WeightedNeighborLight(const TArray<int32>& Neighbors, TRead Read, int32 Excluded = INDEX_NONE)
    {
        int32 RemainingNeighbors = Neighbors.Num() - int32(Neighbors.Contains(Excluded));
        int32 RemainingPercent = 100;
        int32 Total = 0;
        for (const int32 Neighbor : Neighbors)
        {
            if (Neighbor != Excluded)
            {
                const int32 Percent = RemainingPercent / RemainingNeighbors--;
                RemainingPercent -= Percent;
                Total += Read(Neighbor) * ((Percent * FullWeight) / 100);
            }
        }
        return (Total / FullWeight);
    }
}

void ANativeWadLevel::BeginPlay()
{
    Super::BeginPlay();
    RefreshSectorLights();
}

int32 ANativeWadLevel::GetSectorLightLevel(int32 Sector) const
{
    if (!Sectors.IsValidIndex(Sector))
    {
        return INDEX_NONE;
    }
    return AppliedLights.IsValidIndex(Sector) && AppliedLights[Sector] != INDEX_NONE ? AppliedLights[Sector] : Sectors[Sector].Light;
}

void ANativeWadLevel::SetOutsideLightLevel(int32 Light)
{
    OutsideLight = FMath::Clamp(Light, 0, 255);
    RefreshSectorLights();
}

void ANativeWadLevel::ClearSectorLightOverride(int32 Sector)
{
    if (LightOverrides.Remove(Sector))
    {
        RefreshSectorLights();
    }
}

void ANativeWadLevel::RefreshSectorLights()
{
    InitializeTopology();

    // Start each update with the imported levels so the same door position and outside value
    // reproduce the same result regardless of frame rate or the door's prior history.
    TArray<int32> Current;
    Current.Reserve(Sectors.Num());
    for (const FAAWadNativeSector& Entry : Sectors)
    {
        Current.Add(FMath::Clamp(Entry.Light, 0, 255));
    }
    if (bUseSectorLightingRules)
    {
        const int32 Sky = FMath::Clamp(OutsideLight, 0, 255);
        TArray<int32> DoorWeights;
        DoorWeights.Init(FullWeight, Sectors.Num());
        for (const FAAWadNativeDoor& Door : Doors)
        {
            if (Heights.IsValidIndex(Door.Sector) && Door.OpenZ > Door.ClosedZ)
            {
                const double Open = (FMath::FloorToDouble(Heights[Door.Sector].Ceiling) - Door.ClosedZ) / (Door.OpenZ - Door.ClosedZ);
                DoorWeights[Door.Sector] = int32(FMath::Clamp(Open, 0.0, 1.0) * FullWeight);
            }
        }
        TArray<int32> Next = Current;
        for (int32 Pass = 0; Pass < LightingPasses; ++Pass)
        {
            const auto ReadLight = [&](int32 Sector) { return Current[Sector]; };
            for (int32 Sector = 0; Sector < Sectors.Num(); ++Sector)
            {
                if (Sectors[Sector].bOutside)
                {
                    Next[Sector] = Sky;
                    continue;
                }
                if (!LightingNeighbors[Sector].IsEmpty())
                {
                    const int32 AcrossDoors = WeightedNeighborLight(LightingNeighbors[Sector], [&](int32 Neighbor)
                    {
                        const TArray<int32>& Through = LightingNeighbors[Neighbor];
                        return DoorBySector.Contains(Neighbor) && Through.Num() > 1
                            ? (WeightedNeighborLight(Through, ReadLight, Sector) * DoorWeights[Neighbor]) / FullWeight
                            : Current[Neighbor];
                    });
                    Next[Sector] = (AcrossDoors * DoorWeights[Sector]) / FullWeight;
                }
            }
            Swap(Current, Next);
        }
    }
    for (int32 Sector = 0; Sector < Current.Num(); ++Sector)
    {
        const int32* Override = LightOverrides.Find(Sector);
        ApplySectorLight(Sector, Override ? *Override : Current[Sector]);
    }
}

#pragma once

#include "CoreMinimal.h"
#include "DoomWadWalls.h"

// Original OVERHEAD.C indexed page. State belongs to a character, never a viewport/global.
class AAORIGINAL_API FWadMagicMap
{
public:
    enum EFeature : uint8 { AllWalls=1, Objects=2, Creatures=4, Rotate=8, Transparent=16, Perspective=32, Translucent=64 };
    static constexpr int32 Size = 75;
    static constexpr int32 PlayerRadius = 20; // CLIENT.C type510 -> HEAD002.INF, independent of native collision capsule.
    struct FLine { FWadWallLine Wall; uint16 Flags=0; };
    struct FMarker { FVector2D Position; double Radius=0; bool bCreature=false; bool bInvisible=false; bool bExtraData=false; };
    bool bOn=false;
    uint8 Features=Rotate|Translucent;
    int32 Tier=0;
    int32 Position=1; // OVERHEAD_POSITION_UPPER_RIGHT
    int32 Zoom=0x2000;
    void StartOwner(const FString& Owner, int32 Power);
    void SynchronizeOwners(const TMap<FString,int32>& Active);
    bool HasOwners() const { return !Owners.IsEmpty(); }
    int32 OwnerCount() const { return Owners.Num(); }
    bool ToggleDisplay();
    void Command(int32 RowKey);
    bool IsSeen(const FString& Map, int32 Line) const;
    void Observe(const FString& Map, int32 Line);
    void BindTopology(const FString& Map, int32 LineCount, const FString& Hash);
    static bool Eligible(uint16 Flags, bool bSeen, uint8 Features);
    static uint8 LineColor(uint16 Flags) { return Flags & 0x0041 ? 31 : 47; }
    FIntPoint Location(FIntRect Opening) const;
    FIntPoint Project(FVector2D World, FVector2D Center, double Yaw) const;
    TArray<uint8> Rasterize(const FString& Map, TConstArrayView<FLine> Lines,
        TConstArrayView<FMarker> Markers, FVector2D Center, double Yaw, double PlayerMarkerRadius) const;
    // Current textured bands only. Opaque bands occlude; masked/translucent bands do not.
    static bool CanObserve(TConstArrayView<FLine> Lines, int32 Target, FVector Eye, FRotator View,
        double HorizontalFov, double Aspect, int32 Sample,
        FBox2D ScreenCrop = FBox2D(FVector2D(0,0),FVector2D(1,1)));
    FString Encode() const;
    bool Decode(const FString& Payload);
private:
    TMap<FString,int32> Owners;
    TMap<FString,TSet<int32>> Seen;
    struct FTopology { int32 Count=0; FString Hash; };
    TMap<FString,FTopology> Topology;
    static void Line(TArray<uint8>& Page, FIntPoint From, FIntPoint To, uint8 Color);
};

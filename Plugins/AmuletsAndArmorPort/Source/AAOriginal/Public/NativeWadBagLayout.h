#pragma once
#include "CoreMinimal.h"

// Spatial location is separate from CarriedItems ownership and its public integer indices.
struct FWadBagItem
{
    int32 Type=0;
    FIntPoint Bitmap=FIntPoint(17,14);
    int32 StackLimit=1;
    bool bOffPage=false;
};
struct FWadBagPlacement
{
    int32 Page=INDEX_NONE,X=INDEX_NONE,Y=INDEX_NONE,Leader=INDEX_NONE;
    int32 Order=INDEX_NONE; // Source linked-record order; independent of promoted native item index.
};

class AAORIGINAL_API FWadBagLayout
{
public:
    static constexpr int32 Columns=6;
    static constexpr int32 Rows=8;
    static constexpr int32 CellWidth=17;
    static constexpr int32 CellHeight=14;
    static constexpr int32 SourcePages=5;
    static constexpr int32 MaxSlots=65536;
    static constexpr int32 MaxPages=1024;
    static FIntPoint Footprint(FIntPoint Bitmap);
    static FIntPoint BitmapOffset(FIntPoint Bitmap);
    static FIntPoint Anchor(FVector2D Point,FVector2D Origin,FIntPoint Bitmap);
    const TArray<FWadBagPlacement>& GetPlacements() const { return Entries; }
    int32 Num() const { return Entries.Num(); }
    int32 PageCount() const { return Pages; }
    bool HasLegacyOverflow() const { return bLegacyOverflow; }
    int32 LeaderOf(int32 Slot) const;
    int32 StackCount(int32 Leader) const;
    TArray<int32> LeadersInOrder() const;
    int32 At(int32 Page,int32 X,int32 Y,TConstArrayView<FWadBagItem> Items) const;
    bool Place(int32 Slot,int32 Page,FIntPoint GridAnchor,TConstArrayView<FWadBagItem> Items);
    bool AutoPlace(int32 Slot,TConstArrayView<FWadBagItem> Items,bool bAllowOverflow=false,bool bRowMajor=false);
    void Detach(int32 Slot);
    void RemoveAt(int32 Slot);
    bool AddSlot();
    bool Initialize(TConstArrayView<FWadBagItem> Items,bool bAllowLegacyOverflow,bool bRowMajor=false);
    bool Validate(TConstArrayView<FWadBagItem> Items) const;
    FString Encode(TConstArrayView<FWadBagItem> Items) const;
    bool Decode(const FString& Payload,TConstArrayView<FWadBagItem> Items);
    void Reset(){Entries.Reset();Pages=SourcePages;bLegacyOverflow=false;NextOrder=0;}
private:
    void TrimOverflow();
    int32 AllocateOrder();
    TArray<FWadBagPlacement> Entries;
    int32 Pages=SourcePages;
    bool bLegacyOverflow=false;
    int32 NextOrder=0;
};

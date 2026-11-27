#pragma once
#include "NativeWadBagLayout.h"

struct FWadStockItem
{
    int32 Type=0,Category=13;
    FIntPoint Bitmap=FIntPoint(17,14);
};
struct FWadStockPlacement
{
    FWadStockItem Item;
    int32 Page=0,X=0,Y=0;
    FIntPoint Footprint=FIntPoint(1,1);
};
// Source STORE/BANK stock is a separate spatial inventory; it never owns player indices.
class AAORIGINAL_API FWadStockLayout
{
public:
    bool Build(TConstArrayView<FWadStockItem> Items,int32 GridRows);
    int32 PageCount() const{return Pages;}
    const TArray<FWadStockPlacement>& GetCells() const{return Cells;}
    int32 At(int32 Page,FIntPoint Cell) const;
    int32 CategoryAt(int32 Page) const;
private:
    TArray<FWadStockPlacement> Cells;
    int32 Pages=1;
};

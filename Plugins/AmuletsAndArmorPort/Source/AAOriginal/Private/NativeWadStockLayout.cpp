#include "NativeWadStockLayout.h"

bool FWadStockLayout::Build(TConstArrayView<FWadStockItem> Items,int32 GridRows)
{
    if(GridRows<1||GridRows>8||Items.Num()>FWadBagLayout::MaxSlots){return false;}
    TArray<FWadStockItem> Sorted(Items);Sorted.StableSort([](const auto& A,const auto& B){return A.Category<B.Category;});
    FWadStockLayout Built;TArray<bool> Occupied;Occupied.Init(false,6*GridRows);
    int32 X=0,Y=0,Page=0,Category=Sorted.IsEmpty()?13:Sorted[0].Category;
    const auto NextPage=[&]{++Page;X=Y=0;for(int32 I=0;I<Occupied.Num();++I){Occupied[I]=false;}};
    for(const auto& Item:Sorted)
    {
        const auto F=FWadBagLayout::Footprint(Item.Bitmap);
        if(Item.Type<=0||Item.Type>MAX_uint16||Item.Category<0||Item.Category>13||F.X>6||F.Y>GridRows){return false;}
        if(Item.Category>Category){Category=Item.Category;NextPage();}
        for(;;)
        {
            bool bFree=X+F.X<=6&&Y+F.Y<=GridRows;
            if(bFree)
            {for(int32 J=Y;J<Y+F.Y;++J)for(int32 I=X;I<X+F.X;++I){if(Occupied[J*6+I]){bFree=false;}}}
            if(bFree)
            {
                Built.Cells.Add({Item,Page,X,Y,F});
                for(int32 J=Y;J<Y+F.Y;++J)for(int32 I=X;I<X+F.X;++I){Occupied[J*6+I]=true;}
                X+=F.X;
            }
            else{++X;}
            // Preserve InventoryReorder's post-placement row/page advance, even
            // when a completely filled category leaves an empty trailing page.
            if(X>=6){X=0;if(++Y>=GridRows){NextPage();}}
            if(bFree){break;}
        }
    }
    Built.Pages=Page+1;*this=MoveTemp(Built);return true;
}
int32 FWadStockLayout::At(int32 Page,FIntPoint Point) const
{
    for(const auto& Cell:Cells)
    {if(Cell.Page==Page&&Point.X>=Cell.X&&Point.X<Cell.X+Cell.Footprint.X&&Point.Y>=Cell.Y&&Point.Y<Cell.Y+Cell.Footprint.Y){return Cell.Item.Type;}}
    return 0;
}
int32 FWadStockLayout::CategoryAt(int32 Page) const
{for(const auto& Cell:Cells){if(Cell.Page==Page){return Cell.Item.Category;}}return INDEX_NONE;}

#pragma once

#include "NativeWadHudData.h"

namespace NativeWadInventoryView
{
    inline constexpr int32 Columns = 6;
    inline constexpr int32 SlotsPerPage = Columns * 8;
    inline const FBox2D Bag({213, 16}, {315, 128});
    inline const FBox2D Ready({94, 156}, {130, 184});

    inline FBox2D SlotBox(int32 Slot)
    {
        if (Slot == INDEX_NONE)
        {
            return Ready;
        }
        const int32 Cell = Slot % SlotsPerPage;
        const FVector2D Corner = Bag.Min + FVector2D((Cell % Columns) * 17, (Cell / Columns) * 14);
        return {Corner, Corner + FVector2D(17, 14)};
    }

    struct FCell
    {
        int32 FirstSlot;
        int32 Count = 1;
    };

    // Saved inventories hold individual objects. Stack them only for display and hit testing.
    struct FView
    {
        FView(const ANativeWadPlayer& Player, const UNativeWadHudData& Data, bool bBagOnly = false, int32 Held = INDEX_NONE)
        {
            const TArray<int32>& Items = Player.GetCarriedItems();
            CellBySlot.Init(INDEX_NONE, Items.Num());
            TMap<int32, int32> LastStackByType;
            for (int32 Slot = 0; Slot < Items.Num(); ++Slot)
            {
                if (bBagOnly && (Slot == Held || Slot == Player.GetReadySlot() || Player.IsWornInventorySlot(Slot)))
                {
                    continue;
                }
                const int32 Type = Items[Slot];
                const FWadInventoryDetails* Details = Data.ItemDetails.Find(Type);
                const int32 Limit = Details ? Details->StackLimit : 1;
                const int32* Previous = LastStackByType.Find(Type);
                if (Previous && Cells[*Previous].Count < Limit)
                {
                    ++Cells[*Previous].Count;
                    CellBySlot[Slot] = *Previous;
                }
                else
                {
                    const int32 Cell = Cells.Add({Slot});
                    CellBySlot[Slot] = Cell;
                    if (Limit > 1)
                    {
                        LastStackByType.Add(Type, Cell);
                    }
                }
            }
        }

        int32 PageCount() const { return FMath::Max(1, FMath::DivideAndRoundUp(Cells.Num(), SlotsPerPage)); }
        int32 CellForSlot(int32 Slot) const { return CellBySlot.IsValidIndex(Slot) ? CellBySlot[Slot] : INDEX_NONE; }

        TArray<FCell> Cells;
        TArray<int32> CellBySlot;
    };
}

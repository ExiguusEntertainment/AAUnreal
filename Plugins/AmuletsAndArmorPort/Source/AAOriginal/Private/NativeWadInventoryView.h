#pragma once

#include "NativeWadHudData.h"
#include "NativeWadPlayer.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Texture2D.h"
#include "GameFramework/PlayerController.h"
#include "TextureResource.h"

namespace NativeWadInventoryView
{
    inline constexpr int32 Columns = 6;
    inline constexpr int32 SlotsPerPage = Columns * 8;
    inline const FBox2D Bag({213, 16}, {315, 128});
    inline const FBox2D Ready({94, 156}, {130, 184});
    inline const FBox2D ArmorBoxes[] = {
        {{242, 19}, {259, 30}}, {{253, 28}, {272, 54}}, {{230, 28}, {245, 65}},
        {{241, 50}, {259, 83}}, {{244, 29}, {253, 51}}, {{258, 47}, {268, 62}},
    };
    // INVENTOR.C's neck and four ring rectangles on the original Equipment form.
    inline const FBox2D JewelryBoxes[] = {
        {{291, 20}, {307, 33}}, {{294, 36}, {305, 45}}, {{294, 48}, {305, 57}},
        {{294, 60}, {305, 69}}, {{294, 72}, {305, 81}},
    };

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
            bNeedsPlacementCell = bBagOnly && Items.IsValidIndex(Held);
            CellBySlot.Init(INDEX_NONE, Items.Num());
            TMap<int32, int32> LastStackByType;
            for (int32 Slot = 0; Slot < Items.Num(); ++Slot)
            {
                if (bBagOnly && (Slot == Held || Slot == Player.GetReadySlot() || Player.IsWornInventorySlot(Slot)))
                {
                    continue;
                }
                const int32 Type = Items[Slot];
                if (NativeWadRules::IsRuneItem(Type)) { continue; }
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

        int32 PageCount() const { return FMath::Max(1, FMath::DivideAndRoundUp(Cells.Num() + int32(bNeedsPlacementCell), SlotsPerPage)); }
        int32 CellForSlot(int32 Slot) const { return CellBySlot.IsValidIndex(Slot) ? CellBySlot[Slot] : INDEX_NONE; }

        // A held object needs one real empty bag target, including when all earlier pages are full.
        bool bNeedsPlacementCell = false;
        TArray<FCell> Cells;
        TArray<int32> CellBySlot;
    };
    inline void DrawMouseHand(UCanvas& Canvas, const ANativeWadPlayer& Player,
        APlayerController& Controller, const UNativeWadHudData& Data, FVector2D Origin, double Scale)
    {
        const UTexture2D* Image = Data.Items.FindRef(Player.GetMouseHandItem());
        const bool bShow = Image && !Player.IsMouseLook() && !Player.IsDefeated() && Scale > 0.0;
        Controller.CurrentMouseCursor = bShow ? EMouseCursor::None : EMouseCursor::Default;
        FVector2D Cursor;
        if (!bShow || !Controller.GetMousePosition(Cursor.X, Cursor.Y)) { return; }
        const FVector2D Local = (Cursor - Origin) / Scale;
        if (Local.X < 0 || Local.Y < 0 || Local.X >= 320 || Local.Y >= 200) { return; }
        // CONTROL.C637 uses the original bitmap centered at its natural width/2,height/2 hotspot.
        const FVector2D Size(Image->GetSizeX(), Image->GetSizeY());
        FCanvasTileItem Tile(Cursor - Size * (Scale * 0.5), Image->GetResource(), Size * Scale, FLinearColor::White);
        Tile.BlendMode = SE_BLEND_Translucent;
        Canvas.DrawItem(Tile);
    }

}

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

    inline bool InBag(FVector2D Point)
    {return Point.X>=213&&Point.X<=315&&Point.Y>=16&&Point.Y<=129;}
    struct FCell
    {
        int32 FirstSlot=INDEX_NONE,Count=0,Page=0;
        FIntPoint Anchor=FIntPoint::ZeroValue,Footprint=FIntPoint(1,1),Bitmap=FIntPoint(17,14);
        FBox2D Box() const
        {
            const FVector2D Start=Bag.Min+FVector2D(Anchor.X*17,Anchor.Y*14);
            return {Start,Start+FVector2D(Footprint.X*17,Footprint.Y*14)};
        }
    };
    struct FView
    {
        FView(const ANativeWadPlayer& Player,const UNativeWadHudData&,bool =true,int32 =INDEX_NONE)
        {
            const auto& Layout=Player.GetBagLayout();const auto Items=Player.GetBagItems();
            CellBySlot.Init(INDEX_NONE,Items.Num());Pages=Layout.PageCount();
            TArray<int32> Counts,CellByLeader;Counts.Init(0,Items.Num());CellByLeader.Init(INDEX_NONE,Items.Num());
            for(int32 Slot=0;Slot<Items.Num();++Slot){const int32 Leader=Layout.LeaderOf(Slot);if(Counts.IsValidIndex(Leader)){++Counts[Leader];}}
            for(int32 Leader:Layout.LeadersInOrder())
            {
                const auto& P=Layout.GetPlacements()[Leader];const auto& Item=Items[Leader];
                const int32 Cell=Cells.Add({Leader,Counts[Leader],P.Page,{P.X,P.Y},FWadBagLayout::Footprint(Item.Bitmap),Item.Bitmap});
                CellByLeader[Leader]=Cell;
            }
            for(int32 Slot=0;Slot<Items.Num();++Slot){const int32 Leader=Layout.LeaderOf(Slot);if(CellByLeader.IsValidIndex(Leader)){CellBySlot[Slot]=CellByLeader[Leader];}}
        }
        int32 PageCount() const{return Pages;}
        int32 CellForSlot(int32 Slot) const{return CellBySlot.IsValidIndex(Slot)?CellBySlot[Slot]:INDEX_NONE;}
        FBox2D Box(int32 Cell) const{return Cells.IsValidIndex(Cell)?Cells[Cell].Box():FBox2D(FVector2D(-1,-1),FVector2D(-1,-1));}
        bool bNeedsPlacementCell=false; // Five fixed source pages already provide all legitimate targets.
        TArray<FCell> Cells;
        TArray<int32> CellBySlot;
        int32 Pages=5;
    };
    inline void DrawGrid(UCanvas& Canvas,const UNativeWadHudData& Data,FVector2D Origin,double Scale,
        FVector2D Start,int32 GridColumns,int32 GridRows)
    {
        const auto Rect=[&](FVector2D P,FVector2D Size,uint8 Color)
        {
            FCanvasTileItem Tile(Origin+P*Scale,Size*Scale,FLinearColor::FromSRGBColor(Data.Palette[Color]));
            Tile.BlendMode=SE_BLEND_Translucent;Canvas.DrawItem(Tile);
        };
        Rect(Start,FVector2D(GridColumns*17+1,GridRows*14+1),76);
        for(int32 X=0;X<=GridColumns;++X){Rect(Start+FVector2D(X*17,0),FVector2D(1,GridRows*14+1),79);}
        for(int32 Y=0;Y<=GridRows;++Y){Rect(Start+FVector2D(0,Y*14),FVector2D(GridColumns*17+1,1),79);}
    }
    inline void DrawFootprintItem(UCanvas& Canvas,const UNativeWadHudData& Data,FVector2D Origin,double Scale,
        int32 Type,FVector2D Corner,FIntPoint Footprint,FIntPoint Bitmap,int32 Count=1)
    {
        const auto Rect=[&](FVector2D P,FVector2D Size,uint8 Color)
        {
            FCanvasTileItem Tile(Origin+P*Scale,Size*Scale,FLinearColor::FromSRGBColor(Data.Palette[Color]));
            Tile.BlendMode=SE_BLEND_Translucent;Canvas.DrawItem(Tile);
        };
        const FVector2D Span(Footprint.X*17,Footprint.Y*14);
        Rect(Corner,Span+FVector2D(1,1),75);
        Rect(Corner,FVector2D(Span.X+1,1),73);Rect(Corner+FVector2D(0,Span.Y),FVector2D(Span.X+1,1),73);
        Rect(Corner,FVector2D(1,Span.Y+1),73);Rect(Corner+FVector2D(Span.X,0),FVector2D(1,Span.Y+1),73);
        if(const UTexture2D* Image=Data.Items.FindRef(Type))
        {
            const FVector2D At=Corner+FVector2D(1+(int32(Span.X)-Bitmap.X)/2,1+(int32(Span.Y)-Bitmap.Y)/2);
            FCanvasTileItem Tile(Origin+At*Scale,Image->GetResource(),FVector2D(Bitmap)*Scale,FLinearColor::White);
            Tile.BlendMode=SE_BLEND_Translucent;Canvas.DrawItem(Tile);
        }
        else{DrawWadBitmapText(Canvas,Data.Tiny,Data.Palette,TEXT("?"),Origin+(Corner+FVector2D(2,2))*Scale,Scale,int32(Span.X)-2,1,0,false,144);}
        if(Count>1)
        {
            const FString Number=FString::FromInt(Count);
            DrawWadBitmapText(Canvas,Data.Tiny,Data.Palette,Number,Origin+(Corner+FVector2D(3,3))*Scale,Scale,int32(Span.X)-2,1,0,false,0);
            DrawWadBitmapText(Canvas,Data.Tiny,Data.Palette,Number,Origin+(Corner+FVector2D(2,2))*Scale,Scale,int32(Span.X)-2,1,0,false,144);
        }
    }
    inline void DrawBagPage(UCanvas& Canvas,const ANativeWadPlayer& Player,const UNativeWadHudData& Data,
        FVector2D Origin,double Scale,int32 Page)
    {
        DrawGrid(Canvas,Data,Origin,Scale,Bag.Min,6,8);const FView View(Player,Data);
        for(const auto& Cell:View.Cells)
        {if(Cell.Page==Page){DrawFootprintItem(Canvas,Data,Origin,Scale,Player.GetCarriedItems()[Cell.FirstSlot],Cell.Box().Min,Cell.Footprint,Cell.Bitmap,Cell.Count);}}
    }
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
        const FVector2D Size(Data.GetItemBitmapSize(Player.GetMouseHandItem()));
        FCanvasTileItem Tile(Cursor - Size * (Scale * 0.5), Image->GetResource(), Size * Scale, FLinearColor::White);
        Tile.BlendMode = SE_BLEND_Translucent;
        Canvas.DrawItem(Tile);
    }

}

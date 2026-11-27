#include "NativeWadLook.h"

#include "NativeWadHudData.h"

bool NativeWadLook::ParseDescription(const FString& Source, FWadCreatureDescription& Out)
{
    FString Text = Source;
    Text.ReplaceInline(TEXT("\r\n"), TEXT("\n"));
    Text.ReplaceInline(TEXT("\r"), TEXT("\n"));
    int32 First = INDEX_NONE;
    if (!Text.FindChar('\n', First)) { return false; }
    const int32 Second = Text.Find(TEXT("\n"), ESearchCase::CaseSensitive, ESearchDir::FromStart, First + 1);
    Out.Name = Text.Left(First);
    Out.Description = Second == INDEX_NONE ? Text.Mid(First + 1) : Text.Mid(First + 1, Second - First - 1);
    return true;
}

FBox2D NativeWadLook::PortraitBox(FVector2D NaturalSize)
{
    const FBox2D Frame({215, 16}, {314, 85});
    if (NaturalSize.X <= 0 || NaturalSize.Y <= 0) { return Frame; }
    // GRAPHICS.C centers smaller pictures at natural size and scales oversized pictures down uniformly.
    const double Scale = FMath::Min3(1.0, Frame.GetSize().X / NaturalSize.X, Frame.GetSize().Y / NaturalSize.Y);
    const FVector2D Size(FMath::FloorToDouble(NaturalSize.X * Scale), FMath::FloorToDouble(NaturalSize.Y * Scale));
    const FVector2D Margin = (Frame.GetSize() - Size) * 0.5;
    const FVector2D Start = Frame.Min + FVector2D(FMath::FloorToDouble(Margin.X), FMath::FloorToDouble(Margin.Y));
    return {Start, Start + Size};
}

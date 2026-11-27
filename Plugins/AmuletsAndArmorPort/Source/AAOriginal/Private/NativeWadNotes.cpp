#include "NativeWadNotes.h"

#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "Input/Events.h"
#include "Input/Reply.h"
#include "InputCoreTypes.h"
#include "NativeWadHudData.h"
#include "NativeWadPlayer.h"
#include "Widgets/SLeafWidget.h"

FNativeWadNotesEditor::FNativeWadNotesEditor(const FWadTownFont& Font, const FString& InitialText)
    : Widths(Font.Widths), LineHeight(FMath::Max(1, Font.Height))
{
    // Keep old saves and pasted text within the source buffer and supported bitmap character set.
    bool bAfterReturn = false;
    for (TCHAR Ch : InitialText)
    {
        if (Ch == '\n' && bAfterReturn) { bAfterReturn = false; continue; }
        bAfterReturn = Ch == '\r';
        if (Ch == '\r') { Ch = '\n'; }
        if (Text.Len() < MaxLength && (Ch == '\n' || Ch == '\t' || (Ch >= 32 && Ch < 128))) { Text.AppendChar(Ch); }
    }
    Reflow();
}

int32 FNativeWadNotesEditor::Advance(TCHAR Ch, int32 X) const
{
    const auto Width = [&](TCHAR Glyph) { return Widths.IsValidIndex(int32(Glyph)) ? int32(Widths[Glyph]) : 4; };
    if (Ch == '\t')
    {
        const int32 Tab = FMath::Max(1, 3 * Width('W')); // TXTBOX.C uses three capital W's per tab stop.
        return Tab - X % Tab;
    }
    return FMath::Max(1, Width(Ch));
}

void FNativeWadNotesEditor::Reflow()
{
    Lines.Reset();
    const int32 CaretRoom = Advance('W', 0);
    int32 Start = 0, Index = 0, X = 0, Space = INDEX_NONE;
    while (Index < Text.Len())
    {
        const TCHAR Ch = Text[Index];
        if (Ch == '\n')
        {
            Lines.Add({Start, Index});
            Start = ++Index; X = 0; Space = INDEX_NONE;
            continue;
        }
        const int32 Step = Advance(Ch, X);
        if (X + Step + CaretRoom > FieldWidth && Index > Start)
        {
            const int32 End = Space >= Start ? Space + 1 : Index;
            Lines.Add({Start, End});
            Start = Index = End; X = 0; Space = INDEX_NONE;
            continue;
        }
        if (Ch == ' ' || Ch == '\t') { Space = Index; }
        X += Step;
        ++Index;
    }
    Lines.Add({Start, Text.Len()});
    Cursor = FMath::Clamp(Cursor, 0, Text.Len());
    Scroll = FMath::Clamp(Scroll, 0, FMath::Max(0, Lines.Num() - GetVisibleRows()));
}

int32 FNativeWadNotesEditor::GetCursorLine() const
{
    int32 Row = 0;
    while (Row + 1 < Lines.Num() && Lines[Row + 1].Start <= Cursor) { ++Row; }
    return Row;
}

int32 FNativeWadNotesEditor::CursorX(int32 Row, int32 Position) const
{
    int32 X = 0;
    for (int32 Index = Lines[Row].Start; Index < FMath::Min(Position, Lines[Row].End); ++Index)
    { X += Advance(Text[Index], X); }
    return X;
}

int32 FNativeWadNotesEditor::PositionAt(int32 Row, int32 WantedX) const
{
    const FLine& Line = Lines[Row];
    int32 X = 0;
    for (int32 Index = Line.Start; Index < Line.End; ++Index)
    {
        const int32 Step = Advance(Text[Index], X);
        if (WantedX < X + (Step + 1) / 2) { return Index; }
        X += Step;
    }
    // At a soft wrap this insertion point belongs to the following row.
    return Row + 1 < Lines.Num() && Lines[Row + 1].Start == Line.End ? FMath::Max(Line.Start, Line.End - 1) : Line.End;
}

void FNativeWadNotesEditor::RevealCursor()
{
    const int32 Row = GetCursorLine();
    Scroll = FMath::Clamp(Scroll, FMath::Max(0, Row - GetVisibleRows() + 1), Row);
    Scroll = FMath::Clamp(Scroll, 0, FMath::Max(0, Lines.Num() - GetVisibleRows()));
}

bool FNativeWadNotesEditor::Insert(TCHAR Ch)
{
    if (Ch == '\r') { Ch = '\n'; }
    if (Text.Len() >= MaxLength || (Ch != '\n' && Ch != '\t' && (Ch < 32 || Ch >= 128))) { return false; }
    Text.InsertAt(Cursor++, Ch);
    PreferredX = INDEX_NONE;
    Reflow(); RevealCursor();
    return true;
}

bool FNativeWadNotesEditor::Key(const FKey& Key, bool bControl)
{
    bool bChanged = false;
    const int32 Row = GetCursorLine();
    if (Key == EKeys::BackSpace)
    {
        if (Cursor > 0) { Text.RemoveAt(--Cursor, 1); bChanged = true; }
    }
    else if (Key == EKeys::Delete)
    {
        if (Cursor < Text.Len()) { Text.RemoveAt(Cursor, 1); bChanged = true; }
    }
    else if (Key == EKeys::Left) { Cursor = FMath::Max(0, Cursor - 1); }
    else if (Key == EKeys::Right) { Cursor = FMath::Min(Text.Len(), Cursor + 1); }
    else if (Key == EKeys::Home) { Cursor = bControl ? 0 : Lines[Row].Start; }
    else if (Key == EKeys::End) { Cursor = bControl ? Text.Len() : PositionAt(Row, MAX_int32); }
    else if (Key == EKeys::Up || Key == EKeys::Down || Key == EKeys::PageUp || Key == EKeys::PageDown)
    {
        if (PreferredX == INDEX_NONE) { PreferredX = CursorX(Row, Cursor); }
        const int32 Delta = Key == EKeys::Up ? -1 : Key == EKeys::Down ? 1
            : Key == EKeys::PageUp ? -GetVisibleRows() : GetVisibleRows();
        Cursor = PositionAt(FMath::Clamp(Row + Delta, 0, Lines.Num() - 1), PreferredX);
        RevealCursor();
        return false;
    }
    else { return false; }
    PreferredX = INDEX_NONE;
    if (bChanged) { Reflow(); }
    RevealCursor();
    return bChanged;
}

void FNativeWadNotesEditor::ScrollLines(int32 Delta)
{
    Scroll = FMath::Clamp(Scroll + Delta, 0, FMath::Max(0, Lines.Num() - GetVisibleRows()));
}

void FNativeWadNotesEditor::PlaceCursor(FVector2D Point)
{
    const int32 Row = FMath::Clamp(Scroll + FMath::FloorToInt(Point.Y / LineHeight), 0, Lines.Num() - 1);
    Cursor = PositionAt(Row, FMath::RoundToInt(Point.X));
    PreferredX = INDEX_NONE;
    RevealCursor();
}

namespace
{
    // Invisible full-viewport input surface. The existing HUD still draws the imported form and bitmap text.
    class SNativeWadNotesInput : public SLeafWidget
    {
    public:
        SLATE_BEGIN_ARGS(SNativeWadNotesInput) {} SLATE_END_ARGS()
        void Construct(const FArguments&, ANativeWadHUD* InHUD) { HUD = InHUD; }
        virtual bool SupportsKeyboardFocus() const override { return true; }
        virtual FReply OnKeyChar(const FGeometry&, const FCharacterEvent& Event) override
        {
            if (HUD.IsValid()) { HUD->HandleNotesCharacter(Event.GetCharacter()); }
            return FReply::Handled();
        }
        virtual FReply OnKeyDown(const FGeometry&, const FKeyEvent& Event) override
        {
            if (HUD.IsValid()) { HUD->HandleNotesKey(Event.GetKey(), Event.IsControlDown(), Event.IsAltDown()); }
            return FReply::Handled();
        }
        virtual FReply OnKeyUp(const FGeometry&, const FKeyEvent&) override { return FReply::Handled(); }
        virtual FReply OnMouseButtonDown(const FGeometry& Geometry, const FPointerEvent& Event) override
        {
            if (HUD.IsValid() && Event.GetEffectingButton() == EKeys::LeftMouseButton)
            {
                FVector2D Point = Geometry.AbsoluteToLocal(Event.GetScreenSpacePosition());
                // Canvas uses viewport pixels; Slate coordinates may be scaled by the desktop's DPI.
                if (const auto* Controller = HUD->GetOwningPlayerController())
                {
                    int32 Width = 0, Height = 0;
                    Controller->GetViewportSize(Width, Height);
                    const FVector2D SlateSize = Geometry.GetLocalSize();
                    if (SlateSize.X > 0 && SlateSize.Y > 0) { Point *= FVector2D(Width, Height) / SlateSize; }
                }
                HUD->ClickHUD(Point);
                if (HUD.IsValid() && HUD->IsNotesOpen()) { return FReply::Handled().SetUserFocus(SharedThis(this)); }
            }
            return FReply::Handled();
        }
        virtual FReply OnMouseButtonUp(const FGeometry&, const FPointerEvent&) override { return FReply::Handled(); }
        virtual FReply OnMouseWheel(const FGeometry&, const FPointerEvent& Event) override
        {
            if (HUD.IsValid()) { HUD->ScrollNotes(Event.GetWheelDelta() > 0 ? -3 : 3); }
            return FReply::Handled();
        }
    private:
        virtual int32 OnPaint(const FPaintArgs&, const FGeometry&, const FSlateRect&, FSlateWindowElementList&,
            int32 Layer, const FWidgetStyle&, bool) const override { return Layer; }
        virtual FVector2D ComputeDesiredSize(float) const override { return FVector2D::ZeroVector; }
        TWeakObjectPtr<ANativeWadHUD> HUD;
    };
}

TSharedRef<SWidget> MakeNativeWadNotesInput(ANativeWadHUD& HUD)
{
    return SNew(SNativeWadNotesInput, &HUD);
}

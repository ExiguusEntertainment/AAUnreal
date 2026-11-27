#pragma once

#include "CoreMinimal.h"

class SWidget;
class ANativeWadHUD;
struct FWadTownFont;
struct FKey;

// The original Notes form is a single 1000-byte character buffer, including its terminator.
// This model uses the imported Tiny font for wrapping and cursor placement; Slate only supplies input.
class AAORIGINAL_API FNativeWadNotesEditor
{
public:
    static constexpr int32 MaxLength = 999;
    static constexpr int32 FieldWidth = 92;
    static constexpr int32 FieldHeight = 120;

    struct FLine { int32 Start; int32 End; };

    explicit FNativeWadNotesEditor(const FWadTownFont& Font, const FString& InitialText);
    const FString& GetText() const { return Text; }
    const TArray<FLine>& GetLines() const { return Lines; }
    int32 GetCursor() const { return Cursor; }
    int32 GetScroll() const { return Scroll; }
    int32 GetLineHeight() const { return LineHeight; }
    int32 GetVisibleRows() const { return FMath::Max(1, FieldHeight / LineHeight); }
    int32 GetCursorLine() const;
    int32 Advance(TCHAR Ch, int32 X) const;
    int32 CursorX(int32 Line, int32 Position) const;
    bool Insert(TCHAR Ch);
    bool Key(const FKey& Key, bool bControl);
    void ScrollLines(int32 Delta);
    void PlaceCursor(FVector2D FieldPoint);

private:
    void Reflow();
    void RevealCursor();
    int32 PositionAt(int32 Line, int32 X) const;
    FString Text;
    TArray<uint8> Widths;
    TArray<FLine> Lines;
    int32 Cursor = 0;
    int32 Scroll = 0;
    int32 LineHeight = 6;
    int32 PreferredX = INDEX_NONE;
};

TSharedRef<SWidget> MakeNativeWadNotesInput(ANativeWadHUD& HUD);

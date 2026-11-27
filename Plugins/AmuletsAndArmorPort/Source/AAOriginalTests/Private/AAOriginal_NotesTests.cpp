#include "Misc/AutomationTest.h"
#include "InputCoreTypes.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"
#include "../../AAOriginal/Private/NativeWadNotes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPlayerNotes, "AAOriginal.WAD.Interface.PlayerNotes",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

bool FAAOriginalPlayerNotes::RunTest(const FString& Parameters)
{
    FWadTownFont Font;
    Font.Widths.Init(4, 256);
    Font.Widths['W'] = 6;
    Font.Widths['i'] = 1;
    Font.Height = 6;
    FNativeWadNotesEditor Editor(Font, TEXT("gate\r\nkey"));
    TestEqual(TEXT("Original carriage returns normalize without doubling CRLF lines"), Editor.GetText(), FString(TEXT("gate\nkey")));
    TestEqual(TEXT("Opening focuses the first insertion point"), Editor.GetCursor(), 0);
    Editor.Key(EKeys::End, false);
    Editor.Insert('!');
    Editor.Key(EKeys::Down, false);
    Editor.Key(EKeys::Home, false);
    Editor.Key(EKeys::Delete, false);
    Editor.Insert('K');
    TestEqual(TEXT("Cursor editing preserves the other line"), Editor.GetText(), FString(TEXT("gate!\nKey")));
    Editor.Key(EKeys::End, true);
    Editor.Insert('\r');
    Editor.Insert('x');
    Editor.Key(EKeys::BackSpace, false);
    TestEqual(TEXT("Return inserts a line and backspace removes one character"), Editor.GetText(), FString(TEXT("gate!\nKey\n")));
    TestEqual(TEXT("Source tabs advance to the next three-W stop"), Editor.Advance('\t', 4), 14);
    TestFalse(TEXT("Unsupported control characters do not enter the bitmap buffer"), Editor.Insert(TCHAR(1)));
    TestFalse(TEXT("Unsupported Unicode does not enter the source ASCII field"), Editor.Insert(TCHAR(0x2603)));

    FNativeWadNotesEditor Wrapped(Font, TEXT("iiiiiiiiiiiiiiiiiiii alpha beta gamma delta epsilon zeta eta theta"));
    TestTrue(TEXT("Tiny glyph widths produce wrapped lines"), Wrapped.GetLines().Num() > 1);
    for (int32 Row = 0; Row < Wrapped.GetLines().Num(); ++Row)
    {
        const auto& Line = Wrapped.GetLines()[Row];
        TestTrue(TEXT("Each row fits the source field with caret room"),
            Wrapped.CursorX(Row, Line.End) + Wrapped.Advance('W', 0) <= FNativeWadNotesEditor::FieldWidth);
        if (Row + 1 < Wrapped.GetLines().Num())
        { TestEqual(TEXT("Soft wrapping does not lose source characters"), Line.End, Wrapped.GetLines()[Row + 1].Start); }
    }
    Wrapped.Key(EKeys::End, true);
    const int32 LastRow = Wrapped.GetCursorLine();
    Wrapped.Key(EKeys::Up, false);
    TestEqual(TEXT("Up moves through wrapped rows"), Wrapped.GetCursorLine(), LastRow - 1);

    FNativeWadNotesEditor Full(Font, FString::ChrN(1200, 'a'));
    TestEqual(TEXT("Oversized saved text is bounded to the source buffer minus terminator"), Full.GetText().Len(), 999);
    TestFalse(TEXT("Typing refuses overflow without changing the character text"), Full.Insert('z'));
    Full.Key(EKeys::End, true);
    TestTrue(TEXT("The cursor scrolls into view on the final wrapped row"), Full.GetScroll() > 0);
    Full.ScrollLines(-MAX_int32);
    TestEqual(TEXT("Wheel scrolling clamps at the first row"), Full.GetScroll(), 0);
    Full.Key(EKeys::PageUp, false);
    TestTrue(TEXT("Page navigation keeps the cursor in the visible region"),
        Full.GetCursorLine() >= Full.GetScroll() && Full.GetCursorLine() < Full.GetScroll() + Full.GetVisibleRows());
    Full.Key(EKeys::BackSpace, false);
    TestTrue(TEXT("Deleting frees exactly one character of buffer space"), Full.Insert('z'));
    TestEqual(TEXT("Editing at the limit retains the terminator allowance"), Full.GetText().Len(), 999);
    return true;
}

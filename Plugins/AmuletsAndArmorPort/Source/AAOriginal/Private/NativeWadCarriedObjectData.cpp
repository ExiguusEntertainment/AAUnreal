#include "NativeWadCarriedObjectData.h"

namespace
{
    bool ValidType(int32 Type) { return Type > 0 && Type <= MAX_uint16; }
    bool ValidPayload(bool bRecorded, int32 Value)
    { return Value >= 0 && Value <= MAX_uint16 && (bRecorded || Value == 0); }
    bool ValidTypes(TConstArrayView<int32> Types)
    {
        if (Types.Num() > FWadCarriedObjectState::MaxItems) { return false; }
        for (int32 Type : Types) { if (!ValidType(Type)) { return false; } }
        return true;
    }
    bool Number(const FString& Token, int32 Limit, int32& Value)
    {
        if (Token.IsEmpty() || Token.Len() > 6 || (Token.Len() > 1 && Token[0] == TEXT('0'))) { return false; }
        int32 Parsed = 0;
        for (TCHAR Ch : Token)
        {
            if (Ch < TEXT('0') || Ch > TEXT('9')) { return false; }
            Parsed = Parsed * 10 + Ch - TEXT('0');
            if (Parsed > Limit) { return false; }
        }
        Value = Parsed;
        return true;
    }
}

bool FWadCarriedObjectState::InitializeLegacy(TConstArrayView<int32> Types)
{
    if (!ValidTypes(Types)) { return false; }
    TArray<FWadCarriedObjectData> Fresh;
    Fresh.Reserve(Types.Num());
    for (int32 Type : Types) { Fresh.Add({uint16(Type), 0, false}); }
    Entries = MoveTemp(Fresh);
    return true;
}

bool FWadCarriedObjectState::Validate(TConstArrayView<int32> Types) const
{
    if (!ValidTypes(Types) || Types.Num() != Entries.Num()) { return false; }
    for (int32 Slot = 0; Slot < Entries.Num(); ++Slot)
    {
        const auto& Item = Entries[Slot];
        if (Types[Slot] != Item.Type || !ValidPayload(Item.bAccessoryDataRecorded, Item.AccessoryData)) { return false; }
    }
    return true;
}

bool FWadCarriedObjectState::Append(int32 Type, bool bRecorded, int32 AccessoryData)
{
    if (Entries.Num() >= MaxItems || !ValidType(Type) || !ValidPayload(bRecorded, AccessoryData)) { return false; }
    Entries.Add({uint16(Type), uint16(AccessoryData), bRecorded});
    return true;
}

bool FWadCarriedObjectState::RemoveAt(int32 Slot)
{
    if (!Entries.IsValidIndex(Slot)) { return false; }
    Entries.RemoveAt(Slot);
    return true;
}

bool FWadCarriedObjectState::SwapEntries(int32 First, int32 Second)
{
    if (!Entries.IsValidIndex(First) || !Entries.IsValidIndex(Second)) { return false; }
    Entries.Swap(First, Second);
    return true;
}

bool FWadCarriedObjectState::SetAccessoryData(int32 Slot, bool bRecorded, int32 AccessoryData)
{
    if (!Entries.IsValidIndex(Slot) || !ValidPayload(bRecorded, AccessoryData)) { return false; }
    Entries[Slot].AccessoryData = uint16(AccessoryData);
    Entries[Slot].bAccessoryDataRecorded = bRecorded;
    return true;
}

FString FWadCarriedObjectState::Encode(TConstArrayView<int32> Types) const
{
    if (!Validate(Types)) { return {}; }
    FString Result = FString::Printf(TEXT("1;%d;"), Entries.Num());
    Result.Reserve(Entries.Num() * 14 + 20);
    for (int32 Slot = 0; Slot < Entries.Num(); ++Slot)
    {
        if (Slot) { Result += TEXT(","); }
        const auto& Item = Entries[Slot];
        Result += FString::Printf(TEXT("%u:%d:%u"), uint32(Item.Type), Item.bAccessoryDataRecorded ? 1 : 0, uint32(Item.AccessoryData));
    }
    return Result.Len() <= MaxEncodedCharacters ? Result : FString();
}

bool FWadCarriedObjectState::Decode(const FString& Payload, TConstArrayView<int32> Types)
{
    if (!ValidTypes(Types) || Payload.Len() > MaxEncodedCharacters || !Payload.StartsWith(TEXT("1;"))) { return false; }
    const int32 HeaderEnd = Payload.Find(TEXT(";"), ESearchCase::CaseSensitive, ESearchDir::FromStart, 2);
    int32 Count = 0;
    if (HeaderEnd == INDEX_NONE || !Number(Payload.Mid(2, HeaderEnd - 2), MaxItems, Count) || Count != Types.Num()) { return false; }
    const FString Rows = Payload.Mid(HeaderEnd + 1);
    FWadCarriedObjectState Fresh;
    if (Count == 0)
    {
        if (!Rows.IsEmpty()) { return false; }
        Entries.Reset();
        return true;
    }
    if (Rows.Len() < Count * 6 - 1 || Rows.Len() > Count * 14 - 1) { return false; }
    Fresh.Entries.Reserve(Count);
    int32 Start = 0;
    for (int32 Slot = 0; Slot < Count; ++Slot)
    {
        const int32 Comma = Rows.Find(TEXT(","), ESearchCase::CaseSensitive, ESearchDir::FromStart, Start);
        const bool bLast = Slot == Count - 1;
        if ((Comma == INDEX_NONE) != bLast) { return false; }
        const int32 Length = (bLast ? Rows.Len() : Comma) - Start;
        if (Length < 5 || Length > 13) { return false; }
        TArray<FString> Fields;
        Rows.Mid(Start, Length).ParseIntoArray(Fields, TEXT(":"), false);
        Start += Length + 1;
        int32 Type = 0, Recorded = 0, Value = 0;
        if (Fields.Num() != 3 || !Number(Fields[0], MAX_uint16, Type) || !ValidType(Type) || Type != Types[Slot]
            || !Number(Fields[1], 1, Recorded) || !Number(Fields[2], MAX_uint16, Value)
            || !Fresh.Append(Type, Recorded == 1, Value)) { return false; }
    }
    if (!Fresh.Validate(Types)) { return false; }
    Entries = MoveTemp(Fresh.Entries);
    return true;
}

EWadCarriedDataRestore FWadCarriedObjectState::Restore(const TCHAR* Payload, TConstArrayView<int32> Types)
{
    if (!Payload) { return InitializeLegacy(Types) ? EWadCarriedDataRestore::LegacyAbsent : EWadCarriedDataRestore::Invalid; }
    return Decode(Payload, Types) ? EWadCarriedDataRestore::Modern : EWadCarriedDataRestore::Invalid;
}

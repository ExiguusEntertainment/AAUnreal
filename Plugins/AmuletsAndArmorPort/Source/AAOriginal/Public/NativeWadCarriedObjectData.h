#pragma once

#include "CoreMinimal.h"

// A per-character sidecar for the existing ordered CarriedItems array. The
// full type is a witness, never an object ID or a 0x0fff art-sharing key.
struct FWadCarriedObjectData
{
    uint16 Type = 0;
    uint16 AccessoryData = 0;
    bool bAccessoryDataRecorded = false;
    bool operator==(const FWadCarriedObjectData& Other) const
    {
        return Type == Other.Type && AccessoryData == Other.AccessoryData
            && bAccessoryDataRecorded == Other.bAccessoryDataRecorded;
    }
};

enum class EWadCarriedDataRestore : uint8 { LegacyAbsent, Modern, Invalid };

// No global IDs, actors, catalog, bag geometry or equipment locations live here.
// Ownership swaps (bag/hand/ready/worn) retain the same entry and need no mutation.
// SwapEntries is ONLY for an actual reorder of the matching CarriedItems entries.
class AAORIGINAL_API FWadCarriedObjectState
{
public:
    static constexpr int32 MaxItems = 65536;
    static constexpr int32 MaxEncodedCharacters = 1024 * 1024;
    const TArray<FWadCarriedObjectData>& GetEntries() const { return Entries; }
    const FWadCarriedObjectData* Get(int32 Slot) const { return Entries.IsValidIndex(Slot) ? &Entries[Slot] : nullptr; }
    bool InitializeLegacy(TConstArrayView<int32> Types);
    bool Validate(TConstArrayView<int32> Types) const;
    bool Append(int32 Type, bool bRecorded = false, int32 AccessoryData = 0);
    bool RemoveAt(int32 Slot);
    bool SwapEntries(int32 First, int32 Second);
    bool SetAccessoryData(int32 Slot, bool bRecorded, int32 AccessoryData);
    // Canonical bounded ASCII: 1;count;fulltype:recorded:value,...
    // Encode returns empty for mismatched/invalid state. Empty ownership is 1;0;.
    FString Encode(TConstArrayView<int32> Types) const;
    bool Decode(const FString& Payload, TConstArrayView<int32> Types);
    // Absence is a deliberate legacy migration. An explicitly empty or malformed
    // modern payload fails atomically; it must never silently erase known data.
    EWadCarriedDataRestore Restore(const TCHAR* Payload, TConstArrayView<int32> Types);
private:
    TArray<FWadCarriedObjectData> Entries;
};

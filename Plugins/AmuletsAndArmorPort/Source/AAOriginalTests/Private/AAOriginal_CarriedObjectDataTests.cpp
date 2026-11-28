#include "Misc/AutomationTest.h"
#include "NativeWadCarriedObjectData.h"
#include "Engine/EngineBaseTypes.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCarriedObjectData, "AAOriginal.WAD.Inventory.CarriedObjectData",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalCarriedObjectData::RunTest(const FString& Parameters)
{
    // Physical journal pages and partially filled quivers share an object type,
    // but each individual native entry retains its own original uint16 payload.
    TArray<int32> Types{373,373,373,147,147,37569,65535};
    FWadCarriedObjectState State;
    const int32 Data[]{0,407,672,6,12,65535,1};
    for (int32 I=0; I<Types.Num(); ++I)
    { if (!TestTrue(TEXT("Append an exact full-type object and payload"),State.Append(Types[I],true,Data[I]))) { return false; } }
    TestTrue(TEXT("The sidecar witnesses the complete ordered full types"),State.Validate(Types));
    const FString Saved=State.Encode(Types);
    TestEqual(TEXT("The canonical ASCII record is explicit and inspectable"),Saved,
        FString(TEXT("1;7;373:1:0,373:1:407,373:1:672,147:1:6,147:1:12,37569:1:65535,65535:1:1")));
    const FString TravelURL=TEXT("/Game/Maps/L301?WadObjectData=")+Saved+TEXT("?WadHealth=777");
    const FURL Parsed(nullptr,*TravelURL,TRAVEL_Absolute);
    const TCHAR* Option=Parsed.GetOption(TEXT("WadObjectData="),nullptr);
    if (!TestNotNull(TEXT("Canonical punctuation survives normal FURL travel options"),Option)) { return false; }
    TestEqual(TEXT("FURL retains the entire ordered metadata payload"),FString(Option),Saved);
    FWadCarriedObjectState Loaded;
    TestTrue(TEXT("The complete persisted record decodes"),Loaded.Decode(Option,Types));
    TestTrue(TEXT("Every exact per-object payload round-trips"),Loaded.GetEntries()==State.GetEntries());
    TestTrue(TEXT("Journal page0 remains explicitly recorded"),Loaded.Get(0)->bAccessoryDataRecorded&&Loaded.Get(0)->AccessoryData==0);
    TestEqual(TEXT("Packed type37569 never collapses to base705"),Loaded.Get(5)->Type,uint16(37569));
    TestTrue(TEXT("Appending a legacy/default object is supported"),Loaded.Append(373)); Types.Add(373);
    TestTrue(TEXT("Absent payload differs from a recorded page0"),!Loaded.Get(7)->bAccessoryDataRecorded&&Loaded.Get(7)->AccessoryData==0);

    // Moving between existing ownership locations changes location indices, not
    // item records. Retaining a hand across forms needs no identity conversion.
    int32 Ready=1,Hand=4;
    const auto Before=Loaded.GetEntries();
    Swap(Ready,Hand);
    TestTrue(TEXT("A Ready/held ownership-pointer swap retains all object records"),Loaded.GetEntries()==Before);
    TestEqual(TEXT("The held page remains407 after pointer swap"),Loaded.Get(Hand)->AccessoryData,uint16(407));
    TestEqual(TEXT("The Ready quiver keeps twelve rather than six"),Loaded.Get(Ready)->AccessoryData,uint16(12));
    TestTrue(TEXT("Removing an earlier object succeeds"),Loaded.RemoveAt(0)); Types.RemoveAt(0);
    --Ready; --Hand;
    TestTrue(TEXT("Central index removal preserves the surviving aligned witnesses"),Loaded.Validate(Types));
    TestEqual(TEXT("The remapped held page still has its exact payload"),Loaded.Get(Hand)->AccessoryData,uint16(407));
    TestEqual(TEXT("The remapped Ready entry retains its own payload"),Loaded.Get(Ready)->AccessoryData,uint16(12));
    TestTrue(TEXT("An actual array reorder swaps complete object records"),Loaded.SwapEntries(0,4)); Types.Swap(0,4);
    TestTrue(TEXT("An array reorder remains valid when both aligned arrays move"),Loaded.Validate(Types));
    TestEqual(TEXT("The reordered packed variant retains its uint16 payload"),Loaded.Get(0)->AccessoryData,uint16(65535));
    TestTrue(TEXT("A payload may be updated independently of position"),Loaded.SetAccessoryData(0,true,32768));
    const auto Valid=Loaded.GetEntries();
    TestFalse(TEXT("Invalid negative payload is rejected"),Loaded.SetAccessoryData(0,true,-1));
    TestFalse(TEXT("Invalid oversized payload is rejected"),Loaded.SetAccessoryData(0,true,65536));
    TestFalse(TEXT("Unrecorded nonzero data is not a canonical legacy state"),Loaded.SetAccessoryData(0,false,1));
    TestFalse(TEXT("Invalid slot removal is rejected"),Loaded.RemoveAt(Types.Num()));
    TestFalse(TEXT("Invalid reorder is rejected"),Loaded.SwapEntries(-1,0));
    TestFalse(TEXT("An invalid full type cannot be narrowed silently"),Loaded.Append(65536,true,0));
    TestTrue(TEXT("All invalid mutations leave the complete state unchanged"),Loaded.GetEntries()==Valid);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCarriedObjectCodec, "AAOriginal.WAD.Inventory.CarriedObjectCodec",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalCarriedObjectCodec::RunTest(const FString& Parameters)
{
    const TArray<int32> Types{373,37569};
    FWadCarriedObjectState State;
    State.Append(373,true,407); State.Append(37569,true,65535);
    const auto Before=State.GetEntries();
    const TCHAR* Bad[]{TEXT(""),TEXT("0;2;373:1:407,37569:1:65535"),TEXT("2;2;373:1:407,37569:1:65535"),
        TEXT("1;02;373:1:407,37569:1:65535"),TEXT("1;1;373:1:407"),TEXT("1;65537;373:1:407"),
        TEXT("1;2;373:1:407"),TEXT("1;2;373:1:407,37569:1:65535,"),TEXT("1;2;,373:1:407,37569:1:65535"),
        TEXT("1;2;373:1:407,,37569:1:65535"),TEXT("1;2;373:1:407,705:1:65535"),
        TEXT("1;2;37569:1:65535,373:1:407"),TEXT("1;2;00373:1:407,37569:1:65535"),
        TEXT("1;2;373:2:407,37569:1:65535"),TEXT("1;2;373:-1:407,37569:1:65535"),
        TEXT("1;2;373:0:407,37569:1:65535"),TEXT("1;2;373:1:65536,37569:1:65535"),
        TEXT("1;2;373:1:-1,37569:1:65535"),TEXT("1;2;373:1:+407,37569:1:65535"),
        TEXT("1;2;373:1:0407,37569:1:65535"),TEXT("1;2;373:1:407 ,37569:1:65535"),
        TEXT("1;2;373::407,37569:1:65535"),TEXT("1;2;373:1:407:0,37569:1:65535"),
        TEXT("1;2;373:1:407,37569:1:65535;"),TEXT("1;2;373:1:407,37569:1:65535?WadHealth=0")};
    for (const TCHAR* Payload : Bad)
    {
        TestFalse(FString::Printf(TEXT("Refuse malformed modern payload: %s"),Payload),State.Decode(Payload,Types));
        TestTrue(TEXT("Every malformed modern decode is atomic"),State.GetEntries()==Before);
    }
    TestTrue(TEXT("Explicit empty modern data refuses rather than migrates"),State.Restore(TEXT(""),Types)==EWadCarriedDataRestore::Invalid);
    TestTrue(TEXT("Failed restore retains exact known page and packed-variant metadata"),State.GetEntries()==Before);
    TestTrue(TEXT("Only genuinely absent metadata takes the legacy path"),State.Restore(nullptr,Types)==EWadCarriedDataRestore::LegacyAbsent);
    TestTrue(TEXT("Legacy defaults preserve types while recording no guessed payload"),State.Validate(Types)
        &&!State.Get(0)->bAccessoryDataRecorded&&!State.Get(1)->bAccessoryDataRecorded&&State.Get(0)->AccessoryData==0&&State.Get(1)->AccessoryData==0);
    TestTrue(TEXT("A valid modern record is distinguished from absence"),State.Restore(TEXT("1;2;373:1:0,37569:0:0"),Types)==EWadCarriedDataRestore::Modern);
    const auto Modern=State.GetEntries();
    const TArray<int32> InvalidTypes{0,37569};
    TestTrue(TEXT("Invalid ownership type prevents encoding"),State.Encode(InvalidTypes).IsEmpty());
    TestFalse(TEXT("Invalid ownership type prevents legacy initialization"),State.InitializeLegacy(InvalidTypes));
    TestTrue(TEXT("Invalid ownership witnesses cannot replace modern data"),State.GetEntries()==Modern);
    const TArray<int32> Reordered{37569,373};
    TestTrue(TEXT("Out-of-sync aligned arrays cannot be serialized"),State.Encode(Reordered).IsEmpty());
    const FString Oversized=FString::ChrN(FWadCarriedObjectState::MaxEncodedCharacters+1,TEXT('0'));
    TestFalse(TEXT("Bounded parser refuses excessive input before splitting it"),State.Decode(Oversized,Types));
    TestTrue(TEXT("Oversized input is also atomic"),State.GetEntries()==Modern);
    FWadCarriedObjectState Empty;
    const TArray<int32> NoTypes;
    TestEqual(TEXT("Empty ownership has an unambiguous valid modern encoding"),Empty.Encode(NoTypes),FString(TEXT("1;0;")));
    TestTrue(TEXT("Empty ownership record decodes"),Empty.Decode(TEXT("1;0;"),NoTypes));
    TestFalse(TEXT("Zero count cannot carry hidden trailing records"),Empty.Decode(TEXT("1;0;373:1:407"),NoTypes));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalCarriedObjectLimits, "AAOriginal.WAD.Inventory.CarriedObjectLimits",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalCarriedObjectLimits::RunTest(const FString& Parameters)
{
    // Existing legacy bag ownership can contain more items than a new five-bag
    // character. Its explicit maximum remains encodable without truncation.
    TArray<int32> Types; Types.Init(65535,FWadCarriedObjectState::MaxItems);
    FWadCarriedObjectState State;
    for (int32 I=0;I<Types.Num();++I)
    { if (!State.Append(65535,true,65535)) { AddError(TEXT("Valid bounded ownership was truncated")); return false; } }
    TestFalse(TEXT("Append beyond the ownership bound fails"),State.Append(65535));
    const FString Encoded=State.Encode(Types);
    TestTrue(TEXT("Worst-case uint16 ownership fits the documented ASCII bound"),!Encoded.IsEmpty()&&Encoded.Len()<=FWadCarriedObjectState::MaxEncodedCharacters);
    FWadCarriedObjectState Loaded;
    TestTrue(TEXT("The maximum valid record decodes without loss"),Loaded.Decode(Encoded,Types));
    TestTrue(TEXT("Every maximum-range payload survives"),Loaded.GetEntries()==State.GetEntries());
    Types.Add(65535);
    TestFalse(TEXT("Legacy absence cannot bypass the maximum ownership bound"),Loaded.InitializeLegacy(Types));
    TestEqual(TEXT("Rejected legacy oversize preserves the existing object count"),Loaded.GetEntries().Num(),FWadCarriedObjectState::MaxItems);
    return true;
}

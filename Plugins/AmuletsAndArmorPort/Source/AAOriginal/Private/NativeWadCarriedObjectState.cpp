#include "NativeWadCarriedObjectState.h"
#include "NativeWadCarriedObjectData.h"
#include "NativeWadPlayer.h"
#include "Engine/EngineBaseTypes.h"

namespace NativeWadCarriedObjectState
{
    bool ValidateOptions(const FURL& URL)
    {
        const TCHAR* Payload=URL.GetOption(TEXT("WadObjectData="),nullptr);
        if(!Payload){return !URL.HasOption(TEXT("WadObjectData"));}
        // Writers emit explicit rune ownership before the ordered witness, so
        // legacy class-rune migration cannot append entries behind modern data.
        if(!URL.HasOption(TEXT("WadRuneRules"))){return false;}
        TArray<int32> Types;
        const TCHAR* Items=URL.GetOption(TEXT("WadItems="),nullptr);
        if(!Items&&URL.HasOption(TEXT("WadItems"))){return false;}
        if(Items&&*Items)
        {
            const FString Source(Items);
            if(Source.Len()>FWadCarriedObjectState::MaxItems*6-1){return false;}
            TArray<FString> Tokens;Source.ParseIntoArray(Tokens,TEXT(","),false);
            if(Tokens.Num()>FWadCarriedObjectState::MaxItems){return false;}
            for(const auto& Token:Tokens)
            {
                int32 Type;
                if(!LexTryParseString(Type,*Token)||Type<=0||Type>MAX_uint16||LexToString(Type)!=Token){return false;}
                Types.Add(Type);
            }
        }
        FWadCarriedObjectState State;
        return State.Decode(Payload,Types);
    }
}

bool ANativeWadPlayer::EnsureCarriedObjectData()
{
    if(!bCarriedObjectDataInitialized)
    {
        bCarriedObjectDataInitialized=true;
        bCarriedObjectDataValid=CarriedObjectData.InitializeLegacy(CarriedItems);
    }
    return bCarriedObjectDataValid&&CarriedObjectData.Validate(CarriedItems);
}

bool ANativeWadPlayer::HasValidCarriedObjectData() const
{return const_cast<ANativeWadPlayer*>(this)->EnsureCarriedObjectData();}

const FWadCarriedObjectData* ANativeWadPlayer::GetCarriedObjectData(int32 Slot) const
{return HasValidCarriedObjectData()?CarriedObjectData.Get(Slot):nullptr;}

bool ANativeWadPlayer::AppendCarriedObject(int32 Type,bool bRecorded,int32 AccessoryData)
{
    // Bag or located-item capacity is checked by the caller before committing
    // its candidate layout. Every successful append owns exactly one sidecar row.
    if(!EnsureCarriedObjectData()||!CarriedObjectData.Append(Type,bRecorded,AccessoryData)){return false;}
    CarriedItems.Add(Type);return true;
}

bool ANativeWadPlayer::RemoveCarriedObjectAt(int32 Slot)
{
    if(!EnsureCarriedObjectData()||!CarriedItems.IsValidIndex(Slot)||!CarriedObjectData.RemoveAt(Slot)){return false;}
    CarriedItems.RemoveAt(Slot);return true;
}

void ANativeWadPlayer::RestoreCarriedObjectData(const FURL& URL)
{
    const TCHAR* Payload=URL.GetOption(TEXT("WadObjectData="),nullptr);
    if(!Payload&&URL.HasOption(TEXT("WadObjectData"))){Payload=TEXT("");}
    bCarriedObjectDataInitialized=true;
    bCarriedObjectDataValid=NativeWadCarriedObjectState::ValidateOptions(URL)
        &&CarriedObjectData.Restore(Payload,CarriedItems)!=EWadCarriedDataRestore::Invalid;
    if(!bCarriedObjectDataValid)
    {UE_LOG(LogTemp,Warning,TEXT("Rejected invalid direct-travel carried-object metadata; ownership was retained and inventory mutation/saving is disabled."));}
}

#include "NativeWadBagState.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/Texture2D.h"
#include "NativeWadHudData.h"

namespace NativeWadBagState
{
    namespace
    {
        bool Integer(const FString& Token,int32& Value)
        {
            if(Token.IsEmpty()||Token.Len()>11){return false;}
            const int32 Start=Token[0]==TEXT('-')?1:0;if(Start==Token.Len()){return false;}
            for(int32 I=Start;I<Token.Len();++I){if(Token[I]<TEXT('0')||Token[I]>TEXT('9')){return false;}}
            return LexTryParseString(Value,*Token)&&LexToString(Value)==Token;
        }
        bool OptionalInteger(const FURL& URL,const TCHAR* Name,int32& Value)
        {
            const TCHAR* Text=URL.GetOption(Name,nullptr);
            const FString Bare=FString(Name).LeftChop(1);
            return Text?Integer(Text,Value):!URL.HasOption(*Bare);
        }
    }
    FItemRule CatalogRule(int32 Type)
    {
        FItemRule Result;Result.Item.Type=Type;const auto* Catalog=UNativeWadHudData::LoadCatalog();
        const auto* Rule=Catalog?Catalog->ItemDetails.Find(Type):nullptr;
        if(Catalog){Result.Item.Bitmap=Catalog->GetItemBitmapSize(Type);}
        if(Rule)
        {
            Result.Item.StackLimit=FMath::Max(1,Rule->StackLimit);Result.ArmorSlot=Rule->ArmorSlot;
            Result.JewelrySlot=Rule->JewelrySlot;Result.Classes=Rule->UsableClasses;
        }
        return Result;
    }
    bool ValidateOptions(const FURL& URL,TFunctionRef<FItemRule(int32)> Resolve,FWadBagLayout* Out)
    {
        using namespace NativeWadRules;
        const TCHAR* Payload=URL.GetOption(TEXT("WadBagLayout="),nullptr);
        if(!Payload){return !URL.HasOption(TEXT("WadBagLayout"));} // Missing legacy field is deliberately distinct.
        if(!URL.HasOption(TEXT("WadRuneRules"))){return false;} // Modern writers always record rune ownership rules.
        int32 Class=PriestClass;
        if(!OptionalInteger(URL,TEXT("WadClass="),Class)||Class<0||Class>=CharacterClassCount){return false;}
        TArray<int32> Types;
        if(URL.HasOption(TEXT("WadItems"))&&!URL.GetOption(TEXT("WadItems="),nullptr)){return false;}
        if(const TCHAR* Text=URL.GetOption(TEXT("WadItems="),nullptr);Text&&*Text)
        {
            const FString Source(Text);if(Source.Len()>1024*1024){return false;}TArray<FString> Tokens;
            Source.ParseIntoArray(Tokens,TEXT(","),false);if(Tokens.Num()>FWadBagLayout::MaxSlots){return false;}
            for(const auto& Token:Tokens){int32 Type;if(!Integer(Token,Type)||Type<=0||Type>MAX_uint16){return false;}Types.Add(Type);}
        }
        TArray<FItemRule> Rules;TArray<FWadBagItem> Items;
        for(int32 Type:Types){auto R=Resolve(Type);R.Item.Type=Type;R.Item.bOffPage=IsRuneItem(Type);Rules.Add(R);Items.Add(R.Item);}
        TSet<int32> Located;
        const auto Locate=[&](int32 Slot,int32 Armor,int32 Jewelry,bool bUsable)
        {
            if(Slot==INDEX_NONE){return true;}
            if(!Items.IsValidIndex(Slot)||Located.Contains(Slot)||IsRuneItem(Types[Slot])){return false;}
            const auto& Rule=Rules[Slot];
            if(bUsable&&(Rule.Classes&ClassFlag(Class))==0){return false;}
            if(Armor!=INDEX_NONE&&Rule.ArmorSlot!=Armor&&!(Armor==RightArmSlot&&Rule.ArmorSlot==LeftArmSlot)){return false;}
            if(Jewelry!=INDEX_NONE&&Rule.JewelrySlot!=Jewelry){return false;}
            Located.Add(Slot);Items[Slot].bOffPage=true;return true;
        };
        int32 Ready=INDEX_NONE,Hand=INDEX_NONE;
        if(!OptionalInteger(URL,TEXT("WadReady="),Ready)||!Locate(Ready,INDEX_NONE,INDEX_NONE,true)){return false;}
        const auto Locations=[&](const TCHAR* Key,int32 Count,bool bJewelry)
        {
            const TCHAR* Text=URL.GetOption(Key,nullptr);
            if(!Text){return !URL.HasOption(*FString(Key).LeftChop(1));}TArray<FString> Tokens;
            FString(Text).ParseIntoArray(Tokens,TEXT(","),false);if(Tokens.Num()!=Count){return false;}
            for(int32 I=0;I<Count;++I)
            {
                int32 Slot;if(!Integer(Tokens[I],Slot)||!Locate(Slot,bJewelry?INDEX_NONE:I,
                    bJewelry?(I==NeckSlot?NeckSlot:Ring1Slot):INDEX_NONE,true)){return false;}
            }
            return true;
        };
        if(!Locations(TEXT("WadWorn="),EquipmentSlotCount,false)||!Locations(TEXT("WadJewelry="),JewelrySlotCount,true)
            ||!OptionalInteger(URL,TEXT("WadMouseHand="),Hand)||!Locate(Hand,INDEX_NONE,INDEX_NONE,false)){return false;}
        FWadBagLayout Parsed;if(!Parsed.Decode(Payload,Items)){return false;}
        int32 Page=0;if(!OptionalInteger(URL,TEXT("WadBagPage="),Page)||Page<0||Page>=Parsed.PageCount()){return false;}
        if(Out){*Out=MoveTemp(Parsed);}return true;
    }
}

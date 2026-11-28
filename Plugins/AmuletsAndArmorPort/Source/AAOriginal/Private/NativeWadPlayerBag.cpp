#include "NativeWadPlayer.h"
#include "NativeWadHudData.h"
#include "NativeWadBagState.h"
#include "Engine/Texture2D.h"

TArray<FWadBagItem> ANativeWadPlayer::GetBagItems() const
{
    TArray<FWadBagItem> Result;Result.Reserve(CarriedItems.Num());
    for(int32 I=0;I<CarriedItems.Num();++I)
    {
        auto Item=NativeWadBagState::CatalogRule(CarriedItems[I]).Item;
        Item.bOffPage=NativeWadRules::IsRuneItem(Item.Type)||I==ReadySlot||I==SelectedInventorySlot||IsWornInventorySlot(I);
        Result.Add(Item);
    }
    return Result;
}

void ANativeWadPlayer::RestoreBagLayout(const TCHAR* Payload)
{
    const auto Items=GetBagItems();BagLayout.Reset();bModernBagLayout=Payload!=nullptr;
    bBagLayoutInitialized=Payload&&BagLayout.Decode(Payload,Items);
    if(Payload&&!bBagLayoutInitialized){UE_LOG(LogTemp,Warning,TEXT("Rejected invalid direct-travel bag placement; repair is limited to the five source bags."));}
    if(!bBagLayoutInitialized)
    {
        bBagLayoutInitialized=BagLayout.Initialize(Items,!bModernBagLayout);
        if(!bBagLayoutInitialized){UE_LOG(LogTemp,Error,TEXT("Could not place owned inventory within %s; ownership was retained and saving is disabled."),bModernBagLayout?TEXT("five source bags"):TEXT("bounded legacy bag capacity"));}
        else if(BagLayout.HasLegacyOverflow()){UE_LOG(LogTemp,Display,TEXT("Migrated existing native inventory with %d legacy overflow bag pages; new games keep five source bags."),BagLayout.PageCount()-5);}
    }
    BagPage=0;
}

void ANativeWadPlayer::EnsureBagLayout()
{
    if(!bBagLayoutInitialized)
    {bBagLayoutInitialized=BagLayout.Initialize(GetBagItems(),!bModernBagLayout);return;}
    auto Items=GetBagItems();
    if(BagLayout.Num()>Items.Num())
    {UE_LOG(LogTemp,Error,TEXT("Inventory ownership changed without central removal remapping; recovering within recorded capacity."));bBagLayoutInitialized=BagLayout.Initialize(Items,!bModernBagLayout);return;}
    while(BagLayout.Num()<Items.Num()){if(!BagLayout.AddSlot()){return;}}
    for(int32 I=0;I<Items.Num();++I)
    {
        if(Items[I].bOffPage){BagLayout.Detach(I);}
        else if(BagLayout.LeaderOf(I)==INDEX_NONE&&!BagLayout.AutoPlace(I,Items))
        {UE_LOG(LogTemp,Error,TEXT("Unplaced owned item %d was added without a checked bag transaction."),I);}
    }
    BagPage=FMath::Clamp(BagPage,0,BagLayout.PageCount()-1);
}

const FWadBagLayout& ANativeWadPlayer::GetBagLayout() const
{const_cast<ANativeWadPlayer*>(this)->EnsureBagLayout();return BagLayout;}
int32 ANativeWadPlayer::GetBagPage() const
{return FMath::Clamp(BagPage,0,GetBagLayout().PageCount()-1);}
void ANativeWadPlayer::SetBagPage(int32 Page)
{EnsureBagLayout();BagPage=FMath::Clamp(Page,0,BagLayout.PageCount()-1);}

bool ANativeWadPlayer::AppendBagItem(int32 Type,bool bRecorded,int32 AccessoryData)
{
    if(Type<=0||Type>MAX_uint16){return false;}
    EnsureBagLayout();if(!bBagLayoutInitialized||BagLayout.Num()>=FWadBagLayout::MaxSlots){return false;}
    FWadBagLayout Candidate=BagLayout;if(!Candidate.AddSlot()){return false;}
    const int32 Slot=CarriedItems.Num();if(!AppendCarriedObject(Type,bRecorded,AccessoryData)){return false;}
    const auto Items=GetBagItems();
    if(!NativeWadRules::IsRuneItem(Type)&&!Candidate.AutoPlace(Slot,Items))
    {RemoveCarriedObjectAt(Slot);return false;}
    BagLayout=MoveTemp(Candidate);return true;
}

bool ANativeWadPlayer::TryPlaceMouseHandInBag(int32 Page,FVector2D Point)
{
    if(IsDefeated()||!EnsureCarriedObjectData()||!CarriedItems.IsValidIndex(SelectedInventorySlot)
        ||!FMath::IsFinite(Point.X)||!FMath::IsFinite(Point.Y)
        ||Point.X<213||Point.X>315||Point.Y<16||Point.Y>129){return false;}
    EnsureBagLayout();auto Items=GetBagItems();const int32 Slot=SelectedInventorySlot;Items[Slot].bOffPage=false;
    const FIntPoint A=FWadBagLayout::Anchor(Point,{213,16},Items[Slot].Bitmap);
    if(!BagLayout.Place(Slot,Page,A,Items)){return false;}
    SelectedInventorySlot=INDEX_NONE;return true;
}

int32 ANativeWadPlayer::BagItemAt(int32 Page,FVector2D Point) const
{
    if(!FMath::IsFinite(Point.X)||!FMath::IsFinite(Point.Y)||Point.X<213||Point.X>315||Point.Y<16||Point.Y>129){return INDEX_NONE;}
    const auto& Layout=GetBagLayout();const auto Items=GetBagItems();
    return Layout.At(Page,int32(Point.X-213)/17,int32(Point.Y-16)/14,Items);
}

void ANativeWadPlayer::InitializeCreationBagLayout()
{
    BagLayout.Reset();bModernBagLayout=true;bBagLayoutInitialized=BagLayout.Initialize(GetBagItems(),false,true);BagPage=0;
    if(!bBagLayoutInitialized){UE_LOG(LogTemp,Error,TEXT("Original starter inventory did not fit its five source bags."));}
}

#include "NativeWadPlayer.h"
#include "Camera/CameraComponent.h"
#include "Components/InputComponent.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadScenery.h"
#include "Misc/Crc.h"

FString ANativeWadPlayer::GetMagicMapName() const
{ return UGameplayStatics::GetCurrentLevelName(this,true); }

void ANativeWadPlayer::SynchronizeMagicMapOwners()
{
    TMap<FString,int32> Active;
    // Registry order is source activation order; no summed/max-tier selection.
    for(const auto& Effect:TimedEffects.GetEffects())
    {
        if(Effect.Kind==EWadTimedEffectKind::Status&&Effect.Status==NativeWadRules::MagicMapStatus)
        { Active.Add(Effect.Owner,Effect.Power); }
    }
    if(GetReadyItem()==435&&CanUseWadItem(435)) { Active.Add(TEXT("ready:map"),1); }
    for(int32 I=0;I<NativeWadRules::JewelrySlotCount;++I)
    {
        if(CarriedItems.IsValidIndex(JewelrySlots[I])&&CarriedItems[JewelrySlots[I]]==435&&CanUseWadItem(435))
        { Active.Add(FString::Printf(TEXT("jewelry:map:%d"),I),1); }
    }
    MagicMap.SynchronizeOwners(Active);
}

void ANativeWadPlayer::ReadyMagicMapRing()
{
    SynchronizeMagicMapOwners();
    if(GetReadyItem()==435&&CanUseWadItem(435)) { MagicMap.StartOwner(TEXT("ready:map"),1); }
    for(int32 I=0;I<NativeWadRules::JewelrySlotCount;++I)
    {
        if(CarriedItems.IsValidIndex(JewelrySlots[I])&&CarriedItems[JewelrySlots[I]]==435&&CanUseWadItem(435))
        { MagicMap.StartOwner(FString::Printf(TEXT("jewelry:map:%d"),I),1); }
    }
}

void ANativeWadPlayer::MagicMapCommand(FKey Key)
{
    if(auto* PC=GetController<APlayerController>())
    { if(auto* HUD=PC->GetHUD<ANativeWadHUD>();HUD&&HUD->IsNotesOpen()){return;} }
    const FKey Keys[]={EKeys::One,EKeys::Two,EKeys::Three,EKeys::Four,EKeys::Five,EKeys::Six,EKeys::Seven};
    for(int32 I=0;I<UE_ARRAY_COUNT(Keys);++I)
    {
        if(Key!=Keys[I]){continue;}SynchronizeMagicMapOwners();
        if(I==0&&!MagicMap.bOn&&!MagicMap.HasOwners())
        { ShowInteractionMessage(TEXT("Magic map only available with spell"));return; }
        MagicMap.Command(I+1);return;
    }
}

void ANativeWadPlayer::TickMagicMap(float DeltaSeconds)
{
    auto* PC=GetController<APlayerController>();auto* Level=NativeLevel.Get();
    if(!PC||!PC->IsLocalController()||!Level){return;}
    SynchronizeMagicMapOwners();
    const bool bNewTopology=MagicMapLines.Num()!=Level->GetLineCount()||MagicMapGeometryRevision==MAX_uint32;
    if(bNewTopology||MagicMapGeometryRevision!=Level->GetWallGeometryRevision())
    {
        MagicMapLines.SetNum(Level->GetLineCount());
        for(int32 I=0;I<MagicMapLines.Num();++I)
        { Level->GetWallLine(I,MagicMapLines[I].Wall);MagicMapLines[I].Flags=uint16(Level->GetWadLineFlags(I)); }
        // Floor/ceiling motion changes band heights, not source topology or seen-line identity.
        // Rebind/hash only on arrival/restore or a changed topology size.
        if(bNewTopology)
        {
        uint32 Hash=0;
        for(const auto& Entry:MagicMapLines)
        {
            const double Vertices[]={Entry.Wall.From.X,Entry.Wall.From.Y,Entry.Wall.To.X,Entry.Wall.To.Y};
            Hash=FCrc::MemCrc32(Vertices,sizeof(Vertices),Hash);
            const int32 Sectors[]={Entry.Wall.Sectors.X,Entry.Wall.Sectors.Y};Hash=FCrc::MemCrc32(Sectors,sizeof(Sectors),Hash);
            const uint16 Flags=Entry.Flags&~uint16(0x8000);Hash=FCrc::MemCrc32(&Flags,sizeof(Flags),Hash);
        }
        MagicMap.BindTopology(GetMagicMapName(),MagicMapLines.Num(),FString::Printf(TEXT("%08X"),Hash));
        }
        MagicMapGeometryRevision=Level->GetWallGeometryRevision();
    }
    if(MagicMapLines.IsEmpty()){return;}
    const FString Map=GetMagicMapName();
    int32 Width=0,Height=0;PC->GetViewportSize(Width,Height);
    const double Aspect=Height>0?double(Width)/Height:1.6;
    const double Scale=FMath::Min(Width/320.0,Height/200.0);
    const FVector2D Letterbox((Width-320.0*Scale)*0.5,(Height-200.0*Scale)*0.5);
    const FBox2D Crop=Width>0&&Height>0?FBox2D(
        (Letterbox+FVector2D(4,3)*Scale)/FVector2D(Width,Height),
        (Letterbox+FVector2D(bInventoryOpen?205:316,151)*Scale)/FVector2D(Width,Height))
        :FBox2D(FVector2D(0.0125,0.015),FVector2D(bInventoryOpen?0.640625:0.9875,0.755));
    // Twelve analytic projection/occlusion samples per tick, amortized across every line.
    // Source marks each actually drawn column; subpixel slits and masked-texel occlusion
    // remain a bounded approximation, not renderer visibility or a proximity reveal.
    for(int32 Budget=0;Budget<12;++Budget)
    {
        if(MagicMapObserverLine>=MagicMapLines.Num())
        { MagicMapObserverLine=0;MagicMapObserverSample=(MagicMapObserverSample+1)%54; }
        const int32 I=MagicMapObserverLine++;
        if(MagicMap.IsSeen(Map,I)){continue;}
        if(FWadMagicMap::CanObserve(MagicMapLines,I,GetPawnViewLocation()-Level->GetActorLocation(),
            GetControlRotation(),ViewCamera->FieldOfView,Aspect,MagicMapObserverSample,Crop))
        { MagicMap.Observe(Map,I); }
    }
}

TArray<FWadMagicMap::FMarker> ANativeWadPlayer::GetMagicMapMarkers() const
{
    TArray<FWadMagicMap::FMarker> Markers;TSet<const ANativeWadPickup*> SceneryItems;
    const auto* Level=NativeLevel.Get();if(!Level){return Markers;}
    const FVector Origin=Level->GetActorLocation();
    for(TActorIterator<ANativeWadScenery> It(GetWorld());It;++It)
    {
        auto* Item=It->GetPickup();
        if(Item){SceneryItems.Add(Item);} // One source object: hidden sprite proxies must never reappear as independent markers.
        if(It->IsHidden()||(Item&&!Item->IsAvailable())){continue;}
        Markers.Add({FVector2D(It->GetActorLocation()-Origin),double(It->GetMagicMapRadius()),false,It->IsMagicMapInvisible(),false});
    }
    for(TActorIterator<ANativeWadPickup> It(GetWorld());It;++It)
    {
        if(It->IsHidden()||SceneryItems.Contains(*It)){continue;}
        Markers.Add({FVector2D(It->GetActorLocation()-Origin),It->GetMagicMapRadius(),false,It->IsMagicMapInvisible(),false});
    }
    for(TActorIterator<ANativeWadGuardian> It(GetWorld());It;++It)
    {
        if(It->IsHidden()){continue;}
        Markers.Add({FVector2D(It->GetActorLocation()-Origin),It->GetMagicMapRadius(),true,false,true});
    }
    return Markers;
}

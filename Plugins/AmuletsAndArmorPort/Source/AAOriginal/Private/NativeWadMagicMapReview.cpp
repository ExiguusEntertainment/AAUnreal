#include "NativeWadReview.h"
#include "NativeWadPlayer.h"
#include "NativeWadLevel.h"
#include "NativeWadInventoryView.h"
#include "NativeWadHudData.h"
#include "Algo/Find.h"
#include "Components/CapsuleComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/GenericPlatformInputDeviceMapper.h"
#include "Kismet/GameplayStatics.h"
#include "HAL/FileManager.h"
#include "InputCoreTypes.h"
#include "InputKeyEventArgs.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "UnrealClient.h"

namespace
{
    struct FMagicMapReview
    {
        FName Context;
        FString Output;
        bool bReload=false;
        int32 Step=0,RuneIndex=0,Frames=0,TargetLine=INDEX_NONE,DoorSector=INDEX_NONE;
        int32 BeforeMana=0;
        double Started=FPlatformTime::Seconds(),Deadline=0,MissingSince=0;
        bool bImageFailed=false;
        FVector2D DoorCenter=FVector2D::ZeroVector;
        FRotator DoorView=FRotator::ZeroRotator;
        TOptional<FKey> Held;
        FString SourceMap;
        static FString Manifest(){return FPaths::ProjectSavedDir()/TEXT("ReviewArtifacts/MagicMap-fixture.txt");}
        void Key(APlayerController& PC,FKey K,EInputEvent Event)
        {PC.InputKey(FInputKeyEventArgs(nullptr,IPlatformInputDeviceMapper::Get().GetDefaultInputDevice(),K,Event,FPlatformTime::Cycles64()));}
        void Press(APlayerController& PC,FKey K){Key(PC,K,IE_Pressed);Held=K;}
        bool Finish(APlayerController* PC,const TCHAR* Error=nullptr)
        {
            if(PC&&Held){Key(*PC,Held.GetValue(),IE_Released);Held.Reset();}
            if(Error){UE_LOG(LogTemp,Error,TEXT("MagicMap rendered fixture failed at step%d: %s"),Step,Error);}
            else{UE_LOG(LogTemp,Display,TEXT("Verified MagicMap source controls, individual tier owners, door exploration and native persistence. Controlled fixture only; no campaign reward or original MapSave claim."));}
            UE_LOG(LogTemp,Display,TEXT("Native WAD port review finished"));return false;
        }
        bool Click(ANativeWadHUD& HUD,APlayerController& PC,FVector2D P)
        {
            int32 W=0,H=0;PC.GetViewportSize(W,H);const double S=FMath::Min(W/320.0,H/200.0);if(S<=0){return false;}
            HUD.ClickHUD((FVector2D(W,H)-FVector2D(320,200)*S)*0.5+P*S);return true;
        }
        bool Capture(const TCHAR* Name)
        {
            const FString File=FPaths::GetPath(Output)/(FPaths::GetBaseFilename(Output)+TEXT("-")+Name+TEXT(".png"));
            if(Frames++==0){IFileManager::Get().Delete(*File);FScreenshotRequest::RequestScreenshot(File,true,false);}
            if(Frames<20){return false;}Frames=0;
            if(!IFileManager::Get().FileExists(*File)){bImageFailed=true;UE_LOG(LogTemp,Error,TEXT("Missing MagicMap rendered capture: %s"),*File);return false;}
            return true;
        }
        bool Rune(APlayerController& PC,const TCHAR* Code)
        {
            const FKey Keys[]={EKeys::NumPadOne,EKeys::NumPadTwo,EKeys::NumPadThree,EKeys::NumPadFour,EKeys::NumPadFive,
                EKeys::NumPadSix,EKeys::NumPadSeven,EKeys::NumPadEight,EKeys::NumPadNine};
            if(Code[RuneIndex]){Press(PC,Keys[Code[RuneIndex++]-TEXT('1')]);return false;}
            Press(PC,EKeys::RightMouseButton);RuneIndex=0;return true;
        }
        bool TakeRing(ANativeWadPlayer& P,ANativeWadHUD& HUD,APlayerController& PC)
        {
            const auto* Catalog=UNativeWadHudData::LoadCatalog();if(!Catalog){return false;}
            Click(HUD,PC,{13,161});
            if(!P.IsInventoryOpen()){Click(HUD,PC,{13,161});}
            const int32 Slot=P.GetCarriedItems().Find(435);
            const NativeWadInventoryView::FView Bag(P,*Catalog,true,P.GetMouseHandSlot());
            const int32 Cell=Bag.CellForSlot(Slot);if(Cell<0||Cell>=NativeWadInventoryView::SlotsPerPage){return false;}
            return Click(HUD,PC,NativeWadInventoryView::SlotBox(Cell).GetCenter())&&P.GetMouseHandItem()==435;
        }
        bool EquipRing(ANativeWadPlayer& P,ANativeWadHUD& HUD,APlayerController& PC)
        {
            if(P.GetMouseHandItem()!=435&&!TakeRing(P,HUD,PC)){return false;}
            // Normalize through the real Inventory button: re-equipping starts
            // with Equipment already open after the preceding unREADY click.
            Click(HUD,PC,{13,161});
            if(!P.IsInventoryOpen()){Click(HUD,PC,{13,161});}
            Click(HUD,PC,{30,161});Click(HUD,PC,NativeWadInventoryView::JewelryBoxes[NativeWadRules::Ring1Slot].GetCenter());
            Click(HUD,PC,{310,9});return P.GetMouseHandItem()==0&&P.HasEquippedItem(435);
        }
        bool SelectDoorFixture(ANativeWadLevel& Level,ANativeWadPlayer& P,APlayerController& PC)
        {
            using M=FWadMagicMap;
            // Only fixture selection changes a ceiling synchronously and restores it
            // before returning to the game loop. Actual observation/opening below uses E.
            const auto Snapshot=[&]
            {
                TArray<M::FLine> Result;Result.SetNum(Level.GetLineCount());
                for(int32 I=0;I<Result.Num();++I){Level.GetWallLine(I,Result[I].Wall);Result[I].Flags=uint16(Level.GetWadLineFlags(I));}
                return Result;
            };
            const FString Name=P.GetMagicMapName();
            for(const auto& Door:Level.GetDoors())
            {
                if(Door.Sector!=147||Level.GetDoorLockStrength(Door.Sector)!=0){continue;}
                FVector2D Heights;if(!Level.GetSectorSurfaceHeights(Door.Sector,Heights)||!FMath::IsNearlyEqual(Heights.Y,double(Door.ClosedZ),0.1)){continue;}
                const auto Closed=Snapshot();Level.SetSectorCeilingZ(Door.Sector,Door.OpenZ);
                const auto Open=Snapshot();Level.SetSectorCeilingZ(Door.Sector,Heights.Y);
                for(const auto& Boundary:Closed)
                {
                    if(Boundary.Wall.Sectors.X!=Door.Sector&&Boundary.Wall.Sectors.Y!=Door.Sector){continue;}
                    const FVector2D Edge=Boundary.Wall.To-Boundary.Wall.From;if(Edge.Size()<20){continue;}
                    const FVector2D Middle=(Boundary.Wall.From+Boundary.Wall.To)*0.5;
                    const FVector2D Normal=FVector2D(-Edge.Y,Edge.X).GetSafeNormal();
                    for(int32 Sign:{-1,1})
                    {
                        const FVector2D XY=Middle+Normal*(40.0*Sign);const int32 Sector=Level.FindWadSector(XY);
                        if(Sector==Door.Sector||!Level.GetSectors().IsValidIndex(Sector)){continue;}
                        FVector2D NeighborHeights;if(!Level.GetSectorSurfaceHeights(Sector,NeighborHeights)
                            ||NeighborHeights.Y-NeighborHeights.X<2*ANativeWadPlayer::CapsuleHalfHeight+4){continue;}
                        const FVector Eye(XY,Level.GetWalkingHeight(Sector)+ANativeWadPlayer::CapsuleHalfHeight+2+22);
                        const FRotator View=FVector(Middle-XY,0).Rotation();
                        for(int32 I=0;I<Open.Num();++I)
                        {
                            if(Open[I].Wall.Surfaces.Num()!=1||(Open[I].Flags&0x8180)||P.GetMagicMap().IsSeen(Name,I)){continue;}
                            bool bClosed=false,bOpen=true;
                            for(int32 Sample=0;Sample<9;++Sample)
                            {
                                bClosed|=M::CanObserve(Closed,I,Eye,View,90,1.6,Sample);
                                bOpen&=M::CanObserve(Open,I,Eye,View,90,1.6,Sample,
                                    FBox2D(FVector2D(0.0125,0.015),FVector2D(0.9875,0.755)));
                                if(bClosed||!bOpen){break;}
                            }
                            if(bClosed||!bOpen){continue;}
                            P.SetActorLocation(Level.GetActorLocation()+FVector(XY,Eye.Z-22),false,nullptr,ETeleportType::TeleportPhysics);
                            P.GetCharacterMovement()->StopMovementImmediately();PC.SetControlRotation(View);
                            DoorCenter=Middle+FVector2D(Level.GetActorLocation());DoorView=View;DoorSector=Door.Sector;TargetLine=I;SourceMap=Name;
                            UE_LOG(LogTemp,Display,TEXT("MagicMap controlled camera fixture: door sector%d, target line%d, eye%s; closed/open snapshots restored before player tick. NPCs remain active."),DoorSector,TargetLine,*Eye.ToString());
                            return true;
                        }
                    }
                }
            }
            return false;
        }
        bool Tick()
        {
            const auto* WC=GEngine->GetWorldContextFromHandle(Context);UWorld* World=WC?WC->World():nullptr;
            auto* P=World?Cast<ANativeWadPlayer>(UGameplayStatics::GetPlayerPawn(World,0)):nullptr;
            auto* PC=P?P->GetController<APlayerController>():nullptr;
            auto* HUD=PC?PC->GetHUD<ANativeWadHUD>():nullptr;
            if(!P||!PC||!HUD)
            {const double Now=FPlatformTime::Seconds();if(MissingSince==0){MissingSince=Now;}return Now-MissingSince<30?true:Finish(PC,TEXT("no normal player/classic HUD"));}
            MissingSince=0;
            if(bImageFailed){return Finish(PC,TEXT("rendered screenshot was not written"));}
            if(Held){Key(*PC,Held.GetValue(),IE_Released);Held.Reset();}
            if(FPlatformTime::Seconds()-Started>240){return Finish(PC,TEXT("timed out"));}
            if(P->IsDefeated()){return Finish(PC,TEXT("fixture character defeated; no NPC freezing is used"));}
            const auto& Map=P->GetMagicMap();const double Now=FPlatformTime::Seconds();
            if(bReload)
            {
                if(!P->HasRestoredWadCheckpoint()){return true;}
                TArray<FString> Fields;
                if(!FFileHelper::LoadFileToStringArray(Fields,*Manifest())||Fields.Num()!=3||!LexTryParseString(TargetLine,*Fields[1]))
                {return Finish(PC,TEXT("missing explicit prior-process fixture manifest"));}
                SourceMap=Fields[0];FWadMagicMap Expected;if(!Expected.Decode(Fields[2])){return Finish(PC,TEXT("invalid review manifest map state"));}
                if(!World->GetMapName().EndsWith(TEXT("L301"))||!Map.bOn||Map.Tier!=1||Map.Features!=Expected.Features
                    ||Map.Position!=Expected.Position||Map.OwnerCount()!=2||P->GetOwnedEffectCount()!=1||!P->HasEquippedItem(435)
                    ||P->GetItemCount(435)!=1||!Map.IsSeen(SourceMap,TargetLine))
                {return Finish(PC,TEXT("new process lost source controls, ring/timed owner, or L30 exploration through travel"));}
                if(!Capture(TEXT("restored"))){return true;}return Finish(PC);
            }
            switch(Step)
            {
            case 0:
                if(Map.HasOwners()||Map.bOn||P->GetCharacterClass()!=NativeWadRules::MageClass||P->GetMana()!=30000)
                {return Finish(PC,TEXT("fresh fixture needs Mage,30000mana,no active effects"));}
                UE_LOG(LogTemp,Display,TEXT("Starting provisioned MagicMap UI/door fixture, not an earned campaign character. Grants original runes300..308 and ring435; no XP/quest rewards, no NPC changes."));
                for(int32 Type=300;Type<=308;++Type){if(!P->GetItemCount(Type)){P->CollectItem(Type);}}
                if(!P->GetItemCount(435)){P->CollectItem(435);}Press(*PC,EKeys::One);++Step;return true;
            case 1:
                if(Map.bOn||P->GetInteractionMessage()!=TEXT("Magic map only available with spell")){return Finish(PC,TEXT("source no-effect map refusal"));}
                if(!EquipRing(*P,*HUD,*PC)){return Finish(PC,TEXT("source bag/equipment ring placement"));}++Step;return true;
            case 2:
                if(!Map.bOn||Map.Tier!=1||Map.OwnerCount()!=1){return Finish(PC,TEXT("ring READY tier1"));}Press(*PC,EKeys::Two);++Step;return true;
            case 3:
                if(Map.Features!=FWadMagicMap::Translucent){return Finish(PC,TEXT("source row2 rotate"));}Press(*PC,EKeys::Three);++Step;return true;
            case 4:
                if(Map.Features!=0){return Finish(PC,TEXT("source row3 translucent"));}Press(*PC,EKeys::Four);++Step;return true;
            case 5:
                if(Map.Features!=FWadMagicMap::Transparent){return Finish(PC,TEXT("source row4 transparency"));}Press(*PC,EKeys::Seven);++Step;return true;
            case 6:
                if(Map.Position!=2){return Finish(PC,TEXT("source row7 six-position cycle"));}
                if(!Capture(TEXT("tier1-controls"))){return true;}Press(*PC,EKeys::One);++Step;return true;
            case 7:
                if(Map.bOn){return Finish(PC,TEXT("source row1 hide"));}Press(*PC,EKeys::Two);++Step;return true;
            case 8:
                if(Map.Features!=FWadMagicMap::Transparent){return Finish(PC,TEXT("hidden map accepted rotate input"));}
                BeforeMana=P->GetMana();if(Rune(*PC,TEXT("4967"))){++Step;}return true;
            case 9:
                if(Map.Tier!=4||Map.OwnerCount()!=2||P->GetMana()!=BeforeMana-12000||Map.Features!=79||!Map.bOn)
                {return Finish(PC,TEXT("tier4 source cast/charge/reset"));}
                if(!Capture(TEXT("tier4-all-walls"))){return true;}
                Click(*HUD,*PC,{30,161});Click(*HUD,*PC,NativeWadInventoryView::JewelryBoxes[NativeWadRules::Ring1Slot].GetCenter());
                if(P->GetMouseHandItem()!=435){return Finish(PC,TEXT("source ring unREADY"));}++Step;return true;
            case 10:
                if(Map.Tier!=4||Map.OwnerCount()!=1){return Finish(PC,TEXT("unREADY recomputed surviving tier"));}
                if(!EquipRing(*P,*HUD,*PC)){return Finish(PC,TEXT("ring reREADY placement"));}++Step;return true;
            case 11:
                if(Map.Tier!=1||Map.Features!=72||Map.OwnerCount()!=2){return Finish(PC,TEXT("ring did not downgrade last-started tier"));}
                BeforeMana=P->GetMana();if(Rune(*PC,TEXT("4967"))){++Step;}return true;
            case 12:
                if(Map.Tier!=4||Map.OwnerCount()!=2||P->GetMana()!=BeforeMana-12000){return Finish(PC,TEXT("same-owner recast/charge"));}
                BeforeMana=P->GetMana();Step=120;return true;
            case 120:
                if(Rune(*PC,TEXT("436"))){Step=13;}return true;
            case 13:
                if(Map.Tier!=2||Map.OwnerCount()!=3||P->GetOwnedEffectCount()!=2||P->GetMana()!=BeforeMana-4000)
                {return Finish(PC,TEXT("distinct tier2 owner was summed/maxed"));}
                P->ReceiveTypedHit(1,NativeWadRules::SpecialDamageFlag(NativeWadRules::ESpecialDamage::Dispel));++Step;return true;
            case 14:
                if(Map.Tier!=1||Map.OwnerCount()!=2||P->GetOwnedEffectCount()!=1||Map.Features!=72)
                {return Finish(PC,TEXT("incoming Dispel fixture did not reREADY ring and remove exactly one timed owner"));}
                if(!Capture(TEXT("dispel-ring-ready"))){return true;}
                {TActorIterator<ANativeWadLevel> L(World);if(!L||!SelectDoorFixture(**L,*P,*PC)){return Finish(PC,TEXT("no valid unseen authored door sightline fixture"));}}
                Deadline=Now+2; ++Step;return true;
            case 15:
                if(Map.IsSeen(SourceMap,TargetLine)){return Finish(PC,TEXT("closed textured door revealed its occluded target"));}
                if(Now<Deadline){return true;}if(!Capture(TEXT("door-closed-unseen"))){return true;}
                Press(*PC,EKeys::E);Deadline=Now+60;++Step;return true;
            case 16:
                if(!Map.IsSeen(SourceMap,TargetLine))
                {
                    if(Now>Deadline){return Finish(PC,TEXT("normal door opening did not reveal target through observer"));}
                    TActorIterator<ANativeWadLevel> L(World);FVector2D H;
                    if(L&&L->GetSectorSurfaceHeights(DoorSector,H))
                    {
                        const auto* D=Algo::FindByPredicate(L->GetDoors(),[&](const auto& Entry){return Entry.Sector==DoorSector;});
                        if(D&&FMath::IsNearlyEqual(H.Y,double(D->ClosedZ),0.1)){Press(*PC,EKeys::E);}
                    }
                    return true;
                }
                UE_LOG(LogTemp,Display,TEXT("Actual native observer marked previously unseen L30 line%d only after normal E opened authored door%d, while source ring tier1 was active."),TargetLine,DoorSector);
                if(!Capture(TEXT("door-open-explored"))){return true;}Press(*PC,EKeys::Two);++Step;return true;
            case 17:Press(*PC,EKeys::Three);++Step;return true;
            case 18:Press(*PC,EKeys::Four);++Step;return true;
            case 19:Press(*PC,EKeys::Seven);++Step;return true;
            case 20:
                if(Map.Features!=16||Map.Position!=3||!Map.IsSeen(SourceMap,TargetLine)){return Finish(PC,TEXT("prepared source control/exploration save state"));}
                IFileManager::Get().MakeDirectory(*FPaths::GetPath(Manifest()),true);
                if(!FFileHelper::SaveStringToFile(SourceMap+TEXT("\n")+FString::FromInt(TargetLine)+TEXT("\n")+Map.Encode()+TEXT("\n"),*Manifest()))
                {return Finish(PC,TEXT("could not save explicit review fixture manifest"));}
                P->TravelToWadLevel(TSoftObjectPtr<UWorld>(FSoftObjectPath(TEXT("/Game/Maps/L301.L301"))));++Step;return true;
            case 21:
                if(!World->GetMapName().EndsWith(TEXT("L301"))||!P->GetCharacterMovement()->IsMovingOnGround()){return true;}
                if(Map.Features!=16||Map.Position!=3||Map.Tier!=1||Map.OwnerCount()!=2||!Map.IsSeen(SourceMap,TargetLine))
                {return Finish(PC,TEXT("normal L30 to L301 travel lost character map state"));}
                if(!P->SaveWadCheckpoint(TEXT("NativeWadMagicMapReview"))){return Finish(PC,TEXT("could not checkpoint native map state"));}
                ++Step;return true;
            case 22:
                if(!Capture(TEXT("travel-saved"))){return true;}return Finish(PC);
            }
            return true;
        }
    };
}

bool StartNativeWadMagicMapReview(UWorld* World,const FString& OutputPath,bool bReload)
{
    if(!World||!World->IsGameWorld()||FParse::Param(FCommandLine::Get(),TEXT("nullrhi"))
        ||!World->GetMapName().EndsWith(bReload?TEXT("L301"):TEXT("L30"))){return false;}
    const auto* Context=GEngine->GetWorldContextFromWorld(World);if(!Context){return false;}
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutputPath),true);
    auto Review=MakeShared<FMagicMapReview>();Review->Context=Context->ContextHandle;Review->Output=OutputPath;Review->bReload=bReload;
    FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Review](float){return Review->Tick();}));return true;
}

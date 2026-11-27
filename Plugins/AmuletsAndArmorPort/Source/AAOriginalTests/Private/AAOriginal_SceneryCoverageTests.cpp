#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Components/BoxComponent.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/World.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/PlayerController.h"
#include "Materials/Material.h"
#include "NativeWadEncounter.h"
#include "NativeWadLevel.h"
#include "NativeWadScenery.h"
#include "NativeWadScenerySource.h"
#include "UObject/UnrealType.h"

namespace
{
    bool RawThings(const FString& Name, TArray<uint16>& Types)
    {
        TArray<uint8> Bytes;
        if (!FFileHelper::LoadFileToArray(Bytes, *(FPaths::ProjectDir() /
            TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe") / (Name + TEXT(".MAP")))) || Bytes.Num() < 12) { return false; }
        const auto U32 = [&](int32 At) { return uint32(Bytes[At]) | uint32(Bytes[At+1]) << 8 | uint32(Bytes[At+2]) << 16 | uint32(Bytes[At+3]) << 24; };
        const uint32 Count = U32(4), Directory = U32(8);
        if (uint64(Directory) + uint64(Count)*16 > uint64(Bytes.Num())) { return false; }
        for (uint32 I=0; I<Count; ++I)
        {
            const uint32 Entry = Directory + I*16;
            if (FMemory::Memcmp(Bytes.GetData()+Entry+8, "THINGS\0\0", 8)) { continue; }
            const uint32 At=U32(Entry), Size=U32(Entry+4);
            if (Size%10 || uint64(At)+Size > uint64(Bytes.Num())) { return false; }
            for (uint32 J=At; J<At+Size; J+=10) { Types.Add(uint16(Bytes[J+6]) | uint16(Bytes[J+7])<<8); }
            return true;
        }
        return false;
    }
    FWadSceneryDefinition DefinitionFrom(const FWadScenerySource& Source)
    {
        FWadSceneryDefinition D;
        D.Radius=Source.Radius; D.SourceAttributes=Source.Attributes;
        D.bPassable=(Source.Attributes & 0x2001)!=0 || Source.Radius==0;
        D.bHanging=(Source.MoveAttributes & 0x0200)!=0;
        D.bRaised=(Source.MoveAttributes & 0x0400)!=0;
        D.bStaysAboveLiquid=(Source.MoveAttributes & 0x0100)!=0;
        D.FrameTicks=Source.FrameTicks; D.AnimationType=Source.AnimationType;
        for (const auto& Frame:Source.Frames)
        {
            auto& F=D.Frames.AddDefaulted_GetRef();
            for (const int32 Id:Frame.Pictures)
            {
                auto& V=F.Views.AddDefaulted_GetRef();
                V.Size=FIntPoint(32,80); V.bMirrored=Id<0;
                V.Material=UMaterial::GetDefaultMaterial(MD_Surface);
            }
        }
        return D;
    }
    struct FSceneryFixture
    {
        UWorld* World=nullptr;
        ANativeWadLevel* Level=nullptr;
        APlayerController* Controller=nullptr;
        UStaticMesh* Plane=nullptr;
        FSceneryFixture()
        {
            World=UWorld::CreateWorld(EWorldType::Game,false);
            if(!World){return;}
            World->SetGameInstance(NewObject<UGameInstance>());
            Level=World->SpawnActor<ANativeWadLevel>();
            Controller=World->SpawnActor<APlayerController>();
            Plane=LoadObject<UStaticMesh>(nullptr,TEXT("/Engine/BasicShapes/Plane.Plane"));
            if(!Level||!Controller||!Plane){return;}
            World->AddController(Controller);
            auto* Camera=World->SpawnActor<AActor>(FVector(200,0,120),FRotator::ZeroRotator);
            Controller->SetViewTarget(Camera);
            const FArrayProperty* P=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Sectors"));
            if(!P){Level=nullptr;return;}
            auto& Sector=P->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(Level)->AddDefaulted_GetRef();
            const auto Surface=[&](double Z)
            {
                auto* C=NewObject<UStaticMeshComponent>(Level);
                C->SetMobility(EComponentMobility::Movable); C->SetupAttachment(Level->GetRootComponent());
                C->RegisterComponent(); C->SetRelativeLocation(FVector(0,0,Z)); return C;
            };
            Sector.Floor=Surface(0); Sector.Ceiling=Surface(200);
        }
        ~FSceneryFixture(){if(World){World->DestroyWorld(false);}}
        bool Valid() const{return World&&Level&&Controller&&Plane;}
        ANativeWadScenery* Spawn(const FWadSceneryDefinition& D,int32 Id=17)
        {
            auto* Result=World->SpawnActor<ANativeWadScenery>();
            if(Result){Result->Configure(*Level,Plane,D,0,Id);Result->DispatchBeginPlay();}
            return Result;
        }
        bool CapsuleHit(double Z) const
        {
            FHitResult Hit;
            return World->SweepSingleByChannel(Hit,FVector(-100,0,Z),FVector(100,0,Z),FQuat::Identity,ECC_Pawn,
                FCollisionShape::MakeCapsule(8,16));
        }
    };
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalScenerySourceCoverage,"AAOriginal.WAD.Import.OrdinaryScenerySource",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalScenerySourceCoverage::RunTest(const FString& Parameters)
{
    TMap<int32,FString> Resources; FString Error;
    if(!TestTrue(TEXT("Load exact original OBJS/CREDATA/INVDESC classification"),NativeWadScenerySource::LoadOrdinaryCatalog(Resources,Error))){AddError(Error);return false;}
    for(const int32 Excluded:{0,1,2,3,4,31,88,89,924,1504,1520,33787,33803,37886,46075,54268,803,807,244,115})
    {TestFalse(*FString::Printf(TEXT("Exact gameplay/item/marker type%d cannot become scenery"),Excluded),Resources.Contains(Excluded));}
    int32 Ordinary=0,Supported=0,Missing=0;TSet<int32> Types;TSet<int32> Validated;
    const TCHAR* Maps[]={TEXT("L30"),TEXT("L301"),TEXT("L531"),TEXT("L31"),TEXT("L22"),TEXT("L25"),TEXT("L20"),TEXT("L60"),TEXT("L64"),TEXT("L65"),TEXT("L34"),TEXT("L151"),TEXT("L152"),TEXT("L153"),TEXT("L231"),TEXT("L75"),TEXT("L10"),TEXT("L12"),TEXT("L51"),TEXT("L52"),TEXT("L41"),TEXT("L44"),TEXT("L74"),TEXT("L73"),TEXT("L72"),TEXT("L32"),TEXT("L71"),TEXT("L70"),TEXT("L61"),TEXT("L40")};
    for(const TCHAR* Name:Maps)
    {
        TArray<uint16> Things;
        if(!TestTrue(*FString::Printf(TEXT("Independent raw THINGS %s"),Name),RawThings(Name,Things))){return false;}
        for(const uint16 Type:Things)
        {
            if(!Resources.Contains(Type)){continue;}
            ++Ordinary;Types.Add(Type);
            if(!Validated.Contains(Type))
            {
                FWadScenerySource Source;
                const bool Loaded=NativeWadScenerySource::Load(Resources.FindChecked(Type),Source,Error);
                if(Type==834){TestFalse(TEXT("ELMARK translucency is explicit, not silently rendered as masked"),Loaded);}
                else if(!TestTrue(*FString::Printf(TEXT("Initial authored stance has valid CPC art: type%d %s"),Type,*Error),Loaded)){return false;}
                Validated.Add(Type);
            }
            if(Type==834){++Missing;}else{++Supported;}
        }
    }
    TestEqual(TEXT("All30 independent ordinary placements"),Ordinary,3382);
    TestEqual(TEXT("Only five translucent ELMARK placements remain explicitly unsupported"),Missing,5);
    TestEqual(TEXT("Supported ordinary placement count, not a claim about saved actors"),Supported,3377);
    TestEqual(TEXT("Exact ordinary types across all30"),Types.Num(),99);
    FWadScenerySource Bone,Tree,Torch,Hanging;
    if(!TestTrue(TEXT("Two-stance Bone Throne initial stance0 is parsed with correct header offset"),NativeWadScenerySource::Load(TEXT("BONETHR"),Bone,Error))){return false;}
    TestEqual(TEXT("Bone Throne retains two authored stances without flattening"),Bone.StanceCount,2);
    TestEqual(TEXT("Bone Throne starts at original picture0"),Bone.Frames[0].Pictures[0],0);
    if(!TestTrue(TEXT("Tree directional source"),NativeWadScenerySource::Load(TEXT("TREE8"),Tree,Error))){return false;}
    TestEqual(TEXT("Tree8 has all eight directional views"),Tree.Frames[0].Pictures.Num(),8);
    if(!TestTrue(TEXT("Torch source"),NativeWadScenerySource::Load(TEXT("TORCH1"),Torch,Error))){return false;}
    TestEqual(TEXT("Torch has four random source frames"),Torch.Frames.Num(),4);
    TestEqual(TEXT("Torch source tick interval"),Torch.FrameTicks,5);
    TestEqual(TEXT("Torch original random mode"),Torch.AnimationType,2);
    if(!TestTrue(TEXT("Stal9 hanging source"),NativeWadScenerySource::Load(TEXT("STAL9"),Hanging,Error))){return false;}
    TestTrue(TEXT("Source hanging flag is preserved"),(Hanging.MoveAttributes&0x0200)!=0);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSceneryCollision,"AAOriginal.WAD.Gameplay.OrdinarySceneryCollision",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalSceneryCollision::RunTest(const FString& Parameters)
{
    FSceneryFixture F;if(!TestTrue(TEXT("Real world, components and capsule sweep fixture"),F.Valid())){return false;}
    FWadScenerySource Source; FString Error;
    if(!NativeWadScenerySource::Load(TEXT("BARREL1"),Source,Error)){AddError(Error);return false;}
    auto D=DefinitionFrom(Source);auto* Prop=F.Spawn(D);
    if(!TestNotNull(TEXT("Solid original barrel prop"),Prop)){return false;}
    TestTrue(TEXT("Actual pawn capsule sweep blocks on source-radius obstacle"),F.CapsuleHit(40));
    auto* Obstacle=Prop->FindComponentByClass<UBoxComponent>();
    TestEqual(TEXT("Collision keeps original radius22"),Obstacle->GetUnscaledBoxExtent().X,22.0);
    TestEqual(TEXT("Bottom-rooted sprite is centered half a frame above floor"),Prop->FindComponentByClass<UStaticMeshComponent>()->GetRelativeLocation().Z,40.0);
    D.bPassable=true;Prop->Configure(*F.Level,F.Plane,D,0,17);
    TestFalse(TEXT("Passable source prop allows same capsule sweep"),F.CapsuleHit(40));
    D.bPassable=false;D.bHanging=true;Prop->Configure(*F.Level,F.Plane,D,0,17);
    TestFalse(TEXT("Hanging object leaves floor-level path clear"),F.CapsuleHit(40));
    TestTrue(TEXT("Hanging object blocks actual capsule at ceiling mount"),F.CapsuleHit(160));
    TestEqual(TEXT("Hanging top is rooted at source ceiling200"),Prop->GetActorLocation().Z,200.0);
    TestEqual(TEXT("Hanging sprite extends downward"),Prop->FindComponentByClass<UStaticMeshComponent>()->GetRelativeLocation().Z,-40.0);
    D.SourceAttributes=0x0004;Prop->Configure(*F.Level,F.Plane,D,0,17);
    TestFalse(TEXT("Original invisible flag hides sprite independently of collision"),Prop->FindComponentByClass<UStaticMeshComponent>()->IsVisible());
    TestTrue(TEXT("Invisible solid source object still blocks"),F.CapsuleHit(160));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSceneryRefresh,"AAOriginal.WAD.Gameplay.OrdinarySceneryRefresh",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalSceneryRefresh::RunTest(const FString& Parameters)
{
    FSceneryFixture F;if(!F.Valid()){return false;}
    FWadScenerySource Source;FString Error;
    if(!NativeWadScenerySource::Load(TEXT("CANDAB1"),Source,Error)){AddError(Error);return false;}
    auto D=DefinitionFrom(Source);auto* Prop=F.Spawn(D);
    if(!Prop){return false;}
    auto* Sprite=Prop->FindComponentByClass<UStaticMeshComponent>();
    auto* Custom=NewObject<UMaterial>();Sprite->SetMaterial(0,Custom);
    auto* Pickup=F.World->SpawnActor<ANativeWadPickup>();Prop->SetPickup(Pickup);
    Prop->Tags.Add(TEXT("WAD Scenery Thing 17"));Prop->Tags.Add(TEXT("User tag"));
    Prop->SetActorTransform(FTransform(FRotator(0,25,0),FVector(7,9,13),FVector(1.2,1.2,1.2)));
    const FTransform Edited=Prop->GetActorTransform();const auto Tags=Prop->Tags;
    Prop->Configure(*F.Level,F.Plane,D,0,17,true,294);
    TestTrue(TEXT("Targeted reconfigure preserves edited actor transform"),Prop->GetActorTransform().Equals(Edited));
    TestTrue(TEXT("Custom valid material is preserved"),Sprite->GetMaterial(0)==Custom);
    TestTrue(TEXT("Attached pickup identity survives"),Prop->GetPickup()==Pickup);
    TestTrue(TEXT("Original primary and user tags are unchanged"),Prop->Tags==Tags);
    Prop->Tick(5.0/70.0+0.0001);
    TestEqual(TEXT("Source two-frame ordered animation advances at five ticks"),Prop->GetAnimationFrame(),1);
    TestTrue(TEXT("Animation does not replace custom material"),Sprite->GetMaterial(0)==Custom);
    TestTrue(TEXT("Billboarding preserves edited root yaw and root Z"),Prop->GetActorTransform().Equals(Edited));
    TestTrue(TEXT("Sprite facing remains horizontal despite elevated eye"),FMath::IsNearlyZero(Sprite->GetUpVector().Z));
    const FArrayProperty* P=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Sectors"));
    auto& Sectors=*P->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(F.Level);
    Sectors[0].Floor->SetRelativeLocation(FVector(0,0,20));Prop->Tick(0);
    TestEqual(TEXT("Edited Z offset follows sector floor movement"),Prop->GetActorLocation().Z,33.0);
    Prop->SetActorLocation(FVector(7,9,38));Prop->Tick(0);
    TestEqual(TEXT("A later deliberate editor Z change is retained too"),Prop->GetActorLocation().Z,38.0);
    Sectors[0].Floor->SetRelativeLocation(FVector(0,0,25));Prop->Tick(0);
    TestEqual(TEXT("Later source movement retains the new offset"),Prop->GetActorLocation().Z,43.0);
    return true;
}


IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSceneryOriginalMount,"AAOriginal.WAD.Import.OrdinarySceneryOriginalMount",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalSceneryOriginalMount::RunTest(const FString& Parameters)
{
    const FString Filename=FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe/L30.MAP");
    TArray<uint8> Bytes;FString Error;int32 Sector=INDEX_NONE;bool bFallback=false;
    if(!TestTrue(TEXT("Read original L30 including BLOCKMAP"),FFileHelper::LoadFileToArray(Bytes,*Filename))){return false;}
    TestTrue(TEXT("Exact original outside-polygon tree resolves without moving it"),
        NativeWadScenerySource::ResolveMountSector(Bytes,FIntPoint(2360,3188),Sector,bFallback,Error));
    TestEqual(TEXT("Source row22->25, line760/front sidedef1078 resolves sector120"),Sector,120);
    TestFalse(TEXT("The authored tree uses a real original lookup rather than fallback0"),bFallback);
    TestTrue(TEXT("Genuinely outside BLOCKMAP uses original setup fallback0"),
        NativeWadScenerySource::ResolveMountSector(Bytes,FIntPoint(-32768,-32768),Sector,bFallback,Error));
    TestEqual(TEXT("Original invalid lookup mounts to sector0"),Sector,0);
    TestTrue(TEXT("Fallback remains explicit to caller"),bFallback);
    auto Truncated=Bytes;Truncated.SetNum(11);
    TestFalse(TEXT("Truncated header is rejected, never guessed sector0"),
        NativeWadScenerySource::ResolveMountSector(Truncated,FIntPoint(2360,3188),Sector,bFallback,Error));
    TestEqual(TEXT("Malformed input leaves no valid sector"),Sector,INDEX_NONE);
    auto BadDirectory=Bytes;for(int32 I=8;I<12;++I){BadDirectory[I]=0xff;}
    TestFalse(TEXT("Overflowed directory offset is rejected"),
        NativeWadScenerySource::ResolveMountSector(BadDirectory,FIntPoint(2360,3188),Sector,bFallback,Error));
    const auto U32=[](const TArray<uint8>& B,int32 At){return uint32(B[At])|uint32(B[At+1])<<8|uint32(B[At+2])<<16|uint32(B[At+3])<<24;};
    const uint32 Count=U32(Bytes,4),Directory=U32(Bytes,8);
    for(uint32 I=0;I<Count;++I)
    {
        const int32 At=int32(Directory+I*16);
        if(FMemory::Memcmp(Bytes.GetData()+At+8,"BLOCKMAP",8)){continue;}
        const uint32 Offset=U32(Bytes,At);auto BadIndex=Bytes;
        // This exact authored point starts at block32,row22. Its list offset must be in-bounds.
        const int32 Cell=int32(Offset)+8+(22*46+32)*2;
        BadIndex[Cell]=0xff;BadIndex[Cell+1]=0xff;
        TestFalse(TEXT("Malformed BLOCKMAP list offset is rejected without out-of-bounds read"),
            NativeWadScenerySource::ResolveMountSector(BadIndex,FIntPoint(2360,3188),Sector,bFallback,Error));
        auto BadDimensions=Bytes;BadDimensions[Offset+4]=0xff;BadDimensions[Offset+5]=0xff;
        BadDimensions[Offset+6]=0xff;BadDimensions[Offset+7]=0xff;
        TestFalse(TEXT("Overflowed BLOCKMAP table dimensions are rejected"),
            NativeWadScenerySource::ResolveMountSector(BadDimensions,FIntPoint(2360,3188),Sector,bFallback,Error));
        return true;
    }
    AddError(TEXT("Original BLOCKMAP disappeared"));return false;
}

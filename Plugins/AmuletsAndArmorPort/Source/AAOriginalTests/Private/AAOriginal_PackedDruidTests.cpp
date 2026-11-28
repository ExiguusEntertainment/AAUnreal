#include "Misc/AutomationTest.h"
#include "Misc/ScopeExit.h"
#include "Misc/App.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Internationalization/Regex.h"
#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "NativeWadEncounter.h"
#include "NativeWadArt.h"
#include "../../AAOriginal/Private/NativeWadHudData.h"
#include "NativeWadLevel.h"
#include "NativeWadPlayer.h"
#include "UObject/UnrealType.h"
#include "../../AAOriginal/Private/NativeWadCheckpoint.h"

namespace
{
    FString SourceRoot() { return FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor"); }
    bool Record(int32 Type,TArray<FString>& Fields)
    {
        FString Source;
        if (!FFileHelper::LoadFileToString(Source,*(SourceRoot()/TEXT("Include/CREDATA.H")))) { return false; }
        FRegexMatcher Match(FRegexPattern(FString::Printf(TEXT("/\\*[^*]*\\*/\\s*(%d\\s*,[^}]+)"),Type)),Source);
        if (!Match.FindNext()) { return false; }
        Match.GetCaptureGroup(1).TrimStartAndEnd().ParseIntoArray(Fields,TEXT(","),true);
        for (auto& Field:Fields) { Field.TrimStartAndEndInline(); }
        return Fields.Num()==43;
    }
    bool Floor(ANativeWadLevel& Level)
    {
        const auto* P=FindFProperty<FArrayProperty>(ANativeWadLevel::StaticClass(),TEXT("Sectors"));
        if (!P) { return false; }
        auto& Sectors=*P->ContainerPtrToValuePtr<TArray<FAAWadNativeSector>>(&Level);
        auto& S=Sectors.AddDefaulted_GetRef();
        S.RepairedBoundaryPoints={{-500,-500},{500,-500},{500,-500},{500,500},{500,500},{-500,500},{-500,500},{-500,-500}};
        S.Floor=NewObject<UStaticMeshComponent>(&Level); S.Ceiling=NewObject<UStaticMeshComponent>(&Level);
        S.Floor->SetupAttachment(Level.GetRootComponent()); S.Ceiling->SetupAttachment(Level.GetRootComponent());
        S.Ceiling->SetRelativeLocation(FVector(0,0,256)); S.Floor->RegisterComponent();S.Ceiling->RegisterComponent();
        Level.SetSectorFloorZ(0,0);return true;
    }
    TArray<ANativeWadPickup*> Drops(UWorld* World)
    {
        TArray<ANativeWadPickup*> Out;
        for (TActorIterator<ANativeWadPickup> It(World);It;++It) { if (It->IsDropped() && !It->IsActorBeingDestroyed()) { Out.Add(*It); } }
        return Out;
    }
    struct FFixture
    {
        UGameInstance* Instance=NewObject<UGameInstance>(GEngine);
        double Volume=FApp::GetVolumeMultiplier();
        UWorld* World=nullptr; ANativeWadLevel* Level=nullptr; ANativeWadPlayer* Player=nullptr;
        FFixture()
        {
            FApp::SetVolumeMultiplier(0.0);
            Instance->InitializeStandalone();World=Instance->GetWorld();if (!World) { return; }
            World->URL=FURL(nullptr,TEXT("/Game/Maps/L22?WadCreated=1?WadClass=0?WadLevel=1?WadMaxHealth=30000?WadHealth=30000?WadMaxMana=30000?WadMana=30000?WadFood=2000?WadWater=2000?WadItems=?WadReady=-1"),TRAVEL_Absolute);
            Level=World->SpawnActor<ANativeWadLevel>(); if (!Level || !Floor(*Level)) { return; }
            Player=World->SpawnActor<ANativeWadPlayer>(FVector(400,0,34),FRotator::ZeroRotator);
            auto* Controller=World->SpawnActor<APlayerController>();if (!Player || !Controller) { return; }
            World->AddController(Controller);Controller->Possess(Player);Player->DispatchBeginPlay();
        }
        ~FFixture() { Instance->Shutdown();if (World) { GEngine->DestroyWorldContext(World);World->DestroyWorld(false); } FApp::SetVolumeMultiplier(Volume); }
        ANativeWadGuardian* Creature(int32 Type)
        {
            auto* Enemy=World->SpawnActor<ANativeWadGuardian>(FVector(0,0,33.5),FRotator::ZeroRotator);
            if (!Enemy) { return nullptr; }
            FWadEncounterProfile Profile; Profile.SourceType=Type;Profile.CollisionHalfHeight=31.5f;Profile.SpriteSize=FVector2D(87,63);
            auto* Material=UMaterial::GetDefaultMaterial(MD_Surface);
            Enemy->Configure(nullptr,Material,Material,Material,Material,Material,Profile);
            Enemy->Tags.Add(Type==46075?FName(TEXT("L22 Legacy Jugurtha")):FName(TEXT("WAD Creature Thing 61")));
            Enemy->DispatchBeginPlay();return Enemy;
        }
    };
    bool CpcPixels(const FString& Picture,int32 Palette,TArray<FColor>& Out,FIntPoint& Size,EWadArtPalette Domain)
    {
        TArray<uint8> Cpc,Map,Colors;
        const FString Art=SourceRoot()/TEXT("Resources/ArtFinal");
        if (!FFileHelper::LoadFileToArray(Cpc,*(Art/TEXT("OBJS")/Picture)) || Cpc.Num()<4
            || !FFileHelper::LoadFileToArray(Colors,*(Art/TEXT("GAME.PAL"))) || Colors.Num()<768
            || !FFileHelper::LoadFileToArray(Map,*(Art/TEXT("COLORIZE")/FString::Printf(TEXT("COLORT%02d"),Palette))) || Map.Num()!=256) { return false; }
        if(Domain==EWadArtPalette::WorldStandard)
        {
            TArray<uint8> View;if(!FFileHelper::LoadFileToArray(View,*(Art/TEXT("VIEW00.PAL")))||View.Num()!=768){return false;}
            FMemory::Memcpy(Colors.GetData(),View.GetData(),224*3); // Independent original ViewSetPalette composition.
        }
        else if(Domain!=EWadArtPalette::InterfaceGame){return false;}
        const auto U16=[&](int32 I){return int32(Cpc[I]) | int32(Cpc[I+1])<<8;};
        Size=FIntPoint(U16(2),U16(0)); if (Size.X<=0 || Size.Y<=0 || Cpc.Num()<4+4*Size.X) { return false; }
        Out.Init(FColor(0,0,0,0),Size.X*Size.Y);
        for (int32 X=0;X<Size.X;++X)
        {
            int32 I=4+4*X,Offset=U16(I),Top=Cpc[I+2],Bottom=Cpc[I+3];
            if (Top==255 && Bottom==255) { continue; }
            if (Top>Bottom || Bottom>=Size.Y || Offset<4+4*Size.X || Offset+Bottom-Top>=Cpc.Num()) { return false; }
            for (int32 Y=Top;Y<=Bottom;++Y)
            {
                const uint8 Original=Cpc[Offset+Y-Top];if (!Original) { continue; }
                const int32 Color=Map[Original]*3;
                Out[Y*Size.X+X]=FColor((Colors[Color]&63)*255/63,(Colors[Color+1]&63)*255/63,(Colors[Color+2]&63)*255/63,255);
            }
        }
        return true;
    }
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPackedDruidSource,"AAOriginal.WAD.Gameplay.PackedDruidSource",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalPackedDruidSource::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    if (!World) { return false; }
    ON_SCOPE_EXIT { World->DestroyWorld(false); };
    for (int32 Type:{33787,46075})
    {
        TArray<FString> F;if (!TestTrue(TEXT("Read full packed CREDATA record, not masked1019"),Record(Type,F))) { return false; }
        auto* Enemy=World->SpawnActor<ANativeWadGuardian>();
        if (!Enemy) { return false; }
        FWadEncounterProfile Seed;Seed.SourceType=Type;
        Enemy->Configure(nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,Seed);
        const FWadEncounterProfile& P=Enemy->GetEncounterProfile();
        TestEqual(TEXT("Exact combat identity"),P.SourceType,FCString::Atoi(*F[0]));
        TestEqual(TEXT("Compatible source Druid navigation"),F[1],FString(TEXT("NAV_LOGIC_PKG_COMBO_FIGHTER_ARCHER")));
        TestEqual(TEXT("Compatible hit/shoot target package"),F[2],FString(TEXT("TARGET_LOGIC_PACKAGE_STANDARD_HIT_OR_SHOOT")));
        TestTrue(TEXT("Both original canFly flags are false"),F[3]==TEXT("FALSE")&&!P.bCanFly);
        TestEqual(TEXT("Source update period"),P.UpdateTicks,FCString::Atoi(*F[7]));
        TestEqual(TEXT("Source step/update speed"),P.WalkSpeed,float(FCString::Atoi(*F[8])*70/(P.UpdateTicks+(P.UpdateTicks>>1))));
        TestEqual(TEXT("Source melee range"),P.MeleeRange,float(FCString::Atoi(*F[9])));
        TestEqual(TEXT("Source minimum firing range"),P.MinBoltRange,float(FCString::Atoi(*F[10])));
        TestEqual(TEXT("Source maximum firing range"),P.BoltRange,FCString::Atoi(*F[11]));
        TestEqual(TEXT("Source melee delay"),P.MeleeDelayUpdates,FCString::Atoi(*F[12]));
        TestEqual(TEXT("Distinct source firing delays"),P.RangedDelayUpdates,FCString::Atoi(*F[13]));
        TestEqual(TEXT("Distinct original maximum health"),P.MaxHealth,FCString::Atoi(*F[14]));
        TestEqual(TEXT("Distinct original regeneration"),P.RegenPerUpdate,FCString::Atoi(*F[15]));
        TestEqual(TEXT("Poison missile4015, not homing4018"),P.SourceMissileType,FCString::Atoi(*F[16]));
        TestEqual(TEXT("Original poison missile is straight"),P.HomingMissileType,0);
        TestTrue(TEXT("Source melee mixes normal/poison"),F[17].Contains(TEXT("EFFECT_DAMAGE_POISON")) && P.MeleeDamageMask==uint8(NormalDamage|DamageFlag(EProjectileDamage::Poison)));
        TestEqual(TEXT("Distinct melee damage"),P.MeleeDamage,FCString::Atoi(*F[18]));
        TestTrue(TEXT("Both source families resist poison"),F[20].Contains(TEXT("EFFECT_DAMAGE_POISON")) && P.ResistanceMask==DamageFlag(EProjectileDamage::Poison));
        TestEqual(TEXT("Source retreat health"),P.RetreatHealth,FCString::Atoi(*F[21]));
        TestEqual(TEXT("Source default cloth armor has no palette piercing bonus"),P.ArmorClass,0);
        TestEqual(TEXT("Packed source vision"),P.VisionHalfAngle,Type==46075?157.5f:90.0f);
        TestEqual(TEXT("Source creature target acceptance"),P.ChanceAttackCreature,FCString::Atoi(*F[38]));
        TestEqual(TEXT("Source attack continuation"),P.ChanceContinueAttack,FCString::Atoi(*F[39]));
        TestEqual(TEXT("Original4015 damage is independent of variant melee"),P.BoltDamage,250);
    }
    for (int32 Roll=0;Roll<=65535;++Roll)
    {
        const auto Poison=ResolveWadCreatureTreasure(33787,uint16(Roll)),Boss=ResolveWadCreatureTreasure(46075,uint16(Roll));
        if (Poison.ItemType!=((Roll&1)?202:825) || Poison.AdditionalItemType || Poison.HasAccessoryData()
            || Boss.ItemType!=415 || Boss.AdditionalItemType!=234 || Boss.HasAccessoryData())
        { AddError(TEXT("Packed source treasure differs at a16-bit helper input"));return false; }
    }
    TestEqual(TEXT("Ordinary Druid's separate scroll table is unchanged"),ResolveWadCreatureTreasure(1019,0).ItemType,822);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPackedDruidArt,"AAOriginal.WAD.Import.PackedDruidArtAndCounts",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalPackedDruidArt::RunTest(const FString& Parameters)
{
    for (const FString Map:{FString(TEXT("L31")),FString(TEXT("L22"))})
    {
        UWorld* World=LoadObject<UWorld>(nullptr,*(TEXT("/Game/Maps/")+Map+TEXT(".")+Map));
        if (!TestNotNull(TEXT("Saved map after targeted encounter/art refresh"),World)) { return false; }
        int32 Poison=0,Boss=0;TSet<FName> Tags;
        for (TActorIterator<ANativeWadGuardian> It(World);It;++It)
        {
            const int32 Type=It->GetSourceType();if (Type!=33787 && Type!=46075) { continue; }
            Poison+=Type==33787;Boss+=Type==46075;
            TestEqual(TEXT("Look and combat retain the exact full identity"),It->GetLookSourceType(),Type);
            if (!TestFalse(TEXT("Each authored actor has its own checkpoint tag"),It->Tags.IsEmpty())) { return false; }
            TestFalse(TEXT("No duplicate primary identity"),Tags.Contains(It->Tags[0]));Tags.Add(It->Tags[0]);
            const TCHAR* States[]={TEXT("StandingMaterial"),TEXT("HurtMaterial"),TEXT("DownedMaterial"),TEXT("AttackingMaterial")};
            const TCHAR* Pictures[]={TEXT("DRUID/DRUDW001.CPC"),TEXT("DRUID/DRUDD001.CPC"),TEXT("DRUID/DRUDD008.CPC"),TEXT("DRUID/DRUID001.CPC")};
            for (int32 State=0;State<4;++State)
            {
                const auto* P=FindFProperty<FObjectPropertyBase>(ANativeWadGuardian::StaticClass(),States[State]);
                auto* Material=P?Cast<UMaterialInterface>(P->GetObjectPropertyValue_InContainer(*It)):nullptr;
                if (!TestNotNull(TEXT("Every source stance has saved palette-specific art"),Material)) { return false; }
                TArray<UTexture*> Textures;
                for (const auto& Expression:Material->GetMaterial()->GetExpressionCollection().Expressions)
                { if (const auto* Sample=Cast<UMaterialExpressionTextureSample>(Expression)) { Textures.AddUnique(Sample->Texture.Get()); } }
                UTexture2D* Texture=nullptr;for (auto* T:Textures) { if (auto* Candidate=Cast<UTexture2D>(T)) { Texture=Candidate;break; } }
                if (!TestNotNull(TEXT("Owned generated material has its actual source texture sample"),Texture)) { return false; }
                FIntPoint Size;TArray<FColor> Expected;TArray64<uint8> Actual;EWadArtPalette Domain=EWadArtPalette::InterfaceGame;
                if(const auto* Source=NativeWadArt::GetSource(*Texture))
                {
                    TArray<uint8> ExpectedTable;
                    FFileHelper::LoadFileToArray(ExpectedTable,*(SourceRoot()/TEXT("Resources/ArtFinal/COLORIZE")/FString::Printf(TEXT("COLORT%02d"),Type>>12)));
                    if(!TestTrue(TEXT("Saved source provenance has the exact frame/variant and a supported palette domain"),Source->IsValid()
                        &&Source->Encoding==EWadArtEncoding::Sprite&&Source->ResourcePath.Equals(TEXT("OBJS/")+FString(Pictures[State]),ESearchCase::IgnoreCase)
                        &&ExpectedTable.Num()==256&&Source->ColorTable==ExpectedTable)){return false;}
                    Domain=Source->Palette;
                }
                if (!TestTrue(TEXT("Decode original CPC + authored COLORT08/11 and validated palette independently"),CpcPixels(Pictures[State],Type>>12,Expected,Size,Domain))
                    || !TestTrue(TEXT("Read saved texture's actual source pixels"),Texture->Source.GetMipData(Actual,0))) { return false; }
                TestEqual(TEXT("Original sprite width"),Texture->Source.GetSizeX(),int64(Size.X));TestEqual(TEXT("Original sprite height"),Texture->Source.GetSizeY(),int64(Size.Y));
                TestTrue(TEXT("Every saved pixel equals the source palette remap, including transparent background"),Actual.Num()==Expected.Num()*sizeof(FColor)
                    && FMemory::Memcmp(Actual.GetData(),Expected.GetData(),Actual.Num())==0);
            }
        }
        TestEqual(TEXT("All original Ulfilas placements restored"),Poison,Map==TEXT("L31")?5:8);
        TestEqual(TEXT("Exact original Jugurtha count"),Boss,Map==TEXT("L31")?0:1);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPackedDruidCombat,"AAOriginal.WAD.Gameplay.PackedDruidCombat",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalPackedDruidCombat::RunTest(const FString& Parameters)
{
    using namespace NativeWadRules;
    for (int32 Type:{33787,46075})
    {
        FFixture F;if (!F.Player) { return false; }auto* Enemy=F.Creature(Type);if (!Enemy) { return false; }
        int32 Before=Enemy->GetEncounterHealth();
        TestEqual(TEXT("Actual runtime life starts at its full source health"),Before,Type==46075?4500:2500);
        TestFalse(TEXT("Actual poison hit is resisted"),Enemy->TakeHit(100,nullptr,DamageFlag(EProjectileDamage::Poison)));
        TestEqual(TEXT("No resisted poison stored"),Enemy->GetPoisonLevel(),0);TestEqual(TEXT("No resisted health damage"),Enemy->GetEncounterHealth(),Before);
        TestTrue(TEXT("Mixed normal/poison reaches real typed resolver"),Enemy->TakeHit(100,nullptr,uint8(NormalDamage|DamageFlag(EProjectileDamage::Poison))));
        TestEqual(TEXT("Only the normal half survives resistance"),Enemy->GetEncounterHealth(),Before-50);
        Before=Enemy->GetEncounterHealth();Enemy->TakeHit(100,nullptr,DamageFlag(EProjectileDamage::Piercing));
        TestEqual(TEXT("Palette8/11 has no invented piercing modifier on source cloth"),Enemy->GetEncounterHealth(),Before-100);
        Enemy->Tick(10.0f/70.0f);
        TestEqual(TEXT("One source logical update regenerates its exact amount"),Enemy->GetEncounterHealth(),Before-100+(Type==46075?4:1));
        Enemy->GetCharacterMovement()->SetMovementMode(MOVE_Flying);
        Enemy->ResetForGenerator(); // A genuine fresh life clears the preceding hurt window.
        TestTrue(TEXT("Reuse restores grounded movement even after a legacy flying state"),Enemy->GetCharacterMovement()->IsMovingOnGround());
        TestEqual(TEXT("Reuse retains packed combat identity"),Enemy->GetSourceType(),Type);
        Enemy->Tick(0.01f);Enemy->Tick(1.0f);
        TestEqual(TEXT("Normal creature AI launches a real missile"),Enemy->GetBoltsFired(),1);
        AActor* Bolt=nullptr;for (TActorIterator<AActor> It(F.World);It;++It) { if (It->GetClass()->GetFName()==TEXT("NativeWadBolt")) { Bolt=*It;break; } }
        if (!TestNotNull(TEXT("Actual poison projectile actor"),Bolt)) { return false; }
        const int32 Health=F.Player->GetHealth(),Poison=F.Player->GetPoisonLevel();
        F.Player->SetActorLocation(FVector(400,100,34));Bolt->Tick(0.05f);
        TestTrue(TEXT("The poison missile does not home after target moves"),FMath::IsNearlyZero(Bolt->GetActorLocation().Y,0.001));
        F.Player->SetActorLocation(FVector(400,0,34));Bolt->Tick(0.3f);
        TestTrue(TEXT("Real sweep impacts the player's capsule"),Bolt->IsActorBeingDestroyed());
        TestEqual(TEXT("Source250 poison projectile health damage"),F.Player->GetHealth(),Health-250);
        TestEqual(TEXT("Source poison transfer is250/10"),F.Player->GetPoisonLevel(),Poison+25);
    }
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalPackedDruidTreasure,"AAOriginal.WAD.Gameplay.PackedDruidTreasureCheckpoint",
    EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FAAOriginalPackedDruidTreasure::RunTest(const FString& Parameters)
{
    FFixture F;if (!F.Player) { return false; }auto* Boss=F.Creature(46075);if (!Boss) { return false; }
    auto* Quest=F.World->SpawnActor<ANativeWadPickup>();if (!Quest) { return false; }
    Quest->Configure(nullptr,nullptr,NativeWadRules::JugurthaJournal);Quest->Tags.Add(TEXT("Separate authored quest pickup"));
    Boss->SetDefeatPickup(Quest); // Existing defeat links must coexist; the actual L22 journal is a separate map object.
    const FName Identity=Boss->Tags[0];const int32 Life=Boss->GetCreatureLife();
    TestTrue(TEXT("Actual normal damage defeats source Jugurtha"),Boss->TakeHit(10000));
    auto Items=Drops(F.World);if (!TestEqual(TEXT("Both ordinary source treasures release"),Items.Num(),2)) { return false; }
    TSet<int32> Types;for (auto* Item:Items)
    {
        Types.Add(Item->GetItemType());const auto* Art=Item->FindComponentByClass<UStaticMeshComponent>();
        TestTrue(TEXT("Each ordinary item has real saved world art"),Art && Art->GetMaterial(0));
        const auto* Catalog=LoadObject<UNativeWadHudData>(nullptr,UNativeWadHudData::ObjectPath);const auto* Details=Catalog?Catalog->ItemDetails.Find(Item->GetItemType()):nullptr;
        TestTrue(TEXT("Each drop remains rooted at the defeated creature's bottom"),Details && FMath::IsNearlyEqual(Item->GetActorLocation().Z-Details->SpriteSize.Y*0.5,
            Boss->GetActorLocation().Z-Boss->GetCapsuleComponent()->GetScaledCapsuleHalfHeight(),0.001));
    }
    TestTrue(TEXT("Ring415 and electric mace234 coexist"),Types.Contains(415)&&Types.Contains(234));
    TestTrue(TEXT("Existing separate defeat pickup is released too"),Quest->IsAvailable());
    Boss->TakeHit(10000);TestEqual(TEXT("Repeated hits never duplicate either treasure"),Drops(F.World).Num(),2);
    FWadLevelCheckpoint CP;if (!TestTrue(TEXT("Use real level snapshot with both drops and legacy boss identity"),F.Level->WriteCheckpoint(CP))) { return false; }
    TestTrue(TEXT("The old primary checkpoint identity is preserved"),CP.Encounters.Contains(Identity));
    UClass* SaveClass=FindObject<UClass>(nullptr,TEXT("/Script/AAOriginal.NativeWadCheckpoint"));if (!SaveClass) { return false; }
    auto* Save=static_cast<UNativeWadCheckpoint*>(NewObject<USaveGame>(GetTransientPackage(),SaveClass));Save->Level=CP;
    TArray<uint8> Bytes;if (!UGameplayStatics::SaveGameToMemory(Save,Bytes)) { return false; }
    auto* Loaded=static_cast<UNativeWadCheckpoint*>(UGameplayStatics::LoadGameFromMemory(Bytes));if (!Loaded) { return false; }
    auto* NewlyRestored=F.Creature(33787);if (!NewlyRestored) { return false; }
    TestFalse(TEXT("Earlier checkpoint has no invented state for newly restored source placements"),Loaded->Level.Encounters.Contains(NewlyRestored->Tags[0]));
    if (!TestTrue(TEXT("Actual serialized checkpoint restores boss and both drops"),F.Level->RestoreCheckpoint(Loaded->Level))) { return false; }
    TestEqual(TEXT("Old checkpoint leaves newly restored source actors alive at their authored health"),NewlyRestored->GetEncounterHealth(),2500);
    NewlyRestored->Destroy();
    TestEqual(TEXT("Restore never rerolls the two-item treasure"),Drops(F.World).Num(),2);Boss->TakeHit(10000);
    TestEqual(TEXT("Defeated restored life still cannot duplicate loot"),Drops(F.World).Num(),2);
    for (auto* Item:Drops(F.World))
    {
        const int32 Type=Item->GetItemType(),Before=F.Player->GetItemCount(Type);
        TestTrue(TEXT("Both catalog-backed world objects use ordinary inventory collection"),Item->Collect(*F.Player));
        TestEqual(TEXT("Full item identity reaches inventory"),F.Player->GetItemCount(Type),Before+1);
    }
    TestEqual(TEXT("Collection did not masquerade the boss's separate quest object as ordinary treasure"),F.Player->GetItemCount(NativeWadRules::JugurthaJournal),0);
    Boss->ResetForGenerator();TestEqual(TEXT("Same actor gets a fresh life"),Boss->GetCreatureLife(),Life+1);
    TestEqual(TEXT("Full boss source identity survives reuse"),Boss->GetSourceType(),46075);
    TestTrue(TEXT("New life can release its two normal source items again"),Boss->TakeHit(10000));TestEqual(TEXT("Exactly two new drops"),Drops(F.World).Num(),2);
    for (auto* Item:Drops(F.World)) { Item->Destroy(); }
    auto* Ulfilas=F.Creature(33787);if (!Ulfilas) { return false; }
    TestTrue(TEXT("Real Ulfilas defeat selects its source treasure once"),Ulfilas->TakeHit(10000));
    const auto Ordinary=Drops(F.World);if (!TestEqual(TEXT("Ulfilas releases one normal catalog-backed item"),Ordinary.Num(),1)) { return false; }
    TestTrue(TEXT("Actual drop retains the full packed table, never ordinary Druid scroll822"),Ordinary[0]->GetItemType()==202 || Ordinary[0]->GetItemType()==825);
    Ulfilas->TakeHit(10000);Ulfilas->RestoreEncounterHealth(0);
    TestEqual(TEXT("Repeated hit/restore never rerolls packed Druid loot"),Drops(F.World).Num(),1);
    for (auto* Item:Drops(F.World)) { Item->Destroy(); }
    for (int32 Type:{202,825})
    {
        auto* Drop=ANativeWadPickup::SpawnDropped(*F.Level,Type,FVector(0,0,32));if (!Drop) { return false; }
        const int32 Before=F.Player->GetItemCount(Type);TestTrue(TEXT("Both Ulfilas branch items collect through normal rules"),Drop->Collect(*F.Player));
        TestEqual(TEXT("Ulfilas branch type enters inventory intact"),F.Player->GetItemCount(Type),Before+1);
    }
    return true;
}

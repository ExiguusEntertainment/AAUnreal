#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeWadArt.h"
#include "Engine/Texture2D.h"
#include "../../AAOriginal/Private/WadSurfaceTextures.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalWorldArtRouteDomains,"AAOriginal.WAD.Art.ExplicitRouteDomains",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalWorldArtRouteDomains::RunTest(const FString& Parameters)
{
    FIntPoint GameSize,WorldSize,DefaultSize;TArray<FColor> Game,World,Default;
    if(!LoadWadSprite(TEXT("RING17/RING17.CPC"),GameSize,Game,{},EWadArtPalette::InterfaceGame)
        ||!LoadWadSprite(TEXT("RING17/RING17.CPC"),WorldSize,World,{},EWadArtPalette::WorldStandard)
        ||!LoadWadSprite(TEXT("RING17/RING17.CPC"),DefaultSize,Default))
    {AddError(TEXT("Original ring source route unavailable"));return false;}
    TestEqual(TEXT("Palette does not change original sprite dimensions"),GameSize,WorldSize);
    TestTrue(TEXT("Existing icon call retains approved GAME pixels by default"),Game==Default);
    int32 Changed=0;
    for(int32 I=0;I<Game.Num();++I){TestEqual(TEXT("World palette retains exact CPC transparency"),World[I].A,Game[I].A);Changed+=Game[I]!=World[I];}
    TestTrue(TEXT("Explicit world palette changes real lower-palette source colors"),Changed>0);
    auto* Icon=NewObject<UTexture2D>();auto* WorldPicture=NewObject<UTexture2D>();
    Icon->Source.Init(GameSize.X,GameSize.Y,1,1,TSF_BGRA8,reinterpret_cast<const uint8*>(Game.GetData()));
    WorldPicture->Source.Init(WorldSize.X,WorldSize.Y,1,1,TSF_BGRA8,reinterpret_cast<const uint8*>(World.GetData()));
    if(!NativeWadArt::RecordPicture(*Icon,EWadArtEncoding::Sprite,EWadArtPalette::InterfaceGame,
            TEXT("OBJS/RING17/RING17.CPC"),GameSize,Game,true)
        ||!NativeWadArt::RecordPicture(*WorldPicture,EWadArtEncoding::Sprite,EWadArtPalette::WorldStandard,
            TEXT("OBJS/RING17/RING17.CPC"),WorldSize,World,true)){return false;}
    const auto* IconSource=NativeWadArt::GetSource(*Icon);const auto* WorldSource=NativeWadArt::GetSource(*WorldPicture);
    if(!IconSource||!WorldSource){return false;}
    TestTrue(TEXT("Same exact original bytes can have explicitly separate interface/world provenance"),IconSource->SourceSHA1==WorldSource->SourceSHA1&&IconSource->Palette!=WorldSource->Palette);
    TestTrue(TEXT("Generated pixel hashes distinguish interface and world expansions"),IconSource->MipSHA1!=WorldSource->MipSHA1);
    TArray64<uint8> Before,After;Icon->Source.GetMipData(Before,0);
    TestFalse(TEXT("Invalid source traversal cannot replace original icon provenance"),NativeWadArt::RecordPicture(*Icon,
        EWadArtEncoding::Sprite,EWadArtPalette::WorldStandard,TEXT("../VIEW00.PAL"),GameSize,Game,true));
    Icon->Source.GetMipData(After,0);
    TestTrue(TEXT("Metadata failure never changes approved icon source bytes"),Before==After);
    TestTrue(TEXT("Metadata failure never changes approved interface domain"),NativeWadArt::GetSource(*Icon)->Palette==EWadArtPalette::InterfaceGame);
    FIntPoint BackdropGameSize,BackdropWorldSize;TArray<FColor> BackdropGame,BackdropWorld;
    TestTrue(TEXT("L151 backdrop explicit world route decodes"),LoadWadBackdrop(TEXT("2BACK02.PIC"),BackdropWorldSize,BackdropWorld,EWadArtPalette::WorldStandard));
    TestTrue(TEXT("Backdrop default remains unchanged before world-only migration"),LoadWadBackdrop(TEXT("2BACK02.PIC"),BackdropGameSize,BackdropGame));
    TestEqual(TEXT("Backdrop palette leaves dimensions unchanged"),BackdropWorldSize,BackdropGameSize);
    TestTrue(TEXT("Actual backdrop world palette differs from approved legacy expansion"),BackdropGame!=BackdropWorld);
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalSurfaceSourceProvenance,"AAOriginal.WAD.Art.SurfaceRouteProvenance",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalSurfaceSourceProvenance::RunTest(const FString& Parameters)
{
    auto* Surface=LoadWadSurfaceTexture(TEXT("BIGDOOR1"));if(!TestNotNull(TEXT("Original door surface"),Surface)){return false;}
    const auto* Source=NativeWadArt::GetSource(*Surface);
    if(!TestNotNull(TEXT("Transient surface retains original archive identity before saved import"),Source)){return false;}
    TestTrue(TEXT("World preview records exact five authored uploaded mips"),Source->IsValid()
        &&Source->Encoding==EWadArtEncoding::Surface&&Source->bAuthoredMips&&Source->MipSHA1.Num()==5&&Source->Palette==EWadArtPalette::WorldStandard
        &&Source->ArchiveEntry==TEXT("BIGDOOR1")&&Source->ResourcePath==TEXT("PICS.RES"));
    TestTrue(TEXT("Empty/no-texture marker does not acquire surface pixels"),LoadWadSurfaceTexture(TEXT("-"))==nullptr);
    return true;
}

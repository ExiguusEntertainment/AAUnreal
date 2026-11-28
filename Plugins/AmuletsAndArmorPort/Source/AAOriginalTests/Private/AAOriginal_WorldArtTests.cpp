#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "NativeWadArt.h"
#include "Engine/Texture2D.h"
#include "Serialization/MemoryReader.h"
#include "Serialization/MemoryWriter.h"
#include "Serialization/ObjectAndNameAsStringProxyArchive.h"

namespace
{
    FString ArtRoot(){return FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal");}
    uint32 DWord(TConstArrayView<uint8> Bytes,int32 At)
    {return uint32(Bytes[At])|(uint32(Bytes[At+1])<<8)|(uint32(Bytes[At+2])<<16)|(uint32(Bytes[At+3])<<24);}
    int32 Word(TConstArrayView<uint8> Bytes,int32 At){return Bytes[At]|(int32(Bytes[At+1])<<8);}
    void AddWord(TArray<uint8>& Bytes,int32 Value){Bytes.Add(uint8(Value));Bytes.Add(uint8(Value>>8));}
    TArray<uint8> SyntheticMips(FIntPoint Size)
    {
        TArray<uint8> Bytes;
        for(int32 Level=0;Level<5;++Level)
        {
            AddWord(Bytes,Size.Y);AddWord(Bytes,Size.X);
            for(int32 X=0;X<(Size.X>>Level);++X)for(int32 Y=0;Y<(Size.Y>>Level);++Y)
            {Bytes.Add(uint8(X*7+Y*13+Level*31));}
        }
        return Bytes;
    }
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalWorldPaletteDomains,"AAOriginal.WAD.Art.PaletteDomains",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalWorldPaletteDomains::RunTest(const FString& Parameters)
{
    TArray<uint8> Game,View;
    if(!FFileHelper::LoadFileToArray(Game,*(ArtRoot()/TEXT("GAME.PAL")))
        ||!FFileHelper::LoadFileToArray(View,*(ArtRoot()/TEXT("VIEW00.PAL")))){AddError(TEXT("Original palettes unavailable"));return false;}
    TArray<FColor> Interface,World;
    TestTrue(TEXT("Explicit approved native interface palette decodes"),NativeWadArt::DecodePalette(Game,{},EWadArtPalette::InterfaceGame,Interface));
    TestTrue(TEXT("Original standard world palette composes"),NativeWadArt::DecodePalette(Game,View,EWadArtPalette::WorldStandard,World));
    if(Interface.Num()!=256||World.Num()!=256){return false;}
    int32 Changed=0;
    for(int32 I=0;I<224;++I){Changed+=World[I]!=Interface[I];}
    TestEqual(TEXT("Actual original standard view changes179 lower colors"),Changed,179);
    for(int32 I=224;I<256;++I){TestEqual(FString::Printf(TEXT("Reserved GAME tail%d is retained"),I),World[I],Interface[I]);}
    // A deliberately different VIEW tail verifies composition, not incidental source equality.
    for(int32 I=224*3;I<View.Num();++I){View[I]^=63;}
    TArray<FColor> Composed;TestTrue(TEXT("Changed VIEW tail is ignored"),NativeWadArt::DecodePalette(Game,View,EWadArtPalette::WorldStandard,Composed));
    TestTrue(TEXT("Composition retains all original reserved colors"),Composed==World);
    const auto Before=Composed;View.Pop();
    TestFalse(TEXT("Truncated world palette refuses"),NativeWadArt::DecodePalette(Game,View,EWadArtPalette::WorldStandard,Composed));
    TestTrue(TEXT("Refused palette leaves existing output unchanged"),Composed==Before);
    TArray<uint8> Map;Map.SetNumUninitialized(256);for(int32 I=0;I<256;++I){Map[I]=uint8(255-I);}
    const TArray<uint8> Indices={0,1,223,224,255};TArray<FColor> Pixels;
    TestTrue(TEXT("Packed object recoloring precedes palette lookup"),NativeWadArt::ExpandPixels(Indices,World,true,Map,Pixels));
    TestEqual(TEXT("Original zero remains transparent even if remapped"),Pixels[0],FColor(0,0,0,0));
    TestEqual(TEXT("Opaque source index uses remapped world color"),Pixels[1],World[254]);
    TestTrue(TEXT("Opaque backdrops retain index zero color"),NativeWadArt::ExpandPixels(Indices,World,false,{},Pixels));
    TestEqual(TEXT("Backdrop zero is opaque"),Pixels[0],World[0]);
    Map.Pop();const auto Good=Pixels;
    TestFalse(TEXT("Partial variant mapping refuses"),NativeWadArt::ExpandPixels(Indices,World,true,Map,Pixels));
    TestTrue(TEXT("Failed expansion is transactional"),Pixels==Good);return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalWorldMipCodec,"AAOriginal.WAD.Art.AuthoredMipCodec",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalWorldMipCodec::RunTest(const FString& Parameters)
{
    TArray<uint8> Bytes=SyntheticMips({32,16});FWadIndexedMipChain Chain;
    TestTrue(TEXT("All five original repeated-header levels decode"),NativeWadArt::DecodeSurfaceMips(Bytes,Chain));
    TestEqual(TEXT("Full-size picture dimensions are width,height"),Chain.OriginalSize,FIntPoint(32,16));
    TestEqual(TEXT("Five authored levels retained"),Chain.Mips.Num(),5);
    for(int32 Level=0;Level<Chain.Mips.Num();++Level)
    {
        const auto& Mip=Chain.Mips[Level];TestEqual(TEXT("Reduced dimensions derive from level, never repeated header"),Mip.Size,FIntPoint(32>>Level,16>>Level));
        for(int32 Y=0;Y<Mip.Size.Y;++Y)for(int32 X=0;X<Mip.Size.X;++X)
        {if(Mip.Indices[Y*Mip.Size.X+X]!=uint8(X*7+Y*13+Level*31)){AddError(TEXT("Column-major source did not become row-major"));return false;}}
    }
    const FIntPoint Before=Chain.OriginalSize;Bytes.Pop();
    TestFalse(TEXT("Truncated final authored mip refuses"),NativeWadArt::DecodeSurfaceMips(Bytes,Chain));
    TestEqual(TEXT("Decode rejection preserves previous chain"),Chain.OriginalSize,Before);
    Bytes=SyntheticMips({32,16});Bytes.Add(0);
    TestFalse(TEXT("Unexpected trailing data refuses"),NativeWadArt::DecodeSurfaceMips(Bytes,Chain));
    Bytes=SyntheticMips({32,16});Bytes[4+32*16]=8;
    TestFalse(TEXT("Corrupt reduced-level repeated header refuses"),NativeWadArt::DecodeSurfaceMips(Bytes,Chain));
    Bytes={255,255,255,255};
    TestFalse(TEXT("Extreme untrusted dimensions refuse before allocation"),NativeWadArt::DecodeSurfaceMips(Bytes,Chain));
    Bytes=SyntheticMips({16,8});
    TestTrue(TEXT("Original short surface can have an empty final stored level"),NativeWadArt::DecodeSurfaceMips(Bytes,Chain));
    TestEqual(TEXT("Only nonempty prefix can be uploaded to Unreal"),Chain.UploadMipCount(),4);
    TestEqual(TEXT("Header-only fifth source level remains available for validation"),Chain.Mips.Num(),5);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalWorldAuthoredSurfaces,"AAOriginal.WAD.Art.OriginalSurfaceMips",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalWorldAuthoredSurfaces::RunTest(const FString& Parameters)
{
    TArray<uint8> Data;if(!FFileHelper::LoadFileToArray(Data,*(ArtRoot()/TEXT("PICS.RES")))||Data.Num()<14){return false;}
    const int32 Start=int32(DWord(Data,4)),Count=Word(Data,12);
    if(Start<14||int64(Start)+int64(Count)*39>Data.Num()){return false;}
    int32 SurfaceCount=0;bool bDoor=false,bShort=false;
    for(int32 I=0;I<Count;++I)
    {
        const int32 At=Start+39*I;const int32 Offset=int32(DWord(Data,At+18)),Length=int32(DWord(Data,At+22));
        if(Offset<0||Length<0||int64(Offset)+Length>Start){return false;}
        const TConstArrayView<uint8> Entry(Data.GetData()+Offset,Length);FWadIndexedMipChain Chain;
        if(!NativeWadArt::DecodeSurfaceMips(Entry,Chain)){continue;}
        ++SurfaceCount;FString Name;
        for(int32 N=0;N<14&&Data[At+4+N]!=0;++N){Name.AppendChar(TCHAR(Data[At+4+N]));}
        if(Name.Equals(TEXT("BIGDOOR1"),ESearchCase::IgnoreCase))
        {bDoor=true;TestEqual(TEXT("Actual original door mip shape"),Chain.Mips[4].Size,FIntPoint(4,4));}
        if(Name.Equals(TEXT("ICKWALL2"),ESearchCase::IgnoreCase))
        {bShort=true;TestEqual(TEXT("Actual original short surface excludes empty upload level"),Chain.UploadMipCount(),4);}
    }
    TestEqual(TEXT("All507 shipped five-level surface resources decode without RGB regeneration"),SurfaceCount,507);
    TestTrue(TEXT("Original door found"),bDoor);TestTrue(TEXT("Original short surface found"),bShort);return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalWorldMipDecisions,"AAOriginal.WAD.Art.SourceMipDecisions",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalWorldMipDecisions::RunTest(const FString& Parameters)
{
    using namespace NativeWadArt;
    TestEqual(TEXT("Wall step immediately below original threshold stays full size"),SelectSurfaceMip({64,64},false,MipStep-1),0);
    TestEqual(TEXT("Wall threshold chooses one authored shrink"),SelectSurfaceMip({64,64},false,MipStep),1);
    TestEqual(TEXT("Negative wall threshold has same original decision"),SelectSurfaceMip({64,64},false,-MipStep),1);
    TestEqual(TEXT("Four original shrinking steps maximum"),SelectSurfaceMip({64,64},false,MAX_int32),4);
    TestEqual(TEXT("Tiny wall stops before invalid zero-sized source mip"),SelectSurfaceMip({16,8},false,MAX_int32),2);
    TestEqual(TEXT("Wall height256 excludes mip selection"),SelectSurfaceMip({128,256},false,MAX_int32),0);
    TestEqual(TEXT("The same tall texture remains eligible on a plane"),SelectSurfaceMip({128,256},true,MAX_int32),4);
    TestEqual(TEXT("Plane width256 excludes mip selection"),SelectSurfaceMip({256,128},true,MAX_int32),0);
    TestEqual(TEXT("The same wide texture remains eligible on a wall"),SelectSurfaceMip({256,128},false,MAX_int32),4);
    TestEqual(TEXT("Small plane excludes mip selection"),SelectSurfaceMip({32,128},true,MAX_int32),0);
    TestEqual(TEXT("Plane considers both horizontal texture steps"),SelectSurfaceMip({64,64},true,0,MipStep),1);
    TestEqual(TEXT("Wall does not use unrelated second step"),SelectSurfaceMip({64,64},false,0,MipStep),0);
    return true;
}
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalWorldArtProvenance,"AAOriginal.WAD.Art.SourceProvenance",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalWorldArtProvenance::RunTest(const FString& Parameters)
{
    const TArray<uint8> Original={1,2,3,4};FWadArtProvenance Source;
    Source.Palette=EWadArtPalette::WorldStandard;Source.Encoding=EWadArtEncoding::Surface;
    Source.ResourcePath=TEXT("PICS.RES");Source.ArchiveEntry=TEXT("BIGDOOR1");Source.OriginalSize={64,64};
    Source.bAuthoredMips=true;Source.bZeroTransparent=true;Source.SourceSHA1=NativeWadArt::SHA1(Original);
    for(int32 I=0;I<5;++I){Source.MipSHA1.Add(Source.SourceSHA1);}
    TestTrue(TEXT("Complete source-relative world surface provenance valid"),Source.IsValid());
    auto* Texture=NewObject<UTexture2D>();
    TestTrue(TEXT("Texture retains explicit source metadata"),NativeWadArt::RecordSource(*Texture,Source));
    TestTrue(TEXT("Recorded generated hashes are readable without filename guessing"),NativeWadArt::GetSource(*Texture)&&NativeWadArt::GetSource(*Texture)->MipSHA1==Source.MipSHA1);
    TArray<uint8> Bytes;FMemoryWriter Writer(Bytes);FObjectAndNameAsStringProxyArchive Save(Writer,false);
    FWadArtProvenance::StaticStruct()->SerializeItem(Save,&Source,nullptr);
    FWadArtProvenance Restored;FMemoryReader Reader(Bytes);FObjectAndNameAsStringProxyArchive Load(Reader,true);
    FWadArtProvenance::StaticStruct()->SerializeItem(Load,&Restored,nullptr);
    TestTrue(TEXT("Source path/domain/variant/mip provenance serializes independently of host path"),Restored.IsValid()&&Restored.ResourcePath==Source.ResourcePath&&Restored.Palette==Source.Palette&&Restored.MipSHA1==Source.MipSHA1);
    Source.ResourcePath=TEXT("../GAME.PAL");
    TestFalse(TEXT("Invalid source traversal is rejected without replacing recorded provenance"),NativeWadArt::RecordSource(*Texture,Source));
    TestEqual(TEXT("Refused metadata leaves existing texture source intact"),NativeWadArt::GetSource(*Texture)->ResourcePath,FString(TEXT("PICS.RES")));
    return true;
}

#include "Misc/AutomationTest.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Misc/OutputDeviceNull.h"
#include "NativeWadArtRequests.h"
#include "NativeWadArt.h"
#include "NativeWadRules.h"
#include "NativeWadScenerySource.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalArtRequestManifest,"AAOriginal.WAD.Art.ExactImporterRequestManifest",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalArtRequestManifest::RunTest(const FString& Parameters)
{
    FOutputDeviceNull Log;TArray<FWadArtRequest> Requests;
    if(!TestTrue(TEXT("Pure actual importer request manifest resolves without any world/actor setup"),ReadWadCreatureArtRequests(Requests,Log))){return false;}
    TestTrue(TEXT("Campaign/retreat/special/legacy/quest/script/shared requests are included"),Requests.Num()>230);
    TSet<FString> Names;const FString Root=FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal");
    for(const auto& Request:Requests)
    {
        TestFalse(TEXT("No ambiguous native material request key"),Names.Contains(Request.AssetName.ToUpper()));Names.Add(Request.AssetName.ToUpper());
        TestTrue(FString::Printf(TEXT("Exact original CPC is present for %s"),*Request.AssetName),FPaths::FileExists(Root/TEXT("OBJS")/Request.Picture));
        TestTrue(TEXT("Variant table is complete or explicitly absent"),Request.ColorTable.IsEmpty()||Request.ColorTable.Num()==256);
        for(int32 Type:Request.SourceTypes){TestTrue(TEXT("Full identity remains within original unsigned object type"),Type>0&&Type<=MAX_uint16);}
        TestFalse(TEXT("Current actual static creature/quest/projectile frame producers do not bake mirrored pixels"),Request.bMirrored);
    }
    const auto Find=[&](const TCHAR* Name)->const FWadArtRequest*
    {for(const auto& Request:Requests){if(Request.AssetName==Name){return &Request;}}return nullptr;};
    const auto* Druid8=Find(TEXT("Druid33787"));const auto* Druid11=Find(TEXT("Druid46075"));const auto* SharedBolt=Find(TEXT("PackedDruid_Bolt"));
    if(!Druid8||!Druid11||!SharedBolt){return false;}
    TestTrue(TEXT("Both distinct full packed identities retain distinct original palette expansions"),Druid8->SourceTypes.Contains(33787)&&Druid11->SourceTypes.Contains(46075)
        &&Druid8->Picture==Druid11->Picture&&Druid8->ColorTable!=Druid11->ColorTable);
    TestTrue(TEXT("Shared native bolt merges both owners without losing full identities or applying their color tables"),SharedBolt->SourceTypes.Contains(33787)&&SharedBolt->SourceTypes.Contains(46075)&&SharedBolt->ColorTable.IsEmpty());
    const auto* Elymas=Find(TEXT("Elymas"));const auto* Notes=Find(TEXT("ElymasNotes"));const auto* Journal=Find(TEXT("JugurthasJournal"));
    TestTrue(TEXT("Special source50158 is preserved"),Elymas&&Elymas->SourceTypes.Contains(50158)&&Elymas->ColorTable.Num()==256);
    TestTrue(TEXT("Actual quest producer bindings are covered without art-name guessing"),Notes&&Notes->Picture==TEXT("SCRL8/SCRL8.CPC")&&Journal&&Journal->Picture==TEXT("BOOK/BOOK.CPC"));
    for(int32 Type:{4001,4002,4007,4009,4016,NativeWadRules::PoisonCrossbowBolt})
    {
        FWadArtRequest Request;if(!ResolveWadScriptMissileArt(Type,Request)){return false;}
        const auto* Listed=Find(*FString::Printf(TEXT("Missile%d"),Type));
        TestTrue(TEXT("Actual script producer and manifest use one canonical exact request"),Listed&&Listed->Picture==Request.Picture&&Listed->SourceTypes.Contains(Type));
    }
    for(const auto& Ammo:NativeWadRules::CrossbowAmmo)
    {
        FWadArtRequest Request;const FString Name=FString::Printf(TEXT("Ammo%d"),Ammo.Missile);
        TestTrue(TEXT("Every shared crossbow projectile uses actual source table art"),ReadWadSharedProjectileArtRequest(Name,Request)&&Request.Picture==Ammo.Art&&Request.SourceTypes.Contains(Ammo.Missile));
    }
    FWadArtRequest Dead;
    if(!TestTrue(TEXT("Ordinary scenery resolver uses the actual source reader for DEAD01"),ReadWadSceneryArtRequest(TEXT("DEAD01"),1,Dead))){return false;}
    TestEqual(TEXT("The folder uses DEAD.FL and resolves its exact original frame"),Dead.Picture,FString(TEXT("DEAD01/DEDSTIL.CPC")));
    TestEqual(TEXT("The real producer's generated request name is retained"),Dead.AssetName,FString(TEXT("Scenery_DEAD01_1")));
    TestTrue(TEXT("The request keeps the exact full source identity and no invented palette"),Dead.SourceTypes==TArray<int32>{3104}&&Dead.ColorTable.IsEmpty());
    FWadScenerySource Source;FString Error;
    if(!NativeWadScenerySource::Load(TEXT("DEAD01"),Source,Error)){return false;}
    TestTrue(TEXT("Art request agrees with the importer source reader rather than guessing a frame-list filename"),
        Dead.Picture==TEXT("DEAD01/")+Source.PictureFiles.FindChecked(1));
    TestFalse(TEXT("The shared texture is unmirrored; source views carry individual signed bindings"),Dead.bMirrored);
    int32 LegacyRows=0,Decorations=0;
    TMap<int32,FString> Original;
    TArray<FString> ObjectList;
    if(!FFileHelper::LoadFileToStringArray(ObjectList,*(Root/TEXT("OBJS/OBJS.FL")))){return false;}
    for(const FString& Line:ObjectList)
    {
        TArray<FString> Words;Line.ParseIntoArrayWS(Words);int32 Type;
        if(Words.Num()<2||!LexTryParseString(Type,*Words[1])||!Words[0].EndsWith(TEXT(".res"),ESearchCase::IgnoreCase)){continue;}
        Original.Add(Type,FPaths::GetPath(Words[0].Replace(TEXT("\\"),TEXT("/"))).ToUpper());
    }
    const auto CheckLegacy=[&](const TMap<int32,FString>& Rows)
    {
        for(const auto& [Type,Resource]:Rows)
        {
            ++LegacyRows;
            if(!TestEqual(TEXT("Every actual legacy producer row agrees with original OBJS.FL"),Original.FindRef(Type),Resource)){return false;}
            if(!FPaths::FileExists(Root/FString::Printf(TEXT("INVDESC/DES%05d.TXT"),Type))){continue;}
            ++Decorations;FWadScenerySource Definition;FString Failure;
            if(!TestTrue(TEXT("Every legacy inventory decoration uses supported exact source frames"),NativeWadScenerySource::Load(Resource,Definition,Failure))){return false;}
            for(const auto& Frame:Definition.Frames)
            {for(int32 SignedPicture:Frame.Pictures)
                {
                    const int32 Id=FMath::Abs(SignedPicture);FWadArtRequest Request;
                    if(!TestTrue(TEXT("Every actual legacy decoration frame resolves through the shared producer tables"),ReadWadSceneryArtRequest(Resource,Id,Request))){return false;}
                    TestEqual(TEXT("Legacy frame request agrees with the real source reader"),Request.Picture,Resource/Definition.PictureFiles.FindChecked(Id));
                    TestTrue(TEXT("Full legacy identity survives without a guessed palette or baked mirror"),Request.SourceTypes.Contains(Type)&&Request.ColorTable.IsEmpty()&&!Request.bMirrored);
                }}
        }
        return true;
    };
    if(!CheckLegacy(NativeWadScenerySource::GetLegacyBaseResources())||!CheckLegacy(NativeWadScenerySource::GetLegacyRetreatResources())){return false;}
    TestEqual(TEXT("The complete lifted legacy producer tables keep their39 original rows"),LegacyRows,39);
    TestEqual(TEXT("Exactly four legacy inventory decorations need this separate route"),Decorations,4);
    FWadArtRequest Jaw;
    if(!TestTrue(TEXT("L10's exact legacy JAW frame resolves"),ReadWadSceneryArtRequest(TEXT("JAW"),0,Jaw))){return false;}
    TestTrue(TEXT("JAW retains exact type59 and original CPC"),Jaw.SourceTypes==TArray<int32>{59}&&Jaw.Picture==TEXT("JAW/JAW.CPC"));
    FWadArtRequest Sentinel;Sentinel.AssetName=TEXT("unchanged");
    TestFalse(TEXT("An unknown scenery resource refuses instead of searching similarly named art"),ReadWadSceneryArtRequest(TEXT("NotAnOriginalProducerKey"),1,Sentinel));
    TestFalse(TEXT("An unknown original frame refuses"),ReadWadSceneryArtRequest(TEXT("DEAD01"),999,Sentinel));
    TestFalse(TEXT("A signed view reference is not a separate baked texture request"),ReadWadSceneryArtRequest(TEXT("DEAD01"),-1,Sentinel));
    TestEqual(TEXT("Failed scenery lookup leaves the caller's request intact"),Sentinel.AssetName,FString(TEXT("unchanged")));
    TestFalse(TEXT("Unknown exact fixed request refuses"),ReadWadFixedArtRequest(TEXT("NotAnOriginalProducerKey"),Sentinel));
    TestEqual(TEXT("Unknown request never invents a replacement"),Sentinel.AssetName,FString(TEXT("unchanged")));
    TestFalse(TEXT("Unknown script missile refuses without a guessed icon"),ResolveWadScriptMissileArt(65535,Sentinel));
    return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalArtIdentityProvenance,"AAOriginal.WAD.Art.FullIdentityProvenance",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalArtIdentityProvenance::RunTest(const FString& Parameters)
{
    FWadArtProvenance Source;Source.Encoding=EWadArtEncoding::Sprite;Source.Palette=EWadArtPalette::WorldStandard;
    Source.ResourcePath=TEXT("OBJS/DRUID/DRUDW001.CPC");Source.OriginalSize={87,63};Source.SourceSHA1=FString::ChrN(40,TEXT('A'));
    Source.MipSHA1={Source.SourceSHA1};Source.SourceTypes={33787,46075};
    TestTrue(TEXT("Distinct full identities survive one shared unmirrored exact frame record"),Source.IsValid());
    Source.SourceTypes.Add(33787);TestFalse(TEXT("Duplicate per-frame identities are rejected"),Source.IsValid());
    Source.SourceTypes={-19461};TestFalse(TEXT("Signed reinterpretation of full type46075 refuses"),Source.IsValid());
    Source.SourceTypes={1019};TestTrue(TEXT("Ordinary source base remains a separate explicit identity"),Source.IsValid());
    return true;
}

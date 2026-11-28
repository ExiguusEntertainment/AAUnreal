#include "NativeWadArtEditor.h"
#if WITH_EDITOR
#include "NativeWadArt.h"
#include "NativeWadArtRequests.h"
#include "Misc/FeedbackContext.h"
#include "NativeWadHudData.h"
#include "NativeWadItemSource.h"
#include "WadSurfaceTextures.h"
#include "AssetCompilingManager.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/AssetData.h"
#include "Engine/Texture2D.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionPixelNormalWS.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSample.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "Materials/MaterialExpressionVectorParameter.h"
#include "MaterialShared.h"
#include "Misc/FileHelper.h"
#include "HAL/FileManager.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

namespace
{
    const FName MipParameter(TEXT("WadSourceMipInfo"));
    constexpr const TCHAR* MipDescription=TEXT("WAD authored-mip LOD v1");
    const TCHAR* MipCode=TEXT(
        "if(Info.x<=0||Info.y<=0)return 0; float2 P=UV*Info.xy; float2 X=abs(ddx(P)); float2 Y=abs(ddy(P)); "
        "float G=max(max(X.x,X.y),max(Y.x,Y.y)); float L=clamp(floor(log2(max(G,1e-8)/1.75))+1,0,Info.z); "
        "if(abs(Normal.z)>0.5){if(Info.x<64||Info.y<64||Info.x==256)return 0;return L;} "
        "if(Info.y==256)return 0; float2 Mask=Info.xy-1;float Limit=0; "
        "[unroll]for(int I=0;I<4;I++){if(Mask.x<=2||Mask.y<=2)break;Mask=floor(Mask*0.5);Limit++;}return min(L,Limit);");
    FString Root(){return FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Resources/ArtFinal");}
    bool MapName(const FString& Package,FString& Name)
    {
        Name=FPackageName::GetShortName(Package);
        if(Package!=TEXT("/Game/Maps/")+Name||Name.Len()<2||Name[0]!=TEXT('L')||!FPackageName::DoesPackageExist(Package)){return false;}
        for(int32 I=1;I<Name.Len();++I){if(Name[I]<TEXT('0')||Name[I]>TEXT('9')){return false;}}return true;
    }
    FString Json(const TSharedRef<FJsonObject>& Object)
    {FString Out;FJsonSerializer::Serialize(Object,TJsonWriterFactory<>::Create(&Out));return Out;}
    FString PixelHash(TConstArrayView<FColor> Pixels)
    {return NativeWadArt::SHA1(MakeArrayView(reinterpret_cast<const uint8*>(Pixels.GetData()),Pixels.Num()*int32(sizeof(FColor))));}
    TArray<FString> Hashes(UTexture2D& Texture)
    {
        TArray<FString> Out;
        for(int32 I=0;I<Texture.Source.GetNumMips();++I)
        {TArray64<uint8> Bytes;if(!Texture.Source.GetMipData(Bytes,I)){return {};}Out.Add(NativeWadArt::SHA1(MakeArrayView(Bytes.GetData(),int32(Bytes.Num()))));}
        return Out;
    }
    TArray<FAssetData> Assets(const FString& Package)
    {
        TArray<FAssetData> Out;FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get().GetAssetsByPath(FName(*Package),Out,true);
        Out.Sort([](const FAssetData& A,const FAssetData& B){return A.GetObjectPathString()<B.GetObjectPathString();});return Out;
    }
    bool Save(UObject& Asset)
    {
        auto* Package=Asset.GetOutermost();FSavePackageArgs Args;Args.TopLevelFlags=RF_Public|RF_Standalone;
        const FString File=FPackageName::LongPackageNameToFilename(Package->GetName(),FPackageName::GetAssetPackageExtension());
        return UPackage::SavePackage(Package,&Asset,*File,Args);
    }
    TSharedRef<FJsonObject> Describe(UTexture2D& Texture)
    {
        auto Out=MakeShared<FJsonObject>();Out->SetStringField(TEXT("asset"),Texture.GetPathName());
        Out->SetNumberField(TEXT("width"),Texture.Source.GetSizeX());Out->SetNumberField(TEXT("height"),Texture.Source.GetSizeY());
        Out->SetNumberField(TEXT("mip_count"),Texture.Source.GetNumMips());Out->SetNumberField(TEXT("filter"),int32(Texture.Filter));
        Out->SetNumberField(TEXT("mip_settings"),int32(Texture.MipGenSettings));
        Out->SetNumberField(TEXT("address_x"),int32(Texture.AddressX));Out->SetNumberField(TEXT("address_y"),int32(Texture.AddressY));
        Out->SetBoolField(TEXT("srgb"),Texture.SRGB);TArray<TSharedPtr<FJsonValue>> Values;
        for(const auto& Hash:Hashes(Texture)){Values.Add(MakeShared<FJsonValueString>(Hash));}Out->SetArrayField(TEXT("pixel_sha1"),Values);
        if(const auto* Source=NativeWadArt::GetSource(Texture))
        {
            Out->SetStringField(TEXT("source"),Source->ResourcePath);Out->SetStringField(TEXT("entry"),Source->ArchiveEntry);
            Out->SetStringField(TEXT("source_sha1"),Source->SourceSHA1);Out->SetNumberField(TEXT("palette"),int32(Source->Palette));
            Out->SetBoolField(TEXT("authored_mips"),Source->bAuthoredMips);
            TArray<TSharedPtr<FJsonValue>> Types;for(int32 Type:Source->SourceTypes){Types.Add(MakeShared<FJsonValueNumber>(Type));}
            Out->SetArrayField(TEXT("full_source_types"),Types);Out->SetBoolField(TEXT("mirrored_binding"),Source->bMirrored);
            Out->SetStringField(TEXT("variant_sha1"),Source->ColorTable.IsEmpty()?FString():NativeWadArt::SHA1(Source->ColorTable));
            TArray<TSharedPtr<FJsonValue>> Expected;for(const auto& Hash:Source->MipSHA1){Expected.Add(MakeShared<FJsonValueString>(Hash));}
            Out->SetArrayField(TEXT("expected_mip_sha1"),Expected);
        }
        return Out;
    }
    struct FPlan
    {
        UTexture2D* Texture=nullptr;UMaterial* Material=nullptr;UTexture2D* OldSample=nullptr;
        FString TargetPath;TSharedPtr<FJsonObject> Row;FWadArtProvenance Source;TArray<TArray<FColor>> Mips;TArray<FString> Current;
    };
    bool Load(FPlan& Plan,TArray<FString>& GameHashes)
    {
        auto& S=Plan.Source;TArray<uint8> Raw;TArray<FColor> Palette,Game;
        if(!NativeWadArt::LoadPalette(EWadArtPalette::WorldStandard,Palette)||!NativeWadArt::LoadPalette(EWadArtPalette::InterfaceGame,Game)){return false;}
        S.Palette=EWadArtPalette::WorldStandard;S.bAuthoredMips=S.Encoding==EWadArtEncoding::Surface;
        if(S.Encoding==EWadArtEncoding::Surface)
        {
            FString Resolved;if(!ReadWadSurfaceResource(S.ArchiveEntry,Resolved,Raw)){return false;}S.ArchiveEntry=Resolved;S.ResourcePath=TEXT("PICS.RES");
            FWadIndexedMipChain Chain;if(!NativeWadArt::DecodeSurfaceMips(Raw,Chain)){return false;}S.OriginalSize=Chain.OriginalSize;
            for(int32 I=0;I<Chain.UploadMipCount();++I)
            {
                auto& Pixels=Plan.Mips.AddDefaulted_GetRef();TArray<FColor> Old;
                if(!NativeWadArt::ExpandPixels(Chain.Mips[I].Indices,Palette,true,{},Pixels)
                    ||!NativeWadArt::ExpandPixels(Chain.Mips[I].Indices,Game,true,{},Old)){return false;}
                GameHashes.Add(PixelHash(Old));
            }
        }
        else
        {
            // Paths supplied by recorded provenance or original authored mapping, never RGB guesses.
            FWadArtProvenance Check=S;Check.SourceSHA1=FString::ChrN(40,TEXT('0'));Check.MipSHA1={Check.SourceSHA1};
            if(!Check.IsValid()||!FFileHelper::LoadFileToArray(Raw,*(Root()/S.ResourcePath))){return false;}
            TArray<FColor> Old;auto& Pixels=Plan.Mips.AddDefaulted_GetRef();
            const bool bSprite=S.Encoding==EWadArtEncoding::Sprite;
            if(bSprite)
            {
                FString Path=S.ResourcePath;if(!Path.RemoveFromStart(TEXT("OBJS/"))){return false;}
                if(!LoadWadSprite(Path,S.OriginalSize,Pixels,S.ColorTable,EWadArtPalette::WorldStandard)
                    ||!LoadWadSprite(Path,S.OriginalSize,Old,S.ColorTable,EWadArtPalette::InterfaceGame)){return false;}
            }
            else if(S.Encoding==EWadArtEncoding::Backdrop)
            {
                FString Path=S.ResourcePath;if(!Path.RemoveFromStart(TEXT("BACKDROP/"))){return false;}
                if(!LoadWadBackdrop(Path,S.OriginalSize,Pixels,EWadArtPalette::WorldStandard)
                    ||!LoadWadBackdrop(Path,S.OriginalSize,Old,EWadArtPalette::InterfaceGame)){return false;}
            }
            else{return false;}
            GameHashes.Add(PixelHash(Old));
        }
        S.SourceSHA1=NativeWadArt::SHA1(Raw);S.MipSHA1.Reset();
        for(const auto& Mip:Plan.Mips){S.MipSHA1.Add(PixelHash(Mip));}return S.IsValid();
    }
    bool Resolve(UTexture2D& Texture,const FString& Package,const FString& Backdrop,const TMap<FString,FWadArtRequest>& Requests,FWadArtProvenance& Out)
    {
        if(const auto* Saved=NativeWadArt::GetSource(Texture);Saved&&Saved->IsValid())
        {
            Out=*Saved;
            if(const auto* Request=Requests.Find(Texture.GetName().RightChop(2).ToUpper());Request
                &&Out.ResourcePath.Equals(TEXT("OBJS/")+Request->Picture,ESearchCase::IgnoreCase)&&Out.ColorTable==Request->ColorTable)
            {for(int32 Type:Request->SourceTypes){Out.SourceTypes.AddUnique(Type);}Out.SourceTypes.Sort();Out.bMirrored=Request->bMirrored;}
            return Saved->Encoding!=EWadArtEncoding::InterfacePicture;
        }
        const FString Path=Texture.GetOutermost()->GetName(),Name=Texture.GetName();
        Out.OriginalSize={int32(Texture.Source.GetSizeX()),int32(Texture.Source.GetSizeY())};
        if(Path.StartsWith(Package+TEXT("_Assets/Textures/T_")))
        {Out.Encoding=EWadArtEncoding::Surface;Out.ResourcePath=TEXT("PICS.RES");Out.ArchiveEntry=Name.RightChop(2);Out.bZeroTransparent=true;return true;}
        if(Path.StartsWith(Package+TEXT("_Assets/Sky/T_"))&&Name==TEXT("T_")+FPaths::GetBaseFilename(Backdrop).ToUpper())
        {Out.Encoding=EWadArtEncoding::Backdrop;Out.ResourcePath=TEXT("BACKDROP/")+Backdrop;return true;}
        if(Path.StartsWith(Package+TEXT("_Assets/Things/T_")))
        {
            if(const auto* Request=Requests.Find(Name.RightChop(2).ToUpper()))
            {
                Out.Encoding=EWadArtEncoding::Sprite;Out.ResourcePath=TEXT("OBJS/")+Request->Picture;
                Out.ColorTable=Request->ColorTable;Out.SourceTypes=Request->SourceTypes;Out.bMirrored=Request->bMirrored;
                Out.bZeroTransparent=true;return true;
            }
        }
        if(!Path.StartsWith(Package+TEXT("_Assets/Things/T_Scenery_"))){return false;}
        FString Key=Name.RightChop(10);int32 Split=INDEX_NONE;if(!Key.FindLastChar(TEXT('_'),Split)){return false;}
        const FString Resource=Key.Left(Split),Id=Key.Mid(Split+1);int32 Picture=0;if(!LexTryParseString(Picture,*Id)||LexToString(Picture)!=Id||Picture<0){return false;}
        FWadArtRequest Request;if(!ReadWadSceneryArtRequest(Resource,Picture,Request)){return false;}
        Out.Encoding=EWadArtEncoding::Sprite;Out.ResourcePath=TEXT("OBJS/")+Request.Picture;
        Out.ColorTable=Request.ColorTable;Out.SourceTypes=Request.SourceTypes;Out.bMirrored=Request.bMirrored;
        Out.bZeroTransparent=true;return true;
    }
    bool InstallPolicy(UMaterial& Material)
    {
        auto& Expressions=Material.GetExpressionCollection();UMaterialExpressionTextureSampleParameter2D* Sample=nullptr;
        UMaterialExpressionCustom* Existing=nullptr;bool bNameCollision=false;
        for(const auto& Expr:Expressions.Expressions)
        {
            if(auto* Custom=Cast<UMaterialExpressionCustom>(Expr);Custom&&Custom->Description==MipDescription)
            {if(Existing){return false;}Existing=Custom;}
            if(auto* Info=Cast<UMaterialExpressionVectorParameter>(Expr);Info&&Info->ParameterName==MipParameter){bNameCollision=true;}
            if(auto* Texture=Cast<UMaterialExpressionTextureSampleParameter2D>(Expr);Texture&&Texture->ParameterName==TEXT("SurfaceTexture"))
            {if(Sample){return false;}Sample=Texture;}
        }
        if(!Sample){return false;}
        if(Existing)
        {return Existing->Code==MipCode&&Sample->MipValueMode==TMVM_MipLevel&&Sample->MipValue.Expression==Existing&&Existing->Inputs.Num()==3
            &&Existing->Inputs[0].Input.Expression==Sample->Coordinates.Expression&&bNameCollision;}
        if(bNameCollision||Sample->MipValueMode!=TMVM_None||Sample->MipValue.Expression||!Sample->Coordinates.Expression){return false;}
        auto* Info=NewObject<UMaterialExpressionVectorParameter>(&Material);Info->ParameterName=MipParameter;Info->DefaultValue=FLinearColor(0,0,0,0);
        auto* Normal=NewObject<UMaterialExpressionPixelNormalWS>(&Material);auto* Custom=NewObject<UMaterialExpressionCustom>(&Material);
        Custom->Description=MipDescription;Custom->Code=MipCode;Custom->OutputType=CMOT_Float1;
        // The engine constructor supplies one unnamed input. This owned node
        // has exactly the three named inputs used by its shader and validator.
        Custom->Inputs.Reset();
        FCustomInput UV;UV.InputName=TEXT("UV");UV.Input=Sample->Coordinates;Custom->Inputs.Add(UV);
        FCustomInput Dimensions;Dimensions.InputName=TEXT("Info");Dimensions.Input.Connect(0,Info);Custom->Inputs.Add(Dimensions);
        FCustomInput N;N.InputName=TEXT("Normal");N.Input.Connect(0,Normal);Custom->Inputs.Add(N);
        Expressions.AddExpression(Info);Expressions.AddExpression(Normal);Expressions.AddExpression(Custom);
        Sample->MipValueMode=TMVM_MipLevel;Sample->MipValue.Connect(0,Custom);Material.MarkPackageDirty();Material.PostEditChange();return true;
    }
    bool Apply(FPlan& Plan)
    {
        if(!Plan.Texture)
        {
            UPackage* Package=CreatePackage(*Plan.TargetPath);const FName Name(*FPackageName::GetShortName(Plan.TargetPath));
            Plan.Texture=FindObject<UTexture2D>(Package,*Name.ToString());
            if(!Plan.Texture){Plan.Texture=NewObject<UTexture2D>(Package,Name,RF_Public|RF_Standalone);FAssetRegistryModule::AssetCreated(Plan.Texture);}
        }
        auto& Texture=*Plan.Texture;TArray<uint8> Bytes;
        for(const auto& Mip:Plan.Mips){Bytes.Append(reinterpret_cast<const uint8*>(Mip.GetData()),Mip.Num()*int32(sizeof(FColor)));}
        Texture.Source.Init(Plan.Source.OriginalSize.X,Plan.Source.OriginalSize.Y,1,Plan.Mips.Num(),TSF_BGRA8,Bytes.GetData());
        Texture.MipGenSettings=Plan.Source.bAuthoredMips?TMGS_LeaveExistingMips:TMGS_NoMipmaps;Texture.Filter=TF_Nearest;
        if(Plan.Material){Texture.CompressionSettings=Plan.OldSample->CompressionSettings;Texture.SRGB=Plan.OldSample->SRGB;
            Texture.NeverStream=Plan.OldSample->NeverStream;Texture.AddressX=Plan.OldSample->AddressX;Texture.AddressY=Plan.OldSample->AddressY;}
        if(!NativeWadArt::RecordSource(Texture,Plan.Source)){return false;}
        Texture.MarkPackageDirty();Texture.PostEditChange();
        if(Plan.Material)
        {
            for(const auto& Expr:Plan.Material->GetExpressionCollection().Expressions)
            {if(auto* Sample=Cast<UMaterialExpressionTextureSample>(Expr);Sample&&Sample->Texture==Plan.OldSample){Sample->Texture=&Texture;}}
            Plan.Material->MarkPackageDirty();Plan.Material->PostEditChange();
        }
        return true;
    }
}
bool ConfigureWadSurfaceMipMaterial(UMaterial& Material){return InstallPolicy(Material);}
#endif

FString UNativeWadArtEditor::DescribeWorldArt(const FString& MapPackage)
{
#if WITH_EDITOR
    FString Name;if(!MapName(MapPackage,Name)){return TEXT("{}");}auto Report=MakeShared<FJsonObject>();TArray<TSharedPtr<FJsonValue>> Rows;
    auto All=Assets(MapPackage+TEXT("_Assets"));All.Append(Assets(TEXT("/Game/WadWorldArt")));
    for(const auto& Asset:All)
    {if(Asset.AssetClassPath==UTexture2D::StaticClass()->GetClassPathName())
        {if(auto* Texture=Cast<UTexture2D>(Asset.GetAsset())){Rows.Add(MakeShared<FJsonValueObject>(Describe(*Texture)));}}}
    Report->SetArrayField(TEXT("textures"),Rows);
    TArray<TSharedPtr<FJsonValue>> Materials;
    for(const auto& Asset:Assets(MapPackage+TEXT("_Assets/Materials")))
    {
        auto* Material=Cast<UMaterialInstanceConstant>(Asset.GetAsset());if(!Material){continue;}
        UTexture* Texture=nullptr;FLinearColor Info(0,0,0,0);
        if(!Material->GetTextureParameterValue(FMaterialParameterInfo(TEXT("SurfaceTexture")),Texture)){continue;}
        auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("asset"),Material->GetPathName());
        Row->SetStringField(TEXT("texture"),Texture?Texture->GetPathName():FString());
        const bool bInfo=Material->GetVectorParameterValue(FMaterialParameterInfo(MipParameter),Info);
        TArray<TSharedPtr<FJsonValue>> Dimensions;
        for(float Value:{Info.R,Info.G,Info.B,Info.A}){Dimensions.Add(MakeShared<FJsonValueNumber>(Value));}
        Row->SetBoolField(TEXT("has_mip_info"),bInfo);Row->SetArrayField(TEXT("mip_info"),Dimensions);
        bool bPolicy=false;auto* Parent=Material->GetMaterial();
        if(Parent)
        {
            for(const auto& Expr:Parent->GetExpressionCollection().Expressions)
            {if(auto* Custom=Cast<UMaterialExpressionCustom>(Expr);Custom&&Custom->Description==MipDescription&&Custom->Code==MipCode){bPolicy=true;}}
        }
        Row->SetBoolField(TEXT("authored_mip_policy"),bPolicy);Materials.Add(MakeShared<FJsonValueObject>(Row));
    }
    Report->SetArrayField(TEXT("materials"),Materials);return Json(Report);
#else
    return TEXT("{}");
#endif
}
FString UNativeWadArtEditor::DescribeInterfaceArt()
{
#if WITH_EDITOR
    auto Report=MakeShared<FJsonObject>();TArray<TSharedPtr<FJsonValue>> Rows;
    for(const auto& Asset:Assets(TEXT("/Game/WadInterface")))
    {if(Asset.AssetClassPath==UTexture2D::StaticClass()->GetClassPathName())
        {if(auto* Texture=Cast<UTexture2D>(Asset.GetAsset())){Rows.Add(MakeShared<FJsonValueObject>(Describe(*Texture)));}}}
    Report->SetArrayField(TEXT("textures"),Rows);return Json(Report);
#else
    return TEXT("{}");
#endif
}

bool UNativeWadArtEditor::RefreshWorldArt(const FString& MapPackage,const FString& ReportPath,bool bApply)
{
#if WITH_EDITOR
    FString Name;if(!MapName(MapPackage,Name)){return false;}
    const FString FullReport=FPaths::ConvertRelativePathToFull(ReportPath);
    if(ReportPath.IsEmpty()||!FPaths::IsUnderDirectory(FullReport,FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir()))){return false;}
    auto Report=MakeShared<FJsonObject>();Report->SetStringField(TEXT("map"),Name);Report->SetBoolField(TEXT("applied"),bApply);
    Report->SetStringField(TEXT("lod_policy"),TEXT("Authored indexed mip data; GPU screen derivatives approximate original software projection, retaining per-surface size exclusions."));
    TArray<TSharedPtr<FJsonValue>> Rows;TArray<FPlan> Plans;TSet<UTexture2D*> Seen;TSet<UMaterial*> SeenItems;
    TArray<FWadArtRequest> Manifest;if(!ReadWadCreatureArtRequests(Manifest,*GWarn)){return false;}
    TMap<FString,FWadArtRequest> Requests;for(const auto& Request:Manifest){Requests.Add(Request.AssetName.ToUpper(),Request);}
    int32 Overrides=0,Unresolved=0,Already=0,UnusedLegacy=0;bool bSuccess=true;
    FString Backdrop;FString Info;
    const FString MapInfo=FPaths::ProjectDir()/TEXT("Plugins/AmuletsAndArmorPort/External/AmuletsAndArmor/Exe")/(Name+TEXT(".I"));
    if(FFileHelper::LoadFileToString(Info,*MapInfo)){TArray<FString> Fields;Info.ParseIntoArrayWS(Fields);if(Fields.Num()>1){Backdrop=Fields[1];}}
    const auto Note=[&](const FString& Path,const TCHAR* Status,const TCHAR* Why)
    {auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("asset"),Path);Row->SetStringField(TEXT("status"),Status);Row->SetStringField(TEXT("reason"),Why);Rows.Add(MakeShared<FJsonValueObject>(Row));};
    const auto Stage=[&](FPlan Plan)
    {
        UTexture2D* Existing=Plan.Texture?Plan.Texture:Plan.OldSample;
        if(!Existing){return;}TArray<FString> Old;
        if(!Load(Plan,Old)){++Unresolved;Note(Existing->GetPathName(),TEXT("unresolved"),TEXT("source_decode_or_provenance_invalid"));return;}
        const auto Actual=Hashes(*Existing);Plan.Current=Actual;
        if(Plan.Material&&Plan.OldSample&&Plan.OldSample!=Existing)
        {
            const auto OldActual=Hashes(*Plan.OldSample);
            if(OldActual.Num()!=1||(OldActual[0]!=Old[0]&&OldActual[0]!=Plan.Source.MipSHA1[0]))
            {++Overrides;Note(Plan.OldSample->GetPathName(),TEXT("custom_override"),TEXT("world_material_original_sample_pixels_changed"));return;}
        }
        if(Existing->Source.GetSizeX()!=Plan.Source.OriginalSize.X||Existing->Source.GetSizeY()!=Plan.Source.OriginalSize.Y)
        {++Overrides;Note(Existing->GetPathName(),TEXT("custom_override"),TEXT("source_dimensions_differ"));return;}
        const auto* Prior=NativeWadArt::GetSource(*Existing);
        if(Prior&&(!Prior->IsValid()||Actual!=Prior->MipSHA1||Prior->SourceSHA1!=Plan.Source.SourceSHA1))
        {++Overrides;Note(Existing->GetPathName(),TEXT("custom_override"),TEXT("recorded_source_or_generated_pixels_changed"));return;}
        const bool OldBase=Actual.Num()==1&&Old.Num()&&Actual[0]==Old[0];
        const bool OldChain=Actual==Old;const bool WorldChain=Actual==Plan.Source.MipSHA1;
        if((!OldBase&&!OldChain&&!WorldChain)||Existing->Source.GetFormat()!=TSF_BGRA8||!Existing->SRGB||Existing->CompressionSettings!=TC_EditorIcon
            ||Existing->LODBias!=0||(Existing->Filter!=TF_Nearest&&!(Plan.Source.Encoding==EWadArtEncoding::Backdrop&&Existing->Filter==TF_Default))
            ||(Existing->MipGenSettings!=TMGS_NoMipmaps&&Existing->MipGenSettings!=TMGS_LeaveExistingMips))
        {++Overrides;Note(Existing->GetPathName(),TEXT("custom_override"),TEXT("not_exact_known_original_pixels_or_sampling_defaults"));return;}
        const bool Correct=WorldChain&&Prior&&Prior->Palette==EWadArtPalette::WorldStandard
            &&Existing->MipGenSettings==(Plan.Source.bAuthoredMips?TMGS_LeaveExistingMips:TMGS_NoMipmaps)&&Existing->Filter==TF_Nearest
            &&Prior->SourceTypes==Plan.Source.SourceTypes&&Prior->bMirrored==Plan.Source.bMirrored;
        if(Correct&&(!Plan.Material||Plan.Texture==Plan.OldSample))
        {++Already;Note(Existing->GetPathName(),TEXT("unchanged"),TEXT("exact_world_source_already_saved"));return;}
        auto Row=MakeShared<FJsonObject>();Row->SetStringField(TEXT("asset"),Existing->GetPathName());
        Row->SetStringField(TEXT("target"),Plan.Texture?Plan.Texture->GetPathName():Plan.TargetPath);
        Row->SetStringField(TEXT("source"),Plan.Source.ResourcePath);Row->SetStringField(TEXT("entry"),Plan.Source.ArchiveEntry);
        Row->SetStringField(TEXT("status"),bApply?TEXT("planned_update"):TEXT("dry_run"));Row->SetNumberField(TEXT("authored_mips"),Plan.Mips.Num());
        TArray<TSharedPtr<FJsonValue>> Expected;for(const auto& Hash:Plan.Source.MipSHA1){Expected.Add(MakeShared<FJsonValueString>(Hash));}
        Row->SetArrayField(TEXT("expected_mip_sha1"),Expected);
        TArray<TSharedPtr<FJsonValue>> Types;for(int32 Type:Plan.Source.SourceTypes){Types.Add(MakeShared<FJsonValueNumber>(Type));}
        Row->SetArrayField(TEXT("expected_source_types"),Types);Row->SetBoolField(TEXT("expected_mirrored_binding"),Plan.Source.bMirrored);
        Rows.Add(MakeShared<FJsonValueObject>(Row));Plan.Row=Row;Plans.Add(MoveTemp(Plan));
    };
    const auto MapAssets=Assets(MapPackage+TEXT("_Assets"));
    for(const auto& Asset:MapAssets)
    {
        if(Asset.AssetClassPath!=UTexture2D::StaticClass()->GetClassPathName()){continue;}
        auto* Texture=Cast<UTexture2D>(Asset.GetAsset());if(!Texture||Seen.Contains(Texture)){continue;}Seen.Add(Texture);
        const FString Package=Texture->GetOutermost()->GetName();
        // Activity journals and other interface resources under map roots are excluded.
        if(!Package.StartsWith(MapPackage+TEXT("_Assets/Textures/"))&&!Package.StartsWith(MapPackage+TEXT("_Assets/Sky/"))
            &&!Package.StartsWith(MapPackage+TEXT("_Assets/Things/"))){continue;}
        FPlan Plan;Plan.Texture=Texture;
        if(!Resolve(*Texture,MapPackage,Backdrop,Requests,Plan.Source))
        {
            // One prototype pair predates the retained importer history. Do not
            // infer its source from its name or pixels. Preserve it only while
            // the complete saved reference graph proves the material unused.
            if(Package==TEXT("/Game/Maps/L72_Assets/Things/T_PlasmaBolt"))
            {
                auto& Registry=FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry")).Get();
                TArray<FName> TextureRefs,MaterialRefs;
                const FName MaterialPackage(TEXT("/Game/Maps/L72_Assets/Things/M_PlasmaBolt"));
                Registry.GetReferencers(FName(*Package),TextureRefs,UE::AssetRegistry::EDependencyCategory::All);
                Registry.GetReferencers(MaterialPackage,MaterialRefs,UE::AssetRegistry::EDependencyCategory::All);
                if(TextureRefs.Num()==1&&TextureRefs[0]==MaterialPackage&&MaterialRefs.IsEmpty())
                {++UnusedLegacy;Note(Texture->GetPathName(),TEXT("retained_unused_legacy"),TEXT("only_material_owner_has_no_saved_referencers_source_unknown"));continue;}
            }
            ++Unresolved;Note(Texture->GetPathName(),TEXT("unresolved"),TEXT("no_exact_saved_or_authored_frame_identity"));continue;
        }
        Stage(MoveTemp(Plan));
    }
    // Item world materials may sample the SAME original texture used by approved
    // interface icons. Keep those textures untouched and retarget only known world samples.
    const auto* Catalog=UNativeWadHudData::LoadCatalog();
    if(Catalog)
    {
        TSet<int32> Types;for(const auto& Pair:Catalog->Items){Types.Add(Pair.Key);}TMap<int32,FWadAuthoredItem> Items;
        if(!ReadWadAuthoredItems(Types,Items,*GWarn)){bSuccess=false;}
        for(const auto& Pair:Items)
        {
            const auto* Details=Catalog->ItemDetails.Find(Pair.Key);if(!Details||Details->WorldMaterial.IsNull()){continue;}
            auto* Material=Cast<UMaterial>(Details->WorldMaterial.LoadSynchronous());if(!Material||SeenItems.Contains(Material)){continue;}SeenItems.Add(Material);
            if(Material->GetOutermost()->GetName()!=Pair.Value.WorldMaterialPackage())
            {++Overrides;Note(Material->GetPathName(),TEXT("custom_override"),TEXT("item_world_material_binding_is_not_authored"));continue;}
            UMaterialExpressionTextureSample* Sample=nullptr;int32 Samples=0;
            for(const auto& Expr:Material->GetExpressionCollection().Expressions)
            {if(auto* Found=Cast<UMaterialExpressionTextureSample>(Expr)){Sample=Found;++Samples;}}
            auto* Original=Sample?Cast<UTexture2D>(Sample->Texture):nullptr;
            if(Samples!=1||!Original)
            {++Overrides;Note(Material->GetPathName(),TEXT("custom_override"),TEXT("world_material_has_custom_texture_graph"));continue;}
            FPlan Plan;Plan.Material=Material;Plan.OldSample=Original;Plan.Source.Encoding=EWadArtEncoding::Sprite;
            Plan.Source.ResourcePath=TEXT("OBJS/")+Pair.Value.Picture;Plan.Source.OriginalSize={int32(Original->Source.GetSizeX()),int32(Original->Source.GetSizeY())};
            Plan.Source.bZeroTransparent=true;
            Plan.TargetPath=TEXT("/Game/WadWorldArt/Items/T_")+Pair.Value.AssetName();
            Plan.Texture=LoadObject<UTexture2D>(nullptr,*(Plan.TargetPath+TEXT(".")+FPackageName::GetShortName(Plan.TargetPath)),nullptr,LOAD_NoWarn);
            // A source family with no separate native variant material remains a
            // recorded empty mapping, not a claim that packed item recoloring is restored.
            if(const auto* Prior=NativeWadArt::GetSource(*Original);Prior&&Prior->IsValid()&&Prior->ResourcePath==Plan.Source.ResourcePath)
            {Plan.Source.ColorTable=Prior->ColorTable;}
            Stage(MoveTemp(Plan));
        }
    }
    if(Catalog)
    {
        const auto Shared=[&](UMaterialInterface* Interface,const FString& Key)
        {
            auto* Material=Cast<UMaterial>(Interface);if(!Material||SeenItems.Contains(Material)){return;}
            FWadArtRequest Request;if(!ReadWadSharedProjectileArtRequest(Key,Request)){++Unresolved;Note(Material->GetPathName(),TEXT("unresolved"),TEXT("shared_projectile_request_missing"));return;}
            SeenItems.Add(Material);
            if(Material->GetOutermost()->GetName()!=TEXT("/Game/WadInterface/Inventory/Things/M_")+Request.AssetName)
            {++Overrides;Note(Material->GetPathName(),TEXT("custom_override"),TEXT("shared_projectile_material_override"));return;}
            UMaterialExpressionTextureSample* Sample=nullptr;int32 Count=0;
            for(const auto& Expr:Material->GetExpressionCollection().Expressions)
            {if(auto* Found=Cast<UMaterialExpressionTextureSample>(Expr)){Sample=Found;++Count;}}
            auto* Original=Sample?Cast<UTexture2D>(Sample->Texture):nullptr;
            if(Count!=1||!Original){++Overrides;Note(Material->GetPathName(),TEXT("custom_override"),TEXT("shared_projectile_texture_graph_override"));return;}
            FPlan Plan;Plan.Material=Material;Plan.OldSample=Original;Plan.Source.Encoding=EWadArtEncoding::Sprite;
            Plan.Source.ResourcePath=TEXT("OBJS/")+Request.Picture;Plan.Source.bZeroTransparent=true;Plan.Source.SourceTypes=Request.SourceTypes;
            Plan.Source.ColorTable=Request.ColorTable;Plan.Source.bMirrored=Request.bMirrored;
            Plan.Source.OriginalSize={int32(Original->Source.GetSizeX()),int32(Original->Source.GetSizeY())};
            Plan.TargetPath=TEXT("/Game/WadWorldArt/Items/T_")+Request.AssetName;
            Plan.Texture=LoadObject<UTexture2D>(nullptr,*(Plan.TargetPath+TEXT(".")+Request.AssetName),nullptr,LOAD_NoWarn);Stage(MoveTemp(Plan));
        };
        Shared(Catalog->PlasmaProjectile,TEXT("PlasmaProjectile"));
        for(const auto& Pair:Catalog->AmmoProjectiles){Shared(Pair.Value,FString::Printf(TEXT("Ammo%d"),Pair.Key));}
        for(const auto& Pair:Catalog->WandProjectileMaterials){Shared(Pair.Value,FString::Printf(TEXT("Wand%d"),Pair.Key));}
    }
    TArray<UMaterial*> Parents;
    for(const TCHAR* Prefix:{TEXT("M_AANative"),TEXT("M_AAWall")})for(const TCHAR* Suffix:{TEXT("Opaque"),TEXT("Masked"),TEXT("Translucent")})
    {
        const FString Asset=FString(Prefix)+Suffix;auto* Parent=LoadObject<UMaterial>(nullptr,*(TEXT("/Game/AAReview/")+Asset+TEXT(".")+Asset),nullptr,LOAD_NoWarn);
        if(Parent){Parents.Add(Parent);}
    }
    if(bApply&&bSuccess)
    {
        // Material outputs, multipliers, exposure and shading models stay intact.
        // Only existing original surface sample LOD inputs gain this policy.
        for(auto* Parent:Parents){if(!InstallPolicy(*Parent)){Note(Parent->GetPathName(),TEXT("custom_override"),TEXT("surface_mip_graph_was_customized"));bSuccess=false;}}
        if(bSuccess)
        {
            for(auto& Plan:Plans){if(!Apply(Plan)){bSuccess=false;break;}}
            FAssetCompilingManager::Get().FinishAllCompilation();
            for(auto& Plan:Plans)
            {
                if(!Plan.Texture||Hashes(*Plan.Texture)!=Plan.Source.MipSHA1||!Save(*Plan.Texture)||(Plan.Material&&!Save(*Plan.Material)))
                {bSuccess=false;Note(Plan.TargetPath,TEXT("failed"),TEXT("positive_texture_or_material_save_failed"));break;}
            }
            // Per-instance original dimensions permit the same surface resource to
            // obey different wall versus floor/ceiling size exclusions at runtime.
            for(const auto& Asset:MapAssets)
            {
                if(Asset.AssetClassPath!=UMaterialInstanceConstant::StaticClass()->GetClassPathName()){continue;}
                auto* Material=Cast<UMaterialInstanceConstant>(Asset.GetAsset());if(!Material||!Parents.Contains(Material->GetMaterial())){continue;}
                UTexture* Texture=nullptr;
                if(!Material->GetTextureParameterValue(FMaterialParameterInfo(TEXT("SurfaceTexture")),Texture)){continue;}
                auto* Picture=Cast<UTexture2D>(Texture);const auto* Source=Picture?NativeWadArt::GetSource(*Picture):nullptr;
                if(!Source||!Source->IsValid()||Source->Palette!=EWadArtPalette::WorldStandard||Source->Encoding!=EWadArtEncoding::Surface){continue;}
                const FLinearColor Desired(float(Source->OriginalSize.X),float(Source->OriginalSize.Y),float(Source->MipSHA1.Num()-1),0);
                FLinearColor Previous;
                if(Material->GetVectorParameterValue(FMaterialParameterInfo(MipParameter),Previous,true)&&Previous!=Desired)
                {++Overrides;Note(Material->GetPathName(),TEXT("custom_override"),TEXT("per_instance_mip_policy_override_retained"));continue;}
                Material->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(MipParameter),Desired);
                Material->MarkPackageDirty();Material->PostEditChange();if(!Save(*Material)){bSuccess=false;break;}
            }
            for(auto* Parent:Parents){if(!Save(*Parent)){bSuccess=false;}}
        }
    }
    if(bApply){for(auto& Plan:Plans){if(Plan.Row){Plan.Row->SetStringField(TEXT("status"),bSuccess?TEXT("updated"):TEXT("unverified_save_failure"));}}}
    Report->SetArrayField(TEXT("assets"),Rows);Report->SetNumberField(TEXT("updated"),Plans.Num());Report->SetNumberField(TEXT("already_world"),Already);
    Report->SetNumberField(TEXT("custom_overrides"),Overrides);Report->SetNumberField(TEXT("unresolved_sources"),Unresolved);
    Report->SetNumberField(TEXT("retained_unused_legacy"),UnusedLegacy);
    Report->SetBoolField(TEXT("complete_source_coverage"),Unresolved==0);Report->SetBoolField(TEXT("positive_saves"),bApply&&bSuccess);
    Report->SetStringField(TEXT("item_variant_gap"),TEXT("Existing shared base-item material identities are preserved; missing distinct packed-item variant bindings are not claimed restored."));
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(FullReport),true);
    if(!FFileHelper::SaveStringToFile(Json(Report),*FullReport)){return false;}return bSuccess;
#else
    return false;
#endif
}

#include "Misc/AutomationTest.h"
#include "NativeWadArt.h"
#include "Engine/Texture2D.h"
#include "TextureResource.h"
#include "../../AAOriginal/Private/WadSurfaceTextures.h"
#if WITH_EDITOR
#include "NativeWadArtEditor.h"
#include "Materials/Material.h"
#include "Materials/MaterialExpressionConstant3Vector.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionTextureCoordinate.h"
#include "Materials/MaterialExpressionTextureSampleParameter2D.h"
#include "MaterialShared.h"
#endif

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalWorldSurfaceUpload,"AAOriginal.WAD.Art.ExactWorldMipUpload",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalWorldSurfaceUpload::RunTest(const FString& Parameters)
{
    FString Resolved;TArray<uint8> Bytes;FWadIndexedMipChain Source;TArray<FColor> Palette;
    if(!ReadWadSurfaceResource(TEXT("BIGDOOR1"),Resolved,Bytes)||!NativeWadArt::DecodeSurfaceMips(Bytes,Source)
        ||!NativeWadArt::LoadPalette(EWadArtPalette::WorldStandard,Palette)){return false;}
    auto* Texture=LoadWadSurfaceTexture(TEXT("BIGDOOR1"));if(!Texture){return false;}
    auto* Platform=Texture->GetPlatformData();if(!TestNotNull(TEXT("Native preview mip data"),Platform)){return false;}
    TestEqual(TEXT("Five exact source levels reach native GPU upload"),Platform->Mips.Num(),5);
    for(int32 I=0;I<Platform->Mips.Num();++I)
    {
        auto& Mip=Platform->Mips[I];const auto& Original=Source.Mips[I];TArray<FColor> Expected;
        if(!NativeWadArt::ExpandPixels(Original.Indices,Palette,true,{},Expected)){return false;}
        TestEqual(TEXT("Uploaded authored width"),Mip.SizeX,Original.Size.X);TestEqual(TEXT("Uploaded authored height"),Mip.SizeY,Original.Size.Y);
        const auto* Pixels=Mip.BulkData.LockReadOnly();
        const bool Same=Mip.BulkData.GetBulkDataSize()==Expected.Num()*int32(sizeof(FColor))
            &&FMemory::Memcmp(Pixels,Expected.GetData(),Expected.Num()*sizeof(FColor))==0;
        Mip.BulkData.Unlock();TestTrue(FString::Printf(TEXT("Mip%d is exact source indexed expansion, not generated RGB averaging"),I),Same);
    }
    Bytes.Pop();TestNull(TEXT("Malformed authored chain cannot create a partial native texture"),NativeWadArt::CreateSurfaceTexture(Bytes,Resolved));
    return true;
}
#if WITH_EDITOR
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FAAOriginalWorldMipMaterial,"AAOriginal.WAD.Art.MaterialLodPreservesOutputs",
    EAutomationTestFlags::EditorContext|EAutomationTestFlags::EngineFilter)
bool FAAOriginalWorldMipMaterial::RunTest(const FString& Parameters)
{
    auto* Material=NewObject<UMaterial>();auto& E=Material->GetExpressionCollection();
    auto* UV=NewObject<UMaterialExpressionTextureCoordinate>(Material);auto* Sample=NewObject<UMaterialExpressionTextureSampleParameter2D>(Material);
    Sample->ParameterName=TEXT("SurfaceTexture");Sample->Coordinates.Connect(0,UV);
    auto* Light=NewObject<UMaterialExpressionConstant3Vector>(Material);Light->Constant=FLinearColor(0.17f,0.31f,0.63f);
    E.AddExpression(UV);E.AddExpression(Sample);E.AddExpression(Light);Material->GetEditorOnlyData()->EmissiveColor.Connect(0,Light);
    const auto Original=Material->GetEditorOnlyData()->EmissiveColor.Expression;
    TestTrue(TEXT("Known original texture sample receives source policy"),ConfigureWadSurfaceMipMaterial(*Material));
    TestTrue(TEXT("Source policy leaves authored emissive output and gain untouched"),Material->GetEditorOnlyData()->EmissiveColor.Expression==Original&&Light->Constant==FLinearColor(0.17f,0.31f,0.63f));
    TestTrue(TEXT("Source coordinates are retained"),Sample->Coordinates.Expression==UV);
    const int32 Count=E.Expressions.Num();
    TestTrue(TEXT("Repeated maintenance is idempotent"),ConfigureWadSurfaceMipMaterial(*Material));TestEqual(TEXT("Repeated policy creates no extra nodes"),E.Expressions.Num(),Count);
    Sample->MipValue.Expression=nullptr;
    TestFalse(TEXT("User-disconnected policy is preserved rather than silently overwritten"),ConfigureWadSurfaceMipMaterial(*Material));
    TestEqual(TEXT("Refused altered graph adds no nodes"),E.Expressions.Num(),Count);
    auto* Custom=NewObject<UMaterial>();auto& CE=Custom->GetExpressionCollection();auto* CS=NewObject<UMaterialExpressionTextureSampleParameter2D>(Custom);
    auto* CU=NewObject<UMaterialExpressionTextureCoordinate>(Custom);CS->ParameterName=TEXT("SurfaceTexture");CS->Coordinates.Connect(0,CU);CS->MipValueMode=TMVM_MipBias;
    CE.AddExpression(CS);CE.AddExpression(CU);const int32 Before=CE.Expressions.Num();
    TestFalse(TEXT("Existing user mip policy refuses native replacement"),ConfigureWadSurfaceMipMaterial(*Custom));
    TestEqual(TEXT("Custom mip mode remains untouched"),int32(CS->MipValueMode),int32(TMVM_MipBias));TestEqual(TEXT("Custom graph node count unchanged"),CE.Expressions.Num(),Before);
    return true;
}
#endif

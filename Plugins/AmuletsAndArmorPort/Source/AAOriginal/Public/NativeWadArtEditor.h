#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NativeWadArtEditor.generated.h"

UCLASS()
class AAORIGINAL_API UNativeWadArtEditor : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    // Editor maintenance only. Positive return proves all planned saves succeeded;
    // the JSON separately reports unresolved source identities/custom overrides.
    UFUNCTION(BlueprintCallable,Category="WAD|Review")
    static bool RefreshWorldArt(const FString& MapPackage,const FString& ReportPath,bool bApply=false);
    UFUNCTION(BlueprintCallable,Category="WAD|Review")
    static FString DescribeWorldArt(const FString& MapPackage);
    UFUNCTION(BlueprintCallable,Category="WAD|Review")
    static FString DescribeInterfaceArt();
};

#if WITH_EDITOR
class UMaterial;
AAORIGINAL_API bool ConfigureWadSurfaceMipMaterial(UMaterial& Material);
#endif

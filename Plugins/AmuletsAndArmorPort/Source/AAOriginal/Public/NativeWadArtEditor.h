#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "NativeWadArtEditor.generated.h"
class UPrimitiveComponent;

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
    // Read actual renderer inputs through the public native API; Python's
    // editor-property accessor intentionally protects transient live data.
    UFUNCTION(BlueprintCallable,Category="WAD|Review")
    static FString DescribePrimitiveData(const UPrimitiveComponent* Component);
};

#if WITH_EDITOR
class UMaterial;
AAORIGINAL_API bool ConfigureWadSurfaceMipMaterial(UMaterial& Material);
#endif

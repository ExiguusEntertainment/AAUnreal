using UnrealBuildTool;

public class AAOriginalTests : ModuleRules
{
    public AAOriginalTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core", "CoreUObject", "Engine", "AAOriginal", "AutomationController"
        });

        // Editor-only module
        if (Target.Type != TargetType.Editor)
        {
            bPrecompile = false;
        }
    }
}

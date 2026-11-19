using UnrealBuildTool;

public class AAOriginalTests : ModuleRules
{
    public AAOriginalTests(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "AAOriginal",
            "AutomationController"
        });

        // Needed for IPluginManager (Interfaces/IPluginManager.h) implementation
        PrivateDependencyModuleNames.AddRange(new[]
        {
            "Projects",
            "ProceduralMeshComponent"
        });

        if (Target.Type != TargetType.Editor)
        {
            bPrecompile = false;
        }
    }
}

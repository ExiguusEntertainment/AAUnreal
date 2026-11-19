using UnrealBuildTool;
using System.IO;

public class AAOriginal : ModuleRules
{
    public AAOriginal(ReadOnlyTargetRules Target) : base(Target)
    {
        // C-friendly defaults to avoid friction with legacy code.
        PCHUsage = PCHUsageMode.NoPCHs;
        bUseUnity = false;
        bEnableExceptions = false;
        bUseRTTI = false;

        PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Engine", "ProceduralMeshComponent" });
        PrivateDependencyModuleNames.AddRange(new[] { "ApplicationCore", "Projects", "GeometryCore", "ImageWrapper", "InputCore", "Json", "RenderCore" });

        // Editor-only helpers for map building UI/actions
        if (Target.bBuildEditor)
        {
            PrivateDependencyModuleNames.AddRange(new[] { "UnrealEd", "MeshDescription", "StaticMeshDescription", "AssetRegistry" });
        }

        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));

        // Do NOT add the legacy External AmuletsAndArmor include path to the build.
        // External legacy sources are reference-only per project rules.

        var PluginRoot = Path.GetFullPath(Path.Combine(ModuleDirectory, "..", ".."));
        var ExternalRoot = Path.Combine(PluginRoot, "External", "AmuletsAndArmor");

        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            // Optional: if we ever ship a static lib version of reimplemented code it can be added here.
            var LibDir = Path.Combine(ExternalRoot, "lib", "Win64", Target.Configuration.ToString());
            var LibPath = Path.Combine(LibDir, "AmuletsAndArmor.lib");
            // Intentionally NOT linking the legacy lib unless explicitly restored later.
            // if (File.Exists(LibPath)) { PublicAdditionalLibraries.Add(LibPath); }
        }
    }
}

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

        PublicDependencyModuleNames.AddRange(new[] { "Core" });

        PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "Public"));

        // If you place the upstream headers under External/AmuletsAndArmor/include,
        // add that include path so UE can see them without copying.
        string ExternalRoot = Path.Combine(ModuleDirectory, "External", "AmuletsAndArmor");
        string IncludeDir = Path.Combine(ExternalRoot, "include");
        if (Directory.Exists(IncludeDir))
        {
            PublicSystemIncludePaths.Add(IncludeDir);
        }

        // Optional: If you prebuild a static lib of the original code, drop it under:
        // External/AmuletsAndArmor/lib/Win64/{Debug|Development|Shipping}/AmuletsAndArmor.lib
        if (Target.Platform == UnrealTargetPlatform.Win64)
        {
            string LibDir = Path.Combine(ExternalRoot, "lib", "Win64", Target.Configuration.ToString());
            string LibPath = Path.Combine(LibDir, "AmuletsAndArmor.lib");
            if (File.Exists(LibPath))
            {
                PublicAdditionalLibraries.Add(LibPath);
            }
        }

        // Later, if you decide to compile .c files directly under this module,
        // put them in Source/AAOriginal/Private/AmuletsAndArmor and UBT will compile them as C.
    }
}

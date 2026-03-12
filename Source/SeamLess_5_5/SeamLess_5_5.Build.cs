using UnrealBuildTool;

public class SeamLess_5_5 : ModuleRules
{
    public SeamLess_5_5(ReadOnlyTargetRules Target) : base(Target)
    {
        PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

        PublicDependencyModuleNames.AddRange(new string[]
        {
            "Core",
            "CoreUObject",
            "Engine",
            "InputCore",
            "EnhancedInput"
        });

        PrivateDependencyModuleNames.AddRange(new string[]
        {
            "RenderCore",        // 추가
            "Renderer",          // 추가
            "RHI",               // 추가
        });
    }
}
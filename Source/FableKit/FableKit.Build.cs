using UnrealBuildTool;

public class FableKit : ModuleRules
{
	public FableKit(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"Json",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"BlueprintGraph",
			"AssetTools",
			"AssetRegistry",
			"UMG",
			"UMGEditor",
		});
	}
}

using UnrealBuildTool;

public class BlueprintFormatter : ModuleRules
{
	public BlueprintFormatter(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = ModuleRules.PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(new string[]
		{
			"Core",
			"CoreUObject",
			"Engine",
			"InputCore",
			"Slate",
			"SlateCore",
			"UnrealEd",
			"EditorFramework",
			"ToolMenus",
			"GraphEditor",
			"Kismet",
			"BlueprintGraph",
		});
	}
}

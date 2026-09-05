using UnrealBuildTool;

public class UnrealAISampleEditor : ModuleRules
{
	public UnrealAISampleEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PrivateDependencyModuleNames.AddRange(
			new[]
			{
				"AssetRegistry",
				"BlueprintGraph",
				"Core",
				"CoreUObject",
				"Engine",
				"Kismet",
				"UMG",
				"UMGEditor",
				"UnrealAI",
				"UnrealAISample",
				"UnrealEd"
			});
	}
}

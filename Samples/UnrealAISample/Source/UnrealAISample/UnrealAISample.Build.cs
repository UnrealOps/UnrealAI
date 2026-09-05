using UnrealBuildTool;

public class UnrealAISample : ModuleRules
{
	public UnrealAISample(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;

		PublicDependencyModuleNames.AddRange(
			new[]
			{
				"Core",
				"CoreUObject",
				"Engine",
				"UMG",
				"UnrealAI"
			});
	}
}

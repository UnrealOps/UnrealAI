using UnrealBuildTool;

public class UnrealAI : ModuleRules
{
	public UnrealAI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;

		PublicDependencyModuleNames.AddRange(new string[] {
			"Core",
			"UnrealAITransport",
			"UnrealAIAccess",
			"CoreUObject",
			"Engine",
			"DeveloperSettings",
			"HTTP"
		});

		PrivateDependencyModuleNames.AddRange(new string[] {
			"Json",
			"JsonUtilities"
		});
	}
}

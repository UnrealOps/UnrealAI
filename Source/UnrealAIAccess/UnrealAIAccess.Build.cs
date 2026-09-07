// Copyright UnrealOps. All Rights Reserved.
using UnrealBuildTool;
public class UnrealAIAccess : ModuleRules
{
	public UnrealAIAccess(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "Json" });
	}
}

// Copyright UnrealOps. All Rights Reserved.
using UnrealBuildTool;
public class UnrealAIAuthOpenAI : ModuleRules
{
	public UnrealAIAuthOpenAI(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bUseUnity = false;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "UnrealAIAccess", "UnrealAI", "UnrealAIAuth" });
		PrivateDependencyModuleNames.Add("Json");
	}
}

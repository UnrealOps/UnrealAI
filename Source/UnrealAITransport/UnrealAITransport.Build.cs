// Copyright UnrealOps. All Rights Reserved.
using UnrealBuildTool;
public class UnrealAITransport : ModuleRules
{
	public UnrealAITransport(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		bEnableObjCAutomaticReferenceCounting = true;
		PublicDependencyModuleNames.AddRange(new[] { "Core", "UnrealAIAccess", "HTTP" });
		if (Target.IsInPlatformGroup(UnrealPlatformGroup.Apple)) { PublicFrameworks.Add("Foundation"); }
	}
}

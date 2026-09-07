// Copyright EngineWorks. All Rights Reserved.
using UnrealBuildTool;
public class UnrealAIAuthEditor : ModuleRules
{
	public UnrealAIAuthEditor(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		PrivateDependencyModuleNames.AddRange(new[] { "Core", "CoreUObject", "UnrealAIAccess", "UnrealAIAuth", "Slate", "SlateCore", "ToolMenus" });
	}
}

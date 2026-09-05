using UnrealBuildTool;

public class UnrealAISampleTarget : TargetRules
{
	public UnrealAISampleTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Game;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.Add("UnrealAISample");
	}
}

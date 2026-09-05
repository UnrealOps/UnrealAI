using UnrealBuildTool;

public class UnrealAISampleEditorTarget : TargetRules
{
	public UnrealAISampleEditorTarget(TargetInfo Target) : base(Target)
	{
		Type = TargetType.Editor;
		DefaultBuildSettings = BuildSettingsVersion.V6;
		IncludeOrderVersion = EngineIncludeOrderVersion.Unreal5_7;
		ExtraModuleNames.AddRange(new[] { "UnrealAISample", "UnrealAISampleEditor" });
	}
}

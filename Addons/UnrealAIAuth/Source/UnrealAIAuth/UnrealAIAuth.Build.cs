// Copyright EngineWorks. All Rights Reserved.

using UnrealBuildTool;

public class UnrealAIAuth : ModuleRules
{
	public UnrealAIAuth(ReadOnlyTargetRules Target) : base(Target)
	{
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		bEnableObjCAutomaticReferenceCounting = true;
		PublicDependencyModuleNames.AddRange(new[]
		{
			"Core",
			"UnrealAIAccess"
		});
		PrivateDependencyModuleNames.AddRange(new[] { "Sockets", "UnrealAI", "UnrealAITransport" });

		bool bOAuthOpenSslSupported = Target.IsInPlatformGroup(UnrealPlatformGroup.Apple) ||
			Target.Platform.IsInGroup(UnrealPlatformGroup.Windows) ||
			Target.IsInPlatformGroup(UnrealPlatformGroup.Unix) ||
			Target.Platform == UnrealTargetPlatform.Android;
		PrivateDefinitions.Add("UNREALAI_OAUTH_OPENSSL=" + (bOAuthOpenSslSupported ? "1" : "0"));
		if (bOAuthOpenSslSupported)
		{
			AddEngineThirdPartyPrivateStaticDependencies(Target, "OpenSSL");
		}

		if (Target.IsInPlatformGroup(UnrealPlatformGroup.Apple))
		{
			PublicFrameworks.AddRange(new[]
			{
				"CoreFoundation",
				"Security",
				"Foundation"
			});
		}
	}
}

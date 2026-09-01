#include "UnrealAISettings.h"

#include "HAL/PlatformMisc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"

#define LOCTEXT_NAMESPACE "UnrealAISettings"

namespace UnrealAISettingsPrivate
{
	bool bProjectEnvLoadAttempted = false;
	int32 LastProjectEnvVariablesLoaded = 0;
	FString LastProjectEnvMessage;
	TSet<FString> LoadedProjectEnvKeys;

	FString StripOptionalQuotes(const FString& Value)
	{
		FString TrimmedValue = Value.TrimStartAndEnd();
		if (TrimmedValue.Len() >= 2)
		{
			const TCHAR FirstChar = TrimmedValue[0];
			const TCHAR LastChar = TrimmedValue[TrimmedValue.Len() - 1];
			if ((FirstChar == TCHAR('"') && LastChar == TCHAR('"')) || (FirstChar == TCHAR('\'') && LastChar == TCHAR('\'')))
			{
				TrimmedValue = TrimmedValue.Mid(1, TrimmedValue.Len() - 2);
			}
		}
		return TrimmedValue;
	}

	bool TryParseEnvLine(const FString& Line, FString& OutKey, FString& OutValue)
	{
		FString TrimmedLine = Line.TrimStartAndEnd();
		if (TrimmedLine.IsEmpty() || TrimmedLine.StartsWith(TEXT("#")))
		{
			return false;
		}

		if (TrimmedLine.StartsWith(TEXT("export ")))
		{
			TrimmedLine.RightChopInline(7);
			TrimmedLine = TrimmedLine.TrimStartAndEnd();
		}

		int32 EqualsIndex = INDEX_NONE;
		if (!TrimmedLine.FindChar(TCHAR('='), EqualsIndex) || EqualsIndex <= 0)
		{
			return false;
		}

		OutKey = TrimmedLine.Left(EqualsIndex).TrimStartAndEnd();
		OutValue = StripOptionalQuotes(TrimmedLine.Mid(EqualsIndex + 1));
		return !OutKey.IsEmpty();
	}

	FString GetEnvValue(const FString& VariableName)
	{
		return VariableName.IsEmpty() ? FString() : FPlatformMisc::GetEnvironmentVariable(*VariableName).TrimStartAndEnd();
	}

	void ApplyEnvironmentOverrides(FUnrealAIProviderConfig& Provider)
	{
		const FString BaseUrlOverride = GetEnvValue(Provider.BaseUrlEnvironmentVariable);
		if (!BaseUrlOverride.IsEmpty())
		{
			Provider.BaseUrl = BaseUrlOverride;
		}

		const FString ModelOverride = GetEnvValue(Provider.ModelEnvironmentVariable);
		if (!ModelOverride.IsEmpty())
		{
			Provider.DefaultModel = ModelOverride;
		}
	}
}

UUnrealAISettings::UUnrealAISettings()
{
	if (ProviderProfiles.Num() == 0)
	{
		FUnrealAIProviderConfig OpenAIProvider;
		OpenAIProvider.Name = TEXT("OpenAI");
		OpenAIProvider.BaseUrl = TEXT("https://api.openai.com/v1");
		OpenAIProvider.BaseUrlEnvironmentVariable = TEXT("OPENAI_BASE_URL");
		OpenAIProvider.DefaultModel = TEXT("gpt-5.6-luna");
		OpenAIProvider.ModelEnvironmentVariable = TEXT("OPENAI_MODEL");
		OpenAIProvider.ApiKeyEnvironmentVariable = TEXT("OPENAI_API_KEY");
		OpenAIProvider.TimeoutSeconds = 120.0f;
		ProviderProfiles.Add(OpenAIProvider);

		FUnrealAIProviderConfig XAIProvider;
		XAIProvider.Name = TEXT("XAI");
		XAIProvider.BaseUrl = TEXT("https://api.x.ai/v1");
		XAIProvider.BaseUrlEnvironmentVariable = TEXT("OPENAI_BASE_URL");
		XAIProvider.DefaultModel = TEXT("grok-4.3");
		XAIProvider.ModelEnvironmentVariable = TEXT("OPENAI_MODEL");
		XAIProvider.ApiKeyEnvironmentVariable = TEXT("XAI_API_KEY");
		XAIProvider.TimeoutSeconds = 3600.0f;
		ProviderProfiles.Add(XAIProvider);
	}
}

bool UUnrealAISettings::LoadProjectEnvFile(bool bForceReload, int32& OutVariablesLoaded, FString& OutMessage)
{
	using namespace UnrealAISettingsPrivate;

	if (bProjectEnvLoadAttempted && !bForceReload)
	{
		OutVariablesLoaded = LastProjectEnvVariablesLoaded;
		OutMessage = LastProjectEnvMessage;
		return true;
	}

	bProjectEnvLoadAttempted = true;
	LastProjectEnvVariablesLoaded = 0;

	const FString EnvFilePath = FPaths::Combine(FPaths::ProjectDir(), TEXT(".env"));
	TArray<FString> Lines;
	if (!FFileHelper::LoadFileToStringArray(Lines, *EnvFilePath))
	{
		LastProjectEnvMessage = FString::Printf(TEXT("No project .env file was loaded from %s."), *EnvFilePath);
		OutVariablesLoaded = LastProjectEnvVariablesLoaded;
		OutMessage = LastProjectEnvMessage;
		return false;
	}

	for (const FString& Line : Lines)
	{
		FString Key;
		FString Value;
		if (!TryParseEnvLine(Line, Key, Value))
		{
			continue;
		}

		const FString ExistingValue = FPlatformMisc::GetEnvironmentVariable(*Key);
		const bool bValueOwnedByProjectEnv = LoadedProjectEnvKeys.Contains(Key);
		if (ExistingValue.IsEmpty() || bValueOwnedByProjectEnv)
		{
			FPlatformMisc::SetEnvironmentVar(*Key, *Value);
			LoadedProjectEnvKeys.Add(Key);
			++LastProjectEnvVariablesLoaded;
		}
	}

	LastProjectEnvMessage = FString::Printf(TEXT("Loaded %d project .env variable(s) from %s."), LastProjectEnvVariablesLoaded, *EnvFilePath);
	OutVariablesLoaded = LastProjectEnvVariablesLoaded;
	OutMessage = LastProjectEnvMessage;
	return true;
}

bool UUnrealAISettings::TryGetProviderConfig(FName ProviderName, FUnrealAIProviderConfig& OutProvider) const
{
	int32 VariablesLoaded = 0;
	FString EnvMessage;
	LoadProjectEnvFile(false, VariablesLoaded, EnvMessage);

	const FName ResolvedName = ProviderName.IsNone() ? DefaultProviderName : ProviderName;

	for (const FUnrealAIProviderConfig& Provider : ProviderProfiles)
	{
		if (Provider.Name == ResolvedName)
		{
			OutProvider = Provider;
			UnrealAISettingsPrivate::ApplyEnvironmentOverrides(OutProvider);
			return true;
		}
	}

	return false;
}

#if WITH_EDITOR
FText UUnrealAISettings::GetSectionText() const
{
	return LOCTEXT("UnrealAISettingsSection", "UnrealAI");
}

FText UUnrealAISettings::GetSectionDescription() const
{
	return LOCTEXT("UnrealAISettingsDescription", "Configure OpenAI-compatible model providers such as OpenAI, xAI, OpenRouter, or local gateways.");
}
#endif

#undef LOCTEXT_NAMESPACE

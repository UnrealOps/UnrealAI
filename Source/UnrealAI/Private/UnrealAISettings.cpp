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

	FString GetBaseUrlOverride(const FUnrealAIProviderConfig& Provider)
	{
		if (Provider.Name == TEXT("XAI"))
		{
			const FString XAIOverride = GetEnvValue(TEXT("XAI_BASE_URL"));
			if (!XAIOverride.IsEmpty())
			{
				return XAIOverride;
			}
		}

		const FString ConfiguredOverride = GetEnvValue(Provider.BaseUrlEnvironmentVariable);
		if (!ConfiguredOverride.IsEmpty())
		{
			return ConfiguredOverride;
		}

		return Provider.Name == TEXT("XAI") ? GetEnvValue(TEXT("OPENAI_BASE_URL")) : FString();
	}

	FString GetModelOverride(const FUnrealAIProviderConfig& Provider)
	{
		if (Provider.Name == TEXT("XAI"))
		{
			const FString XAIOverride = GetEnvValue(TEXT("XAI_MODEL"));
			if (!XAIOverride.IsEmpty())
			{
				return XAIOverride;
			}
		}

		const FString ConfiguredOverride = GetEnvValue(Provider.ModelEnvironmentVariable);
		if (!ConfiguredOverride.IsEmpty())
		{
			return ConfiguredOverride;
		}

		return Provider.Name == TEXT("XAI") ? GetEnvValue(TEXT("OPENAI_MODEL")) : FString();
	}

	void ApplyEnvironmentOverrides(FUnrealAIProviderConfig& Provider)
	{
		const FString BaseUrlOverride = GetBaseUrlOverride(Provider);
		if (!BaseUrlOverride.IsEmpty())
		{
			Provider.BaseUrl = BaseUrlOverride;
		}

		const FString ModelOverride = GetModelOverride(Provider);
		if (!ModelOverride.IsEmpty())
		{
			Provider.DefaultModel = ModelOverride;
		}
	}

	bool TryMakeBuiltInProvider(FName ProviderName, FUnrealAIProviderConfig& OutProvider)
	{
		if (ProviderName == TEXT("OpenAI"))
		{
			OutProvider.Name = TEXT("OpenAI");
			OutProvider.Api = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;
			OutProvider.BaseUrl = TEXT("https://api.openai.com/v1");
			OutProvider.BaseUrlEnvironmentVariable = TEXT("OPENAI_BASE_URL");
			OutProvider.DefaultModel = TEXT("gpt-5.6-luna");
			OutProvider.ModelEnvironmentVariable = TEXT("OPENAI_MODEL");
			OutProvider.ApiKeyEnvironmentVariable = TEXT("OPENAI_API_KEY");
			OutProvider.TimeoutSeconds = 120.0f;
			return true;
		}

		if (ProviderName == TEXT("XAI"))
		{
			OutProvider.Name = TEXT("XAI");
			OutProvider.Api = EUnrealAIProviderApi::OpenAICompatibleChatCompletions;
			OutProvider.BaseUrl = TEXT("https://api.x.ai/v1");
			OutProvider.BaseUrlEnvironmentVariable = TEXT("XAI_BASE_URL");
			OutProvider.DefaultModel = TEXT("grok-4.6");
			OutProvider.ModelEnvironmentVariable = TEXT("XAI_MODEL");
			OutProvider.ApiKeyEnvironmentVariable = TEXT("XAI_API_KEY");
			OutProvider.TimeoutSeconds = 3600.0f;
			return true;
		}

		if (ProviderName == TEXT("Anthropic"))
		{
			OutProvider.Name = TEXT("Anthropic");
			OutProvider.Api = EUnrealAIProviderApi::AnthropicMessages;
			OutProvider.BaseUrl = TEXT("https://api.anthropic.com/v1");
			OutProvider.BaseUrlEnvironmentVariable = TEXT("ANTHROPIC_BASE_URL");
			OutProvider.DefaultModel = TEXT("claude-sonnet-5");
			OutProvider.ModelEnvironmentVariable = TEXT("ANTHROPIC_MODEL");
			OutProvider.ApiKeyEnvironmentVariable = TEXT("ANTHROPIC_API_KEY");
			OutProvider.TimeoutSeconds = 120.0f;
			return true;
		}

		if (ProviderName == TEXT("Gemini"))
		{
			OutProvider.Name = TEXT("Gemini");
			OutProvider.Api = EUnrealAIProviderApi::GeminiGenerateContent;
			OutProvider.BaseUrl = TEXT("https://generativelanguage.googleapis.com/v1beta");
			OutProvider.BaseUrlEnvironmentVariable = TEXT("GEMINI_BASE_URL");
			OutProvider.DefaultModel = TEXT("gemini-3.7-flash");
			OutProvider.ModelEnvironmentVariable = TEXT("GEMINI_MODEL");
			OutProvider.ApiKeyEnvironmentVariable = TEXT("GEMINI_API_KEY");
			OutProvider.TimeoutSeconds = 120.0f;
			return true;
		}

		return false;
	}
}

UUnrealAISettings::UUnrealAISettings()
{
	if (ProviderProfiles.Num() == 0)
	{
		const FName BuiltInProviderNames[] = {
			TEXT("OpenAI"),
			TEXT("XAI"),
			TEXT("Anthropic"),
			TEXT("Gemini")};
		for (const FName ProviderName : BuiltInProviderNames)
		{
			FUnrealAIProviderConfig Provider;
			if (UnrealAISettingsPrivate::TryMakeBuiltInProvider(ProviderName, Provider))
			{
				ProviderProfiles.Add(Provider);
			}
		}
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

	if (UnrealAISettingsPrivate::TryMakeBuiltInProvider(ResolvedName, OutProvider))
	{
		UnrealAISettingsPrivate::ApplyEnvironmentOverrides(OutProvider);
		return true;
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
	return LOCTEXT("UnrealAISettingsDescription", "Configure OpenAI-compatible, Anthropic Messages, and Google Gemini model providers.");
}
#endif

#undef LOCTEXT_NAMESPACE

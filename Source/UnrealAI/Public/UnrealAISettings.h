#pragma once

#include "CoreMinimal.h"
#include "Engine/DeveloperSettings.h"
#include "UnrealAITypes.h"
#include "UnrealAISettings.generated.h"

UCLASS(Config = Game, DefaultConfig, meta = (DisplayName = "UnrealAI"))
class UNREALAI_API UUnrealAISettings : public UDeveloperSettings
{
	GENERATED_BODY()

public:
	UUnrealAISettings();

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Providers")
	FName DefaultProviderName = TEXT("OpenAI");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Config, Category = "Providers")
	TArray<FUnrealAIProviderConfig> ProviderProfiles;

	static bool LoadProjectEnvFile(bool bForceReload, int32& OutVariablesLoaded, FString& OutMessage);

	bool TryGetProviderConfig(FName ProviderName, FUnrealAIProviderConfig& OutProvider) const;

#if WITH_EDITOR
	virtual FText GetSectionText() const override;
	virtual FText GetSectionDescription() const override;
#endif
};

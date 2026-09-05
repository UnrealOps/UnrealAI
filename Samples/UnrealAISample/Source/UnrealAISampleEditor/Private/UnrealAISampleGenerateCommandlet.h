#pragma once

#include "CoreMinimal.h"
#include "Commandlets/Commandlet.h"
#include "UnrealAISampleGenerateCommandlet.generated.h"

/** Rebuilds the checked-in Blueprint example from public UnrealAI nodes. */
UCLASS()
class UUnrealAISampleGenerateCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UUnrealAISampleGenerateCommandlet();

	virtual int32 Main(const FString& Params) override;
};

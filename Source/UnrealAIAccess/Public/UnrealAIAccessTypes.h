// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UnrealAIAccessTypes.generated.h"

/** Stable provider-neutral error categories used for policy and retry decisions. */
UENUM(BlueprintType)
enum class EUnrealAIErrorCategory : uint8
{
	None,
	InvalidArgument,
	InvalidConfiguration,
	NotFound,
	NotAuthorized,
	PolicyDenied,
	UnsupportedCapability,
	AuthorityRequired,
	StaleWorldState,
	ResourceConflict,
	Busy,
	RateLimited,
	BudgetExceeded,
	Timeout,
	Cancelled,
	Transport,
	Provider,
	ProviderProtocol,
	SchemaValidation,
	Memory,
	Persistence,
	Audio,
	Vision,
	Gameplay,
	VersionMismatch,
	Internal
};

/** Stable identity of an asynchronous request. */
USTRUCT(BlueprintType)
struct UNREALAIACCESS_API FUnrealAIRequestId
{
	GENERATED_BODY()
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Agent|Identity")
	FGuid Value;
	bool IsValid() const
	{
		return Value.IsValid();
	}
	FString ToString() const
	{
		return Value.ToString(EGuidFormats::DigitsWithHyphensLower);
	}
	friend bool operator==(const FUnrealAIRequestId &A, const FUnrealAIRequestId &B)
	{
		return A.Value == B.Value;
	}
	friend uint32 GetTypeHash(const FUnrealAIRequestId &Id)
	{
		return GetTypeHash(Id.Value);
	}
};

/** Non-authorizing billing identity; independent of any game principal or world. */
struct UNREALAIACCESS_API FUnrealAIBillingPrincipalId
{
	FGuid Value;
	bool IsValid() const
	{
		return Value.IsValid();
	}
	friend uint32 GetTypeHash(const FUnrealAIBillingPrincipalId &Id)
	{
		return GetTypeHash(Id.Value);
	}
	friend bool operator==(const FUnrealAIBillingPrincipalId &A, const FUnrealAIBillingPrincipalId &B)
	{
		return A.Value == B.Value;
	}
};

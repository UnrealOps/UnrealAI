// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIConnectionRegistry.h"
#include "Auth/UnrealAIOAuthCredentialBroker.h"
#include "CoreMinimal.h"
#include "Modules/ModuleInterface.h"
#include "Modules/ModuleManager.h"

#if WITH_EDITOR
/**
 * Unforgeable-by-normal-callers capability issued only by the trusted direct-subscription Editor UI for one explicit
 * local gesture. The provider module accepts interactive operations only when this capability is present.
 */
class FUnrealAIXAIEditorAuthGesture final
{
  private:
	FUnrealAIXAIEditorAuthGesture() = default;
	friend class FUnrealAIDirectSubscriptionEditorGestureAuthority;
};
#endif

/** Experimental Editor/Development-only xAI Grok subscription account service. */
class UNREALAIAUTHXAI_API IUnrealAIAuthXAIModule : public IModuleInterface
{
  public:
	static IUnrealAIAuthXAIModule &Get()
	{
		return FModuleManager::LoadModuleChecked<IUnrealAIAuthXAIModule>(TEXT("UnrealAIAuthXAI"));
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("UnrealAIAuthXAI"));
	}

	virtual bool IsRuntimeReady() const = 0;
	virtual FName GetConnectionAlias() const = 0;
	virtual FName GetAuthProfileId() const = 0;
	virtual FUnrealAIAccessAccountId GetAccountId() const = 0;
	virtual FUnrealAIProviderAccessDescriptor DescribeSubscriptionAccess() const = 0;
	virtual FUnrealAIAccountStatus GetAccountStatus() const = 0;
	virtual TSharedPtr<FUnrealAIOAuthCredentialBroker, ESPMode::ThreadSafe> GetCredentialBroker() const = 0;
	virtual TSharedPtr<const FUnrealAIConnectionRegistrySnapshot, ESPMode::ThreadSafe> GetConnections() const = 0;

#if WITH_EDITOR
	virtual bool StartSignInFromEditorGesture(const FUnrealAIXAIEditorAuthGesture &Gesture,
											  const FUnrealAIInteractiveAuthRequest &Request,
											  TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
											  const FUnrealAICancellationToken &Cancellation,
											  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
											  FUnrealAIProviderAccessError &OutError) = 0;
	virtual bool StartSignOutFromEditorGesture(const FUnrealAIXAIEditorAuthGesture &Gesture,
											   const FUnrealAIAccountAuthRequest &Request,
											   TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
											   const FUnrealAICancellationToken &Cancellation,
											   TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
											   FUnrealAIProviderAccessError &OutError) = 0;
#endif
};

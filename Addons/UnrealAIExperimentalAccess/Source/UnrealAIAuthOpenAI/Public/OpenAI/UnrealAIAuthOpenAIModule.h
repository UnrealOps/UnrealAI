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
class FUnrealAIOpenAIEditorAuthGesture final
{
  private:
	FUnrealAIOpenAIEditorAuthGesture() = default;
	friend class FUnrealAIDirectSubscriptionEditorGestureAuthority;
};
#endif

/** Experimental Editor/Development-only OpenAI Codex subscription account service. */
class UNREALAIAUTHOPENAI_API IUnrealAIAuthOpenAIModule : public IModuleInterface
{
  public:
	static IUnrealAIAuthOpenAIModule &Get()
	{
		return FModuleManager::LoadModuleChecked<IUnrealAIAuthOpenAIModule>(TEXT("UnrealAIAuthOpenAI"));
	}

	static bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded(TEXT("UnrealAIAuthOpenAI"));
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
	virtual bool StartSignInFromEditorGesture(const FUnrealAIOpenAIEditorAuthGesture &Gesture,
											  const FUnrealAIInteractiveAuthRequest &Request,
											  TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
											  const FUnrealAICancellationToken &Cancellation,
											  TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
											  FUnrealAIProviderAccessError &OutError) = 0;
	virtual bool StartSignOutFromEditorGesture(const FUnrealAIOpenAIEditorAuthGesture &Gesture,
											   const FUnrealAIAccountAuthRequest &Request,
											   TSharedRef<IUnrealAIAuthEventSink, ESPMode::ThreadSafe> Sink,
											   const FUnrealAICancellationToken &Cancellation,
											   TSharedPtr<IUnrealAIAuthOperationHandle, ESPMode::ThreadSafe> &OutHandle,
											   FUnrealAIProviderAccessError &OutError) = 0;
#endif
};

// Copyright EngineWorks. All Rights Reserved.
#include "Auth/UnrealAICredentialBrokerFactory.h"
#include "Misc/ScopeLock.h"
namespace
{
FCriticalSection FactoryMutex;
TSharedPtr<IUnrealAICredentialBrokerFactory, ESPMode::ThreadSafe> FactoryInstance;
bool IsStableBindingName(const FName Name)
{
	if (Name.IsNone())
	{
		return false;
	}
	const FString Text = Name.ToString();
	if (Text.IsEmpty() || Text.Len() > FUnrealAICredentialDestination::MaxIdentifierUtf8Bytes)
	{
		return false;
	}
	for (int32 Index = 0; Index < Text.Len(); ++Index)
	{
		const TCHAR Character = Text[Index];
		const bool bAlphaNumeric =
			(Character >= TEXT('a') && Character <= TEXT('z')) || (Character >= TEXT('0') && Character <= TEXT('9'));
		if ((!bAlphaNumeric && Character != TEXT('.') && Character != TEXT('_') && Character != TEXT('-')) ||
															 ((Index == 0 || Index == Text.Len() - 1) &&
															  !bAlphaNumeric))
		{
			return false;
		}
	}
	return true;
}
} // namespace
bool FUnrealAIApiKeyCredentialBinding::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	FString HandleError;
	if (!IsStableBindingName(ConnectionAlias) || !IsStableBindingName(AuthProfileId) || !AccountId.IsValid() ||
		!SecretHandle.ValidateShape(HandleError))
	{
		OutError = TEXT("API-key binding requires a stable exact connection, auth profile, local account ID, and opaque secret handle.");
		return false;
	}
	return true;
}

bool FUnrealAIApiKeyCredentialBrokerConfig::ValidateShape(FString &OutError) const
{
	OutError.Reset();
	if (MaxConcurrentStoreOperations < 1 || MaxConcurrentStoreOperations > HardMaxConcurrentStoreOperations ||
		MaxActiveLeases < 1 || MaxActiveLeases > HardMaxActiveLeases || !FMath::IsFinite(LeaseLifetimeSeconds) ||
		LeaseLifetimeSeconds <= 0.0 || LeaseLifetimeSeconds > FUnrealAICredentialLease::MaxLeaseLifetimeSeconds)
	{
		OutError = TEXT("API-key broker requires positive bounded operation, lease, and lifetime limits.");
		return false;
	}
	return true;
}

TSharedPtr<IUnrealAICredentialBrokerFactory, ESPMode::ThreadSafe> IUnrealAICredentialBrokerFactory::Get()
{
	FScopeLock Lock(&FactoryMutex);
	return FactoryInstance;
}
bool IUnrealAICredentialBrokerFactory::Register(
	TSharedRef<IUnrealAICredentialBrokerFactory, ESPMode::ThreadSafe> Factory)
{
	FScopeLock Lock(&FactoryMutex);
	if (FactoryInstance.IsValid())
	{
		return false;
	}
	FactoryInstance = MoveTemp(Factory);
	return true;
}

// Copyright EngineWorks. All Rights Reserved.

#pragma once
#include "Auth/UnrealAIProviderAccess.h"

/** Process-wide, bounded capability catalog. Contains no credentials or account claims. */
class UNREALAIACCESS_API FUnrealAICredentialStoreRegistry final
{
  public:
	static constexpr int32 MaxCredentialStores = 16;
	static bool RegisterCredentialStoreCapabilities(FName StoreAlias,
													const FUnrealAISecretStoreCapabilities &Capabilities,
													FString &OutError);
	static void
	CopyCredentialStoreCapabilitiesForValidation(TMap<FName, FUnrealAISecretStoreCapabilities> &OutCapabilities);
	static void Seal();
	static bool IsSealed();
};

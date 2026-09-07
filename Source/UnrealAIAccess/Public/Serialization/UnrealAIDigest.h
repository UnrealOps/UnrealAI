// Copyright EngineWorks. All Rights Reserved.
#pragma once
#include "CoreMinimal.h"
namespace UE::UnrealAI::Digest
{
/** Portable SHA-256 for bounded content integrity. Never logs input or digest. */
UNREALAIACCESS_API bool Sha256(TConstArrayView<uint8> Bytes, FString &OutLowerHex);
} // namespace UE::UnrealAI::Digest

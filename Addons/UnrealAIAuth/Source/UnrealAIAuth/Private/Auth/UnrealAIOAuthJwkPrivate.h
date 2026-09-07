// Copyright UnrealOps. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIOAuthStrictJson.h"
#include "CoreMinimal.h"

namespace UE::UnrealAI::Auth::Private
{
/** Converts one already-parsed public JWK into an exact DER SubjectPublicKeyInfo value. */
bool TryEncodeJwkSubjectPublicKeyInfo(const FStrictJsonValue &KeyObject, FName &OutKeyType, FName &OutAlgorithm,
									  TArray<uint8> &OutDer);
} // namespace UE::UnrealAI::Auth::Private

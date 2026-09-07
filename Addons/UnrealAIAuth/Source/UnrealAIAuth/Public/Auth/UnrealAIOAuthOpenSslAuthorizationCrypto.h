// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIOAuthAuthorizationKernel.h"
#include "CoreMinimal.h"

/**
 * Production compact-JWS verifier backed by the engine's reviewed OpenSSL dependency.
 *
 * Supported profiles are RS256, PS256, ES256/P-256, and EdDSA/Ed25519. PublicKeyMaterial must be one exact DER
 * SubjectPublicKeyInfo value produced by the strict JWKS adapter. Unsupported target platforms fail closed.
 */
class UNREALAIAUTH_API FUnrealAIOAuthOpenSslAuthorizationCrypto final : public IUnrealAIOAuthAuthorizationCrypto
{
  public:
	static bool IsPlatformSupported();

	bool GenerateSecureRandomBytes(int32 NumBytes, TArray<uint8> &OutBytes,
								   FUnrealAIProviderAccessError &OutError) override;
	bool Sha256(TConstArrayView<uint8> Input, TArray<uint8> &OutDigest,
				FUnrealAIProviderAccessError &OutError) override;
	bool InspectProtectedHeader(TConstArrayView<uint8> CompactIdToken, FUnrealAIOidcProtectedHeader &OutHeader,
								FUnrealAIProviderAccessError &OutError) override;
	EUnrealAIOidcTokenVerificationResult VerifyAndDecodeIdToken(TConstArrayView<uint8> CompactIdToken,
																const FUnrealAIOidcJsonWebKey &Key,
																FUnrealAIOidcVerifiedClaims &OutClaims,
																FUnrealAIProviderAccessError &OutError) override;
};

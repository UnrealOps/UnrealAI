// Copyright EngineWorks. All Rights Reserved.

#pragma once

#include "Auth/UnrealAIProviderAccess.h"
#include "CoreMinimal.h"

/** Exact non-secret identity bound into one atomic OAuth token record. */
struct UNREALAIAUTH_API FUnrealAIOAuthTokenEnvelopeBinding final
{
	FName ProviderName;
	FName AuthProfileId;
	FUnrealAIAccessAccountId AccountId;

	bool ValidateShape(FString &OutError) const;

	friend bool operator==(const FUnrealAIOAuthTokenEnvelopeBinding &A, const FUnrealAIOAuthTokenEnvelopeBinding &B)
	{
		return A.ProviderName == B.ProviderName && A.AuthProfileId == B.AuthProfileId && A.AccountId == B.AccountId;
	}
};

/**
 * Move-only sensitive values accepted from an authorization or refresh response.
 *
 * AccessToken is required. RefreshToken, IdToken, and AccountRoutingValue are optional at initial authorization. An
 * omitted refresh token retains the envelope's prior refresh token; a present value rotates it. Refresh must preserve
 * the exact presence and bytes of protected account-routing material.
 */
struct UNREALAIAUTH_API FUnrealAIOAuthTokenSet final
{
	static constexpr int32 MaxAccessTokenBytes = 16 * 1024;
	static constexpr int32 MaxRefreshTokenBytes = 16 * 1024;
	static constexpr int32 MaxIdTokenBytes = 16 * 1024;
	static constexpr int32 MaxAccountRoutingBytes = 4 * 1024;

	FUnrealAIOAuthTokenSet() = default;
	FUnrealAIOAuthTokenSet(const FUnrealAIOAuthTokenSet &) = delete;
	FUnrealAIOAuthTokenSet &operator=(const FUnrealAIOAuthTokenSet &) = delete;
	FUnrealAIOAuthTokenSet(FUnrealAIOAuthTokenSet &&) noexcept = default;
	FUnrealAIOAuthTokenSet &operator=(FUnrealAIOAuthTokenSet &&) noexcept = default;
	~FUnrealAIOAuthTokenSet() = default;

	FUnrealAISecretValue AccessToken;
	FUnrealAISecretValue RefreshToken;
	FUnrealAISecretValue IdToken;
	FUnrealAISecretValue AccountRoutingValue;
	/** Provider-reported access-token expiry interpreted as UTC. */
	FDateTime AccessTokenExpiresAtUtc;

	void Reset();
};

/**
 * Decoded move-only token envelope.
 *
 * Token and routing bytes have no plaintext getters. Dispatch atomically takes the access token and optional protected
 * account-routing value as secret values. Refresh request material is minted directly into another secret value.
 */
class UNREALAIAUTH_API FUnrealAIOAuthTokenEnvelope final
{
  public:
	FUnrealAIOAuthTokenEnvelope() = default;
	~FUnrealAIOAuthTokenEnvelope() = default;
	FUnrealAIOAuthTokenEnvelope(const FUnrealAIOAuthTokenEnvelope &) = delete;
	FUnrealAIOAuthTokenEnvelope &operator=(const FUnrealAIOAuthTokenEnvelope &) = delete;
	FUnrealAIOAuthTokenEnvelope(FUnrealAIOAuthTokenEnvelope &&) noexcept = default;
	FUnrealAIOAuthTokenEnvelope &operator=(FUnrealAIOAuthTokenEnvelope &&) noexcept = default;

	bool IsSet() const;
	const FUnrealAIOAuthTokenEnvelopeBinding &GetBinding() const;
	FDateTime GetAccessTokenExpiresAtUtc() const;
	bool HasRefreshToken() const;
	bool HasIdToken() const;
	bool HasAccountRoutingValue() const;
	bool IsExpiredAt(const FDateTime &NowUtc) const;

	/**
	 * Moves the access token and optional account-routing value out exactly once.
	 * Both outputs are reset on failure.
	 */
	bool TryTakeDispatchCredentials(const FDateTime &NowUtc, FUnrealAISecretValue &OutBearerToken,
									FUnrealAISecretValue &OutAccountRoutingValue, FString &OutError);

	/**
	 * Mints application/x-www-form-urlencoded refresh material without exposing the refresh token as plaintext.
	 * The output contains grant_type, refresh_token, and client_id in deterministic order.
	 */
	bool TryMintRefreshRequestBody(FStringView ClientId, FUnrealAISecretValue &OutFormBody, FString &OutError) const;

	/**
	 * Moves the preferred RFC 7009 revocation credential out exactly once. A refresh credential wins when present;
	 * otherwise the access credential is returned. The caller must reset the remainder of this envelope immediately.
	 */
	bool TryTakeRevocationCredential(FUnrealAISecretValue &OutToken, FString &OutError);

	/**
	 * Atomically applies one validated refresh result.
	 * A present refresh token rotates the prior value; omission alone retains the prior value. Protected routing must
	 * be present on both sides with identical bytes, or absent on both sides.
	 */
	bool TryApplyRefresh(FUnrealAIOAuthTokenSet &&RefreshedTokens, const FDateTime &NowUtc, FString &OutError);

	void Reset();

  private:
	FUnrealAIOAuthTokenEnvelopeBinding Binding;
	FUnrealAISecretValue AccessToken;
	FUnrealAISecretValue RefreshToken;
	FUnrealAISecretValue IdToken;
	FUnrealAISecretValue AccountRoutingValue;
	FDateTime AccessTokenExpiresAtUtc;

	friend class FUnrealAIOAuthTokenEnvelopeCodec;
};

/**
 * Versioned binary codec for one atomic secure-store record.
 *
 * Encoding and decoding consume their sensitive input and wipe it on every success or failure path. The wire is
 * deterministic, length-prefixed, checksummed, and never represented as a reflected value or plaintext FString.
 */
class UNREALAIAUTH_API FUnrealAIOAuthTokenEnvelopeCodec final
{
  public:
	static constexpr uint16 CurrentVersion = 1;

	static bool TryCreate(const FUnrealAIOAuthTokenEnvelopeBinding &Binding, FUnrealAIOAuthTokenSet &&Tokens,
						  const FDateTime &NowUtc, FUnrealAIOAuthTokenEnvelope &OutEnvelope, FString &OutError);

	static bool TryEncode(FUnrealAIOAuthTokenEnvelope &&Envelope, const FDateTime &NowUtc,
						  FUnrealAISecretValue &OutEncodedEnvelope, FString &OutError);

	static bool TryDecode(FUnrealAISecretValue &&EncodedEnvelope,
						  const FUnrealAIOAuthTokenEnvelopeBinding &ExpectedBinding, const FDateTime &NowUtc,
						  FUnrealAIOAuthTokenEnvelope &OutEnvelope, FString &OutError);
	/**
	 * Broker recovery decode that admits an expired access token so a still-bound refresh token can rotate it.
	 * Dispatch remains impossible until TryApplyRefresh installs a new unexpired access credential.
	 */
	static bool TryDecodeForRefresh(FUnrealAISecretValue &&EncodedEnvelope,
									const FUnrealAIOAuthTokenEnvelopeBinding &ExpectedBinding, const FDateTime &NowUtc,
									FUnrealAIOAuthTokenEnvelope &OutEnvelope, FString &OutError);

  private:
	static bool TryDecodeInternal(FUnrealAISecretValue &&EncodedEnvelope,
								  const FUnrealAIOAuthTokenEnvelopeBinding &ExpectedBinding, const FDateTime &NowUtc,
								  bool bAllowExpiredAccess, FUnrealAIOAuthTokenEnvelope &OutEnvelope,
								  FString &OutError);
	static bool TryMintRefreshFormBody(const FUnrealAISecretValue &RefreshToken, FStringView ClientId,
									   FUnrealAISecretValue &OutFormBody, FString &OutError);

	friend class FUnrealAIOAuthTokenEnvelope;
};
